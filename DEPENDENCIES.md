<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- SPDX-FileCopyrightText: 2026 Zak Noble-Clarke -->

# Component dependencies

The default `mxguest-agentd` build dynamically links the system C library and compiles the MIT Core byte codecs into the daemon. It links no display, filesystem, D-Bus or systemd client library. Shared folders use the running kernel's FUSE support (`/dev/fuse` and the `fuse` file system type) through its system call interface; no FUSE library or helper program is used. On the distributions selected by the installer, the C library is GNU libc, principally LGPL-2.1-or-later. Its distribution package retains the notices for its separately licensed files. No C library source or binary is bundled here.

| Component | Dependency | Use | Licence |
| --- | --- | --- | --- |
| `mxguest-agentd` | GNU libc supplied by the distribution | C and POSIX process, filesystem and timing interfaces | LGPL-2.1-or-later for the principal implementation |
| `mxguest-agentd` | `mx-guest-core` | Compiled agent byte codecs | MIT |
| `mxguest-agentd` | Kernel FUSE support supplied by the distribution kernel | Shared-folder mounts through `/dev/fuse` and `mount(2)` | GPL-2.0-only for the Linux kernel; nothing is linked or bundled |
| `mxguest-agentd` | Installed `systemctl` | Separate power-command process | LGPL-2.1-or-later for systemd's principal implementation |
| Power compatibility helper | Installed systemd and coreutils | Separate `systemctl`, `loginctl` and `timeout` processes | Distribution packages retain their own licence records; no copies are bundled |

The power helper is optional compatibility for an existing service. Its bind mount applies only inside that service's mount namespace. It routes the two supported power invocations through a bounded system-manager command and resolves unrelated invocations through the system manager's mount view. The installer activates the drop-in at the service's next start.

The GNOME component's interpreted session dependencies are recorded in [`session-bridge/DEPENDENCIES.md`](session-bridge/DEPENDENCIES.md). It adds no library to the daemon's link dependencies. Installer dependencies are distribution build tools and packages; the source media does not ship copies of them.
