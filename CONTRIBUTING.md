<!-- REUSE-IgnoreStart -->
# Contributing to mx-guest-linux-agent

Read [README.md](README.md). Agent feature logic and installation belong here. Device frame encoding belongs in Core and Linux ABI records belong in Linux Common.

- Change dependencies in their owning repositories before updating `deps/core` or `deps/linux-common`. Pin updates identify the old and new commits and their reason.
- Read protocol and ABI constants from those dependencies.
- Update the affected component's dependency record when its linked or runtime dependencies change.
- Discuss new copyleft dependencies, bundling or static linking before adding them. Preserve notices for every shipped dependency.
- Keep advertised capabilities consistent with implemented behavior.

The repository licence is GPL-2.0-only.

## Contributions

Read the [Developer Certificate of Origin 1.1](https://developercertificate.org) before signing off. Every commit requires a `Signed-off-by` trailer matching its author's name and email. Use `git commit -s`; the pull-request DCO workflow checks this requirement.

Write original implementation code. Do not paste or adapt code from other projects; use their supported interfaces. Record any introduced third-party material in `THIRD-PARTY-NOTICES`, retaining its original notices, licence identifier, copyright holders and source location. Discuss material under another licence before adding it.

## File notices and checks

New source files carry this repository's SPDX licence identifier and copyright notice in the file's comment syntax. Preserve existing notices; add a contributor's copyright when appropriate. Files that cannot carry comments are annotated in `REUSE.toml`.

Run the component checks described in [README.md](README.md) and `reuse lint` before submitting. REUSE runs on pushes and pull requests.

<!-- REUSE-IgnoreEnd -->
