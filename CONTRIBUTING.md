<!-- REUSE-IgnoreStart -->
# Contributing to mx-guest-linux-agent

Read [README.md](README.md) first. This repository holds the userspace agent daemon, its clients, and their systemd units, udev rules and install scripts. Agent frame encoding belongs in mx-guest-core, and the local frame protocol between the daemon and its clients belongs in mx-guest-linux-common.

## Working with the submodules

- `deps/core` and `deps/linux-common` are pinned copies of MX's own repositories. Do not edit files under them here. Make the change in the owning repository, then move the pin in this one.
- Move a pin in its own commit, and say in the message which commits it moves between and why.
- Do not declare protocol or ABI version constants in this repository. Read them from Core and from linux-common.

## Link dependencies

- Any commit that adds, removes or changes a library a component links updates that component's link-dependency list in the same commit, with the SPDX licence identifier of each library.
- Do not add a dependency on a copyleft or weak-copyleft library without raising it first. Whether a feature is worth that licence, or should instead invoke a separate program or be dropped, is the repository owner's decision.
- Do not vendor, bundle or statically link a third-party library without raising it first. A copy that ships creates a notice obligation immediately and needs a `THIRD-PARTY-NOTICES` entry in the same commit.

## Developer Certificate of Origin

Contributions are accepted under the Developer Certificate of Origin, version 1.1: https://developercertificate.org

Read the full text before signing off. Adding a sign-off to a commit is your certification of that text for that commit.

Sign off every commit with:

```
git commit -s
```

This appends a trailer of exactly this form to the commit message:

```
Signed-off-by: Name <email>
```

The name and email in the trailer must match the commit's author name and author email exactly, including case. The DCO check (`.github/workflows/dco.yml`) runs on every pull request, examines every non-merge commit in it, and fails the pull request if any commit lacks a `Signed-off-by` trailer equal to that commit's `Author Name <author email>`. The check runs only on pull requests. Maintainers who push directly to a branch must still sign off every commit; the requirement is the same whether or not the check runs.

`git commit -s` writes the trailer from your configured `user.name` and `user.email`. If the commit's author is someone else, for example when you commit a change on another person's behalf, the author must add their own sign-off. To add missing sign-offs to your own commits on a branch, use `git rebase --signoff <base>` or, for the last commit only, `git commit --amend -s --no-edit`, then force-push the branch.

## Licence headers

Every new source file carries a two-line SPDX header as its first lines. For C sources and headers:

```c
/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
```

For files that use `#` comments, such as build files, shell scripts, systemd units and udev rules:

```
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
```

If you hold copyright in your contribution to a file, add your own `SPDX-FileCopyrightText: <year> <name>` line below the existing ones. Never remove or alter an existing copyright or licence line.

Documentation and repository metadata that cannot carry a header are listed in `REUSE.toml`. Do not add a new header-less file without adding it there, and do not use `REUSE.toml` to avoid putting a header on a source file.

## REUSE compliance

`reuse lint` must pass. It runs in CI (`.github/workflows/reuse.yml`) on every push and pull request. Run it locally before pushing; the tool is described at https://reuse.software.

## Do not copy code from other projects

Write the code yourself. Do not paste or adapt code from other clipboard tools, filesystem clients, desktop utilities or any other project, even where the licence would appear to permit it. Use a library through its interface, recorded in the link-dependency list; do not copy its implementation.

The reason is provenance. Every file here is published as MX's own GPL-2.0-only work, and its copyright line says who wrote it. Code pasted from elsewhere carries someone else's copyright and possibly licence terms that conflict with GPL-2.0-only, and once released it is hard to withdraw. A similarity gate that scans changes against a corpus of plausible third-party sources is planned for MX's MIT protocol code; nothing of that kind runs here, so the rule depends on contributors keeping it.

## Material under another licence

No material under a licence other than GPL-2.0-only may enter this repository without an entry in `THIRD-PARTY-NOTICES` in the same commit, naming the work, its SPDX identifier, its copyright holders and its source location, with the original notices kept intact. MIT files arriving through the `deps/` submodules are MX's own and are already recorded there. Raise any other case before opening a pull request.

<!-- REUSE-IgnoreEnd -->
