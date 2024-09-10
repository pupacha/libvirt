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

#include "ch_events.h"
#include "virfile.h"
#include "virlog.h"

VIR_LOG_INIT("ch.ch_events");

static void virCHEventMonitorLoop(void *data)
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

    /*
     * We would need to wait until VM is initialized.
     */
    while (!(vm = virObjectRef(mon->vm)))
        g_usleep(100000);   // 100 milli seconds

    while (g_atomic_int_get(&mon->event_loop_stop) == 0) {
        VIR_DEBUG("Reading events from monitor..");
        /* Read and process events here */
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
                            virCHEventMonitorLoop,
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
