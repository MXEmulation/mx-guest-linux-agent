<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- SPDX-FileCopyrightText: 2026 Zak Noble-Clarke -->

# GNOME session bridge

A GNOME Shell 50 extension exposing native Wayland and X11 window inventory and installed application discovery through a local session D-Bus API. It can activate and close windows and launch installed applications. It does not capture window content or route these operations through the guest-agent daemon, so host integration is incomplete.

## API

The service is `org.mx.GuestSession`, object `/org/mx/GuestSession`, interface `org.mx.GuestSession1`. Methods return JSON strings.

| Method | Result |
| --- | --- |
| `ListWindows()` | Session UUID, window generation, monitors and managed application windows |
| `ListApplications()` | Session UUID, catalogue generation and installed application records |
| `Activate(session, generation, windowId)` | Requests activation of the current native window |
| `Close(session, generation, windowId)` | Requests closing of the current native window |
| `Launch(session, generation, desktopId)` | Requests Gio launch of an application in the current catalogue |

Actions refresh their inventory and reject stale sessions, generations or missing targets with D-Bus errors. An accepted request does not guarantee that the application has finished focusing, closing or opening. Window IDs are positive decimal strings and are not reused within a bridge session; restarting or re-enabling the bridge creates a new session UUID.

Window metadata includes application identity, process, workspace, visibility, stacking, rectangles and available texture dimensions. Rectangles use Mutter stage coordinates, including negative origins; texture dimensions use texture pixels. Monitor recognition follows real DRM/sysfs ancestry and checks PCI identities extracted from Core. Ambiguous connectors are not labelled as MXGPU outputs.

The catalogue filters hidden and desktop-excluded entries, reports actual icon descriptors and launches only installed desktop IDs. A digest of each desktop entry detects launch-command and D-Bus activation changes. `can_launch` indicates that Gio can attempt a launch, not that the application will initialize successfully.

`WindowInventoryChanged(session, generation)` and `ApplicationCatalogueChanged(session, generation)` notify clients to refresh. Window notifications are coalesced through one idle source without frame or pointer polling. Direct list and action calls refresh immediately.

## Build and checks

From the agent repository:

```sh
python3 session-bridge/build-package.py --core deps/core --output build/session-bridge
```

The builder stages the extension and licence, generates PCI identity from Core and writes source/artifact digests. It does not install or enable the extension.

[`tests/`](tests/) covers identity, generations, actions, sysfs ancestry, D-Bus marshalling and installed desktop entries. D-Bus tests run on an isolated session bus; use `GIO_USE_VFS=local` to avoid unrelated filesystem service activation. Tests do not enable the extension or restart GNOME.

GPL-2.0-only. See [LICENCE](../LICENCE); the staged package includes its licence. Runtime and build prerequisites are recorded in [DEPENDENCIES.md](DEPENDENCIES.md); component notices are in [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES).
