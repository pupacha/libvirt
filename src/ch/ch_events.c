/*
 * Copyright Microsoft Corp. 2024
 *
 * ch_capabilities.h: CH capabilities
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <fcntl.h>

#include "ch_domain.h"
#include "ch_events.h"
#include "ch_process.h"
#include "virfile.h"
#include "virjson.h"
#include "virlog.h"

VIR_LOG_INIT("ch.ch_events");

VIR_ENUM_IMPL(virCHEvent,
              virCHEventLast,
              "vmm:starting",
              "vmm:shutdown",
              "vm:booting",
              "vm:booted",
              "vm:pausing",
              "vm:paused",
              "vm:resuming",
              "vm:resumed",
              "vm:snapshotting",
              "vm:snapshotted",
              "vm:restoring",
              "vm:restored",
              "vm:resizing",
              "vm:resized",
              "vm:shutdown",
              "vm:deleted",
              "cpu_manager:create_vcpu",
              "virtio-device:activated",
              "virtio-device:reset"
);

static int chEventsProcessStop(virDomainObj *vm,
                                   virDomainShutoffReason reason)
{
    virCHDriver *driver =  ((virCHDomainObjPrivate *)vm->privateData)->driver;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY))
        return -1;
    virCHProcessStop(driver, vm, reason);
    virDomainObjEndJob(vm);

    return 0;
}

static int chProcessEvent(virCHMonitor *mon,
                           virJSONValue *eventJSON)
{
    const char *event;
    const char *source;
    virCHEvent ev;
    g_autofree char *timestamp = NULL;
    g_autofree char *full_event = NULL;
    virDomainObj *vm = mon->vm;
    int ret = 0;

    if (virJSONValueObjectHasKey(eventJSON, "source") == 0) {
        VIR_WARN("Invalid JSON from monitor, no source key");
        return -1;
    }
    if (virJSONValueObjectHasKey(eventJSON, "event") == 0) {
        VIR_WARN("Invalid JSON from monitor, no event key");
        return -1;
    }
    source = virJSONValueObjectGetString(eventJSON, "source");
    event = virJSONValueObjectGetString(eventJSON, "event");
    full_event = g_strdup_printf("%s:%s", source, event);
    ev = virCHEventTypeFromString(full_event);
    VIR_DEBUG("Source: %s Event: %s, ev: %d", source, event, ev);

    switch (ev) {
        case virCHEventVmmStarting:
        case virCHEventVmBooting:
        case virCHEventVmBooted:
        case virCHEventVmPausing:
        case virCHEventVmPaused:
        case virCHEventVmResuming:
        case virCHEventVmResumed:
        case virCHEventVmSnapshotting:
        case virCHEventVmSnapshotted:
        case virCHEventVmRestoring:
        case virCHEventVmRestored:
        case virCHEventVmResizing:
        case virCHEventVmResized:
            break;
        case virCHEventVmmShutdown:
            virObjectLock(vm);
            if (chEventsProcessStop(vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN)) {
                VIR_WARN("Failed to mark the VM(%s) as SHUTDOWN!",
                        vm->def->name);
                ret = -1;
            }
            virObjectUnlock(vm);
            break;
        case virCHEventVmShutdown:
            virObjectLock(vm);
            virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_SHUTDOWN);
            virObjectUnlock(vm);
            break;
        case virCHEventVmDeleted:
        case virCHEventCpuCreateVcpu:
        case virCHEventVirtioDeviceActivated:
        case virCHEventVirtioDeviceReset:
        case virCHEventLast:
        default:
            break;
    }

    return ret;
}

static int chProcessEvents(virCHMonitor *mon)
{
    char *buf = mon->event_buffer.buffer;
    ssize_t sz = mon->event_buffer.buf_fill_sz;
    virJSONValue *obj = NULL;
    int blocks = 0;
    size_t i = 0;
    char *json_start;
    ssize_t start_index = -1;
    ssize_t end_index = -1;
    char tmp;
    int ret = 0;

    while (i < sz) {
        if (buf[i] == '{') {
            blocks++;
            if (blocks == 1)
                start_index = i;
        } else if (buf[i] == '}' && blocks > 0) {
            blocks--;
            if (blocks == 0) {
                // valid json document
                end_index = i;

                /*
                * We may hit a corner case where a valid JSON
                * doc happens to end right at the end of the buffer.
                * virJSONValueFromString needs '\0' end the JSON doc.
                * So we need to adjust the buffer accordingly.
                */
                if (end_index == CH_EVENT_BUFFER_SZ - 1) {
                    if (start_index == 0) {
                        /*
                        * We have a valid JSON doc same as the buffer
                        * size. As per protocol, max JSON doc should be
                        * less than the buffer size. So this is an error.
                        * Ignore this JSON doc.
                        */
                        VIR_WARN("Invalid JSON doc size. Expected <= %d",
                                 CH_EVENT_BUFFER_SZ);
                        start_index = -1;
                        break;
                    }

                    /*
                    * Move the valid JSON doc to the start of the buffer so
                    * that we can safely fit a '\0' at the end.
                    */
                    memmove(buf, buf+start_index, end_index-start_index+1);
                    end_index -= start_index;
                    start_index = 0;
                }

                // temporarily null terminate the JSON doc
                tmp = buf[end_index + 1];
                buf[end_index + 1] = '\0';
                json_start = buf + start_index;

                if ((obj = virJSONValueFromString(json_start))) {
                    if (chProcessEvent(mon, obj) < 0) {
                        VIR_WARN("Failed to process JSON event doc: %s", json_start);
                        ret = -1;
                    }
                    virJSONValueFree(obj);
                } else {
                    VIR_WARN("Invalid JSON event doc: %s", json_start);
                    ret = -1;
                }

                // replace the original character
                buf[end_index + 1] = tmp;
                start_index = -1;
            }
        }

        i++;
    }

    if (start_index == -1) {
        // We have processed all the JSON docs in the buffer.
        mon->event_buffer.buf_fill_sz = 0;
    } else if (start_index > 0) {
        // We have an incomplete JSON doc at the end of the buffer.
        // Move it to the start of the buffer.
        mon->event_buffer.buf_fill_sz = sz - start_index;
        memmove(buf, buf+start_index, mon->event_buffer.buf_fill_sz);
    }

    return ret;
}

static void chReadProcessEvents(virCHMonitor *mon,
                               int monitor_fd)
{
    size_t max_sz = CH_EVENT_BUFFER_SZ;
    char *buf = mon->event_buffer.buffer;
    virDomainObj *vm = mon->vm;
    bool incomplete = false;
    size_t sz = 0;

    memset(buf, 0, max_sz);
    do {
        ssize_t ret;

        ret = read(monitor_fd, buf + sz, max_sz - sz);
        if (ret == 0 || (ret < 0 && errno == EINTR)) {
            g_usleep(G_USEC_PER_SEC);
            continue;
        } else if (ret < 0) {
            /*
             * We should never reach here. read(2) says possible errors
             * are EINTR, EAGAIN, EBADF, EFAULT, EINVAL, EIO, EISDIR
             * We handle EINTR gracefully. There is some serious issue
             * if we encounter any of the other errors(either in our code
             * or in the system). Better to bail out.
             */
            VIR_ERROR(_("Failed to read monitor events!: %1$s"), g_strerror(errno));
            VIR_FORCE_CLOSE(monitor_fd);
            abort();
        }

        sz += ret;
        mon->event_buffer.buf_fill_sz = sz;

        chProcessEvents(mon);

        if (mon->event_buffer.buf_fill_sz != 0)
            incomplete = true;
        else
            incomplete = false;

        sz = mon->event_buffer.buf_fill_sz;

    } while (virDomainObjIsActive(vm) && (sz < max_sz) && incomplete);

    return;
}

static void chEventMonitorLoop(void *data)
{
    virCHMonitor *mon = data;
    virDomainObj *vm = NULL;
    int monitor_fd;

    VIR_DEBUG("Monitor event loop thread starting");

    while ((monitor_fd = open(mon->monitorpath, O_RDONLY)) < 0) {
        if (errno == EINTR) {
            g_usleep(100000); // 100 milli seconds
            continue;
        }
        /*
         * Any other error should be a BUG(kernel/libc/libvirtd)
         * (ENOMEM can happen on exceeding per-user limits)
         */
        VIR_ERROR(_("Failed to open the monitor FIFO(%1$s) read end!"),
                  mon->monitorpath);
        abort();
    }
    VIR_DEBUG("Opened the monitor FIFO(%s)", mon->monitorpath);

    mon->event_buffer.buffer = g_malloc_n(sizeof(char), CH_EVENT_BUFFER_SZ);
    mon->event_buffer.buf_fill_sz = 0;

    /*
     * We would need to wait until VM is initialized.
     */
    while (!(vm = virObjectRef(mon->vm)))
        g_usleep(100000);   // 100 milli seconds

    while (g_atomic_int_get(&mon->event_loop_stop) == 0) {
        VIR_DEBUG("Reading events from monitor..");
        chReadProcessEvents(mon, monitor_fd);
    }

    VIR_FORCE_CLOSE(monitor_fd);
    virObjectUnref(vm);

    VIR_DEBUG("Monitor event loop thread exiting");
    return;
}

int virCHStartEventMonitorLoop(virCHMonitor *mon)
{
    g_autofree char *name = NULL;
    name = g_strdup_printf("mon-events-%d", mon->pid);

    virObjectRef(mon);
    if (virThreadCreateFull(&mon->event_loop_thread,
                            false,
                            chEventMonitorLoop,
                            name,
                            false,
                            mon) < 0) {
        virObjectUnref(mon);
        return -1;
    }
    virObjectUnref(mon);

    g_atomic_int_set(&mon->event_loop_stop, 0);
    return 0;
}

void virCHStopEventMonitorLoop(virCHMonitor *mon)
{
    g_atomic_int_set(&mon->event_loop_stop, 1);
}
