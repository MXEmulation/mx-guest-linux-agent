<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- SPDX-FileCopyrightText: 2026 Zak Noble-Clarke -->

# GNOME session bridge

A GNOME Shell 50 extension exposing native Wayland and X11 window inventory and installed application discovery through a local session D-Bus API. It can activate and close windows and launch installed applications. It also reports its window inventory to the guest-agent daemon and executes the host's activate and close requests received from it, as described under Daemon socket. It does not capture window content.

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

Window metadata includes application identity, process, workspace, visibility, stacking, rectangles and available texture dimensions; the listing also names the active workspace. Rectangles use Mutter stage coordinates, including negative origins; texture dimensions use texture pixels. Monitor recognition follows real DRM/sysfs ancestry and checks PCI identities extracted from Core. Ambiguous connectors are not labelled as MXGPU outputs.

The catalogue filters hidden and desktop-excluded entries, reports actual icon descriptors and launches only installed desktop IDs. A digest of each desktop entry detects launch-command and D-Bus activation changes. `can_launch` indicates that Gio can attempt a launch, not that the application will initialize successfully.

`WindowInventoryChanged(session, generation)` and `ApplicationCatalogueChanged(session, generation)` notify clients to refresh. Window notifications are coalesced through one idle source without frame or pointer polling. Direct list and action calls refresh immediately.

## Daemon socket

The extension talks to the guest-agent daemon over the Unix stream socket `/run/mxguest-agent/session.sock`. Each message is a 4-byte little-endian length N followed by N bytes: a one-byte type and its payload. Connection and I/O are asynchronous; a failed or closed connection is retried with a delay growing from 1 s to 30 s. The daemon keeps a single client, so a newer connection replaces an older one. A message with a zero or over-long length, an unexpected type or invalid contents closes the connection.

| Type | Direction | Payload |
| --- | --- | --- |
| 1 | both | Clipboard text, UTF-8, at most 1048536 bytes; a zero-length text is an empty clipboard |
| 2 | bridge to daemon | Window inventory, at most 1048552 bytes |
| 3 | daemon to bridge | Window action, 20 bytes: u16 version 1, u16 action (1 activate, 2 close), u64 generation, u64 window ID, little-endian |

The bridge sends the clipboard text when the compositor reports a new owner (bursts coalesced, texts the bridge itself applied are not echoed) and once after each connection, and sets the clipboard when the daemon sends text. Only text is relayed.

The inventory has the layout of the MXGA integration status payload (version 4), carrying only the flags the bridge can vouch for: desktop bridge ready, window inventory ready, and icon resolution ready (set while icon loading works). The daemon adds the MXGPU presence and driver flags. Its generation is the inventory generation. It lists mapped, unminimized windows on the active workspace that have pixels on a monitor recognized as MXGPU, ordered by window ID, with title and application ID cut at 4096 bytes, an application icon as premultiplied BGRA scaled to fit 64 by 64 pixels (loaded through the Shell icon theme, falling back to the installed icon directories; windows with no resolvable icon carry none, and icons are dropped last-first when the frame limit would be exceeded) and at most 128 windows. Scanout IDs number the MXGPU monitors by Mutter monitor index. Rectangles are in the monitor's pixels, relative to its origin. A window wholly on one monitor has one segment and the reliable-geometry flag. A window that spans several MXGPU monitors of one scale and is fully covered by them has one segment per monitor with destination rectangles within the composed window, largest first. Any other window is clipped to the monitor holding most of it and loses the reliable-geometry flag. The inventory is sent after each connection and whenever its content changes, coalesced over 100 ms; a change that only advances the generation is not sent.

On an action the bridge checks that the generation is not newer than the last one sent and that the window is in that inventory, then activates or closes the current window with that ID. The daemon sends no reply because the host expects none.

## Build and checks

From the agent repository:

```sh
python3 session-bridge/build-package.py --core deps/core --output build/session-bridge
```

The builder stages the extension and licence, generates PCI identity from Core and writes source/artifact digests. It does not install or enable the extension.

[`tests/`](tests/) covers identity, generations, actions, sysfs ancestry, D-Bus marshalling, installed desktop entries, the daemon socket framing and the window inventory encoding and placement. D-Bus tests run on an isolated session bus; use `GIO_USE_VFS=local` to avoid unrelated filesystem service activation. Tests do not enable the extension or restart GNOME.

GPL-2.0-only. See [LICENCE](../LICENCE); the staged package includes its licence. Runtime and build prerequisites are recorded in [DEPENDENCIES.md](DEPENDENCIES.md); component notices are in [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES).
