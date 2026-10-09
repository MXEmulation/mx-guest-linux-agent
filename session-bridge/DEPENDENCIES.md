<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- SPDX-FileCopyrightText: 2026 Zak Noble-Clarke -->

# Session bridge dependencies

The extension is JavaScript interpreted by the already installed GNOME Shell. It adds no executable linked against a third-party library and bundles no third-party implementation, library, icon or desktop entry. The running GNOME session is an explicit prerequisite; no GNOME runtime copy is placed in this repository or the staged extension package.

| Runtime prerequisite | Interface used | SPDX identifier of the relevant upstream code |
| --- | --- | --- |
| GNOME Shell 50 | Extension lifecycle, Shell global, native app tracking and app launch context | GPL-2.0-or-later |
| Mutter supplied with GNOME 50 | Meta window, monitor and existing compositor texture metadata | GPL-2.0-or-later for the Meta compositor code; the installed Mutter package contains additional separately licensed work |
| GJS supplied with GNOME | JavaScript modules and existing GObject introspection bindings | MIT OR LGPL-2.0-or-later for GJS's principal implementation |
| GNOME Shell 50 St toolkit | Icon theme lookup of application icon files | GPL-2.0-or-later |
| GdkPixbuf supplied with GNOME | Loading and scaling application icon files to pixels | LGPL-2.1-or-later |
| GLib, GObject, GIO and GIO Unix supplied with GNOME | Session D-Bus, filesystem attributes, UUIDs, notifications, desktop catalogue and launch | LGPL-2.1-or-later |

These are existing session-runtime dependencies, not newly linked or shipped libraries. Their own transitive dependencies remain owned by the guest distribution. Runtime imports use their documented interfaces and no installed extension implementation is copied.

The package builder uses Python 3 and a C compiler as build inputs. The Core identity header is MX's MIT source, already recorded in the repository's MX dependency notices. The generated identity file carries this component's GPL-2.0-only header. Python and the compiler are not bundled. Independent tests additionally invoke the installed `gjs` and `dbus-run-session` programs; the latter starts an isolated test bus, not a guest-agent or desktop service.
