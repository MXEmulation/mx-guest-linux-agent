<!-- REUSE-IgnoreStart -->
# mx-guest-linux-agent

The Linux MX guest-agent daemon, GNOME session bridge and source installer.

`mxguest-agentd` sends HELLO, HEARTBEAT and system statistics and handles shutdown and restart requests. Power results report the actual bounded system-manager invocation and preserve the request sequence. The daemon advertises only implemented capabilities. Clipboard sharing is routed through a local stream socket at `/run/mxguest-agent/session.sock`, which accepts one client at a time: root, or the active seat user from `ACTIVE_UID=` in `/run/systemd/seats/seat0`. Messages in both directions are a 4-byte little-endian length followed by UTF-8 text. The clipboard capabilities are advertised only while a client is connected, the latest host write is delivered when a client connects unless the guest has published newer text, and text the host wrote is not published back. Shared folders, other local-client routing and host application-window content routing are not implemented.

The [GNOME session bridge](session-bridge/README.md) provides local window inventory, activation and closing, installed application discovery and launch. It does not supply the content or daemon route required for host integration.

## Build and checks

A C11 compiler and Make are required. Initialize the pinned protocol and ABI dependencies first:

```sh
git submodule update --init --recursive
make
make check
```

The daemon is built as `build/mxguest-agentd`. Its runtime transport is `/dev/mxguest-agent`; this repository does not provide the kernel driver for that device. The checks exercise collected statistics, frame encoding, bounded power commands and daemon initialization without performing real power actions.

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

An existing agent binary and service are retained. A loaded `mxguest-agent.service` receives a transactional power compatibility drop-in for its next start, without restarting the agent. Fresh installations use the daemon's implemented features and condition the service on the device node. The installer tests cover manifest refusals, rollback, retained-service compatibility and verified source-cache reuse.

## Licence

GPL-2.0-only. See [LICENCE](LICENCE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Compiled MIT dependencies retain their own licence notices. Contribution requirements are in [CONTRIBUTING.md](CONTRIBUTING.md).

<!-- REUSE-IgnoreEnd -->
