<!-- REUSE-IgnoreStart -->
# mx-guest-linux-agent

The Linux MX guest-agent daemon, GNOME session bridge and source installer.

`mxguest-agentd` sends HELLO, HEARTBEAT and system statistics and handles shutdown and restart requests. Power results report the actual bounded system-manager invocation and preserve the request sequence. The daemon advertises only implemented capabilities. Clipboard sharing and host window integration are routed through a local stream socket at `/run/mxguest-agent/session.sock`, which accepts one client at a time: root, or the active seat user from `ACTIVE_UID=` in `/run/systemd/seats/seat0`. Each message in either direction is a 4-byte little-endian length N followed by N bytes: a one-byte type and its payload. Type 1 is clipboard text (UTF-8, at most 1048536 bytes) in both directions. Type 2, from the client, is a window inventory laid out as an MXGA integration status payload that carries only the bridge flags; the daemon validates it with the Core decoder, adds the MXGPU presence and driver flags read from `/sys/bus/pci/devices` (the PCI identity from Core, bound to the `mxgpu` driver) and publishes it as INTEGRATION_STATUS, re-encoded by Core and re-sent when those flags change. Type 3, from the daemon, is a 20-byte INTEGRATION_WINDOW_ACTION payload (activate or close), forwarded only when it names a window of the published inventory; the host expects no reply. A malformed or oversized message from the client closes the connection. The clipboard and integration capabilities are advertised only while a client is connected, the latest host write is delivered when a client connects unless the guest has published newer text, and text the host wrote is not published back. Shared folders and other local-client routing are not implemented.

The [GNOME session bridge](session-bridge/README.md) provides the window inventory, activation and closing, application icons and installed application discovery and launch, and carries them to the daemon over that socket.

## Build and checks

A C11 compiler and Make are required. Initialize the pinned protocol and ABI dependencies first:

```sh
git submodule update --init --recursive
make
make check
```

The daemon is built as `build/mxguest-agentd`. Its runtime transport is `/dev/mxguest-agent`; this repository does not provide the kernel driver for that device. The checks exercise collected statistics, frame encoding, bounded power commands, the local message framing, inventory validation, status flags and action forwarding, and daemon initialization without performing real power actions.

`deps/core` pins [mx-guest-core](https://github.com/MXEmulation/mx-guest-core), and `deps/linux-common` pins [mx-guest-linux-common](https://github.com/MXEmulation/mx-guest-linux-common). The default build compiles Core's sources; `CORE` can select a development checkout. Runtime and link dependencies are recorded in [DEPENDENCIES.md](DEPENDENCIES.md).

## Source installation

[`install/install.sh`](install/install.sh) consumes the exact public source manifest supplied on installation media. By default it reads `install/sources.tsv`; `--sources FILE` selects another manifest.

```sh
bash install/install.sh --plan
bash install/install.sh --prepare-source
bash install/install.sh --build-only
bash install/test-install.sh
```

`--plan` displays sources and dependencies. `--prepare-source` verifies and checks out the pins. `--build-only` runs checks and builds without changing installed drivers, services or graphics configuration; its dependencies must already be installed. Normal installation obtains dependencies, builds the selected sources and installs the graphics stack for the next boot. It does not restart the running driver or desktop.

The installer supports apt-get, dnf and pacman. Mesa requires Python 3.10 or newer; module installation requires Linux 6.6 or newer on x86_64 or AArch64. Full installation on every supported distribution has not been validated. Package downloads and builds are bounded, and installation failures restore saved configuration.

The installer first uninstalls the previously installed guest additions (agent services, user services, helpers, transport and earlier MXGPU releases), with rollback on failure; `--plan` lists what it finds. Running services are not stopped and loaded modules are not unloaded, so activation is at next boot. It then installs the daemon's implemented features as a service conditioned on the device node, registers the `mxguest` transport module (providing `/dev/mxguest-agent`) beside `mxgpu`, and, when GNOME Shell is installed, installs the session bridge extension with an XDG autostart entry that enables it, which carries clipboard sharing and window integration. The installer tests cover manifest refusals, rollback, uninstall footprint scanning and removal, and verified source-cache reuse.

## Licence

GPL-2.0-only. See [LICENCE](LICENCE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Compiled MIT dependencies retain their own licence notices. Contribution requirements are in [CONTRIBUTING.md](CONTRIBUTING.md).

<!-- REUSE-IgnoreEnd -->
