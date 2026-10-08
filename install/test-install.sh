#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
set -Eeuo pipefail
mx_test_script=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mx_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/mx-bootstrap-check-XXXXXXXX")
trap 'rm -rf -- "$mx_test_dir"' EXIT
mx_test_pin=0123456789abcdef0123456789abcdef01234567
mx_test_manifest="$mx_test_dir/sources.tsv"
{
    printf 'MX_SOURCE_MANIFEST\t1\n'
    printf 'COMPONENT\tcore\thttps://github.com/MXEmulation/mx-guest-core.git\t%s\tMIT\n' "$mx_test_pin"
    printf 'COMPONENT\tcommon\thttps://github.com/MXEmulation/mx-guest-linux-common.git\t%s\tMIT\n' "$mx_test_pin"
    printf 'COMPONENT\tkernel\thttps://github.com/MXEmulation/mx-guest-linux-kernel.git\t%s\tGPL-2.0-only\n' "$mx_test_pin"
    printf 'COMPONENT\tagent\thttps://github.com/MXEmulation/mx-guest-linux-agent.git\t%s\tGPL-2.0-only\n' "$mx_test_pin"
    printf 'COMPONENT\tmesa\thttps://github.com/MXEmulation/mesa-mxgpu.git\t%s\tLicenseRef-Mesa-Multi-License\n' "$mx_test_pin"
    printf 'PCI\tmxgpu\t1234\t5678\n'
} > "$mx_test_manifest"
bash "$mx_test_script/install.sh" --sources "$mx_test_manifest" --validate > /dev/null
for mx_test_case in url pin licence identity duplicate; do
    case $mx_test_case in
        url) sed 's,https://github.com/MXEmulation/mx-guest-core.git,https://example.invalid/source,' "$mx_test_manifest" ;;
        pin) sed "s/$mx_test_pin/HEAD/" "$mx_test_manifest" ;;
        licence) sed 's/\tMIT$/\t/' "$mx_test_manifest" ;;
        identity) sed 's/PCI\tmxgpu\t1234\t5678/PCI\tmxgpu\t65536\t5678/' "$mx_test_manifest" ;;
        duplicate) cat "$mx_test_manifest"; printf 'PCI\tmxgpu\t1234\t5678\n' ;;
    esac > "$mx_test_dir/invalid.tsv"
    if bash "$mx_test_script/install.sh" --sources "$mx_test_dir/invalid.tsv" --validate > /dev/null 2>&1; then
        printf 'Invalid manifest accepted: %s\n' "$mx_test_case" >&2
        exit 1
    fi
done
source "$mx_test_script/install-common.sh"
mx_backup_dir="$mx_test_dir/backup"
mkdir "$mx_backup_dir"
printf original > "$mx_test_dir/existing"
chmod 640 "$mx_test_dir/existing"
ln -s old-release "$mx_test_dir/current"
printf replacement > "$mx_test_dir/input"
mx_replace_file "$mx_test_dir/existing" "$mx_test_dir/input" 600
mx_replace_link "$mx_test_dir/current" new-release
mx_replace_file "$mx_test_dir/fresh" "$mx_test_dir/input"
[[ $(cat "$mx_test_dir/existing") == replacement && $(readlink "$mx_test_dir/current") == new-release ]]
mx_restore_files
[[ $(cat "$mx_test_dir/existing") == original && $(stat -c %a "$mx_test_dir/existing") == 640 && $(readlink "$mx_test_dir/current") == old-release && ! -e $mx_test_dir/fresh ]]
mkdir "$mx_test_dir/directory"
printf working > "$mx_test_dir/directory/keep"
if mx_replace_file "$mx_test_dir/directory" "$mx_test_dir/input" 2>/dev/null; then
    printf '%s\n' 'A directory collision was accepted.' >&2
    exit 1
fi
[[ $(cat "$mx_test_dir/directory/keep") == working ]]
mx_transaction_paths=()
mx_transaction_backups=()
mx_backup_dir="$mx_test_dir/removal-backup"
mkdir "$mx_backup_dir"
mkdir "$mx_test_dir/removal"
printf file-content > "$mx_test_dir/removal/file"
chmod 640 "$mx_test_dir/removal/file"
ln -s target-path "$mx_test_dir/removal/link"
mkdir -p "$mx_test_dir/removal/tree/sub"
printf nested > "$mx_test_dir/removal/tree/sub/leaf"
ln -s ../file "$mx_test_dir/removal/tree/inner-link"
mx_remove_path "$mx_test_dir/removal/absent"
[[ ${#mx_transaction_paths[@]} == 0 ]]
mx_remove_path "$mx_test_dir/removal/file"
mx_remove_path "$mx_test_dir/removal/link"
mx_remove_path "$mx_test_dir/removal/tree"
[[ ! -e $mx_test_dir/removal/file && ! -L $mx_test_dir/removal/link && ! -e $mx_test_dir/removal/tree && ${#mx_transaction_paths[@]} == 3 ]]
mx_restore_files
[[ $(cat "$mx_test_dir/removal/file") == file-content && $(stat -c %a "$mx_test_dir/removal/file") == 640 && $(readlink "$mx_test_dir/removal/link") == target-path && $(cat "$mx_test_dir/removal/tree/sub/leaf") == nested && $(readlink "$mx_test_dir/removal/tree/inner-link") == ../file ]]
mx_transaction_paths=()
mx_transaction_backups=()
mx_backup_dir="$mx_test_dir/footprint-backup"
mkdir "$mx_backup_dir"
mx_test_root="$mx_test_dir/footprint-root"
mx_test_found=(
    /etc/systemd/system/mxguest-agent.service /etc/systemd/system/mxguest-agent.service.d /etc/systemd/system/mxguest-mxfs.service
    /etc/systemd/system/multi-user.target.wants/mxguest-agent.service /etc/systemd/system/multi-user.target.wants/mxguest-mxfs.service
    /etc/systemd/user/mxguest-clipboard.service /etc/systemd/user/mxguest-integrate.service
    /etc/systemd/user/graphical-session.target.wants/mxguest-clipboard.service /etc/systemd/user/graphical-session.target.wants/mxguest-integrate.service
    /usr/local/bin/mxguest-clipboard /usr/local/bin/mxguest-integrate /usr/local/bin/mxguest-verify
    /usr/local/sbin/mxguest-agentd /usr/local/sbin/mxguest-mxfs
    /usr/local/libexec/mxguest-integrate-atspi /usr/local/libexec/mxguest-integrate-session /usr/local/libexec/mxguest-integrate-x11-helper
    /usr/local/libexec/mxguest-vector-guard /usr/local/libexec/mxguest_integration_geometry.py /usr/local/libexec/mxguest-power
    /opt/mxgpu/current /opt/mxgpu/releases/abc /usr/src/mxguest-agent-1.0 /usr/src/mxgpu-abc
    /lib/modules/6.8.0/updates/mxgpu.ko.zst /lib/modules/6.8.0/updates/mxguest.ko
    /opt/mxgpu-dev /etc/mxgpu-dev /etc/systemd/system/gdm.service.d/90-mxgpu-dev.conf
    /home/alice/.config/environment.d/90-mxgpu-dev.conf /home/alice/.config/systemd/user/org.gnome.Shell@.service.d/90-mxgpu-dev.conf
    /root/.config/environment.d/90-mxgpu-dev.conf
)
mx_test_ignored=(
    /etc/systemd/system/gdm.service.d/other.conf /home/alice/.config/environment.d/other.conf /home/bob/.config/environment.d/90-mxgpu-dev.conf
    /usr/local/bin/mxguest-other /usr/local/sbin/mxguest-agentd.bak /etc/systemd/system/mxguest-other.service /usr/src/other-1
    /lib/modules/6.8.0/updates/other.ko /lib/modules/6.8.0/updates/dkms/mxgpu.ko /lib/modules/6.9.0/updates/mxgpu.ko
)
for mx_test_item in "${mx_test_found[@]}" "${mx_test_ignored[@]}"; do
    mkdir -p "$mx_test_root$(dirname "$mx_test_item")"
    case $mx_test_item in
        *.d|*/mxguest-power|*/releases/abc|/usr/src/*|*/mxgpu-dev) mkdir "$mx_test_root$mx_test_item" ;;
        */wants/*|*/current) ln -s elsewhere "$mx_test_root$mx_test_item" ;;
        *) printf data > "$mx_test_root$mx_test_item" ;;
    esac
done
printf '%s\n' 'root:x:0:0:root:/root:/bin/sh' 'bob:x:999:999::/home/bob:/bin/sh' 'alice:x:1000:1000::/home/alice:/bin/sh' 'carol:x:1001:1001::/home/carol:/bin/sh' > "$mx_test_root/etc/passwd"
mx_test_expected=$(for mx_test_item in "${mx_test_found[@]}"; do printf '%s\n' "$mx_test_root$mx_test_item"; done | sort)
[[ $(mx_scan_footprint "$mx_test_root" 6.8.0 | sort) == "$mx_test_expected" ]]
[[ -z $(mx_scan_footprint "$mx_test_dir/empty-root" 6.8.0) ]]
mx_test_list=$(mx_scan_footprint "$mx_test_root" 6.8.0)
while IFS= read -r mx_test_item; do mx_remove_footprint_item "$mx_test_item"; done <<< "$mx_test_list"
[[ -z $(mx_scan_footprint "$mx_test_root" 6.8.0) && -f $mx_test_root/usr/local/bin/mxguest-other && -f $mx_test_root/lib/modules/6.8.0/updates/dkms/mxgpu.ko ]]
[[ -f $mx_test_root/etc/systemd/system/gdm.service.d/other.conf && -f $mx_test_root/home/bob/.config/environment.d/90-mxgpu-dev.conf && -f $mx_test_root/home/alice/.config/environment.d/other.conf ]]
[[ ! -e $mx_test_root/home/alice/.config/systemd/user/org.gnome.Shell@.service.d && ! -e $mx_test_root/opt/mxgpu-dev ]]
mx_restore_files
[[ $(mx_scan_footprint "$mx_test_root" 6.8.0 | sort) == "$mx_test_expected" && -L $mx_test_root/opt/mxgpu/current && -d $mx_test_root/home/alice/.config/systemd/user/org.gnome.Shell@.service.d ]]
[[ -z $(find "$mx_test_root" -name '*.mx-removed-*') ]]
mkdir -p "$mx_test_dir/commands"
cat > "$mx_test_dir/commands/dkms" <<'EOF'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
printf '%s\n' "$*" >> "$MX_TEST_DKMS_LOG"
if [[ $1 == status && $* != *' -v '* && $3 == mxgpu ]]; then
    printf '%s\n' 'mxgpu/aaa, 6.8.0, x86_64: installed' 'mxgpu/bbb, 6.8.0, x86_64: built' 'mxgpu/bbb, 6.7.0, x86_64: installed' 'mxgpu/ccc: added'
fi
exit 0
EOF
chmod 755 "$mx_test_dir/commands/dkms"
mx_test_old_path=$PATH
export PATH="$mx_test_dir/commands:$PATH" MX_TEST_DKMS_LOG="$mx_test_dir/dkms.log"
: > "$MX_TEST_DKMS_LOG"
[[ $(mx_dkms_registrations mxgpu 6.8.0 dkms) == $'aaa\t1\nbbb\t0\nccc\t0' ]]
[[ -z $(mx_dkms_registrations mxguest-agent 6.8.0 dkms) ]]
mx_removed_dkms=()
mx_uninstalled=()
: > "$MX_TEST_DKMS_LOG"
mx_remove_dkms_package mxgpu 6.8.0 dkms
[[ ${mx_uninstalled[*]} == 'dkms:mxgpu/aaa dkms:mxgpu/bbb dkms:mxgpu/ccc' ]]
for mx_test_item in aaa bbb ccc; do grep -Fxq "remove -m mxgpu -v $mx_test_item --all" "$MX_TEST_DKMS_LOG"; done
: > "$MX_TEST_DKMS_LOG"
mx_restore_removed_dkms 6.8.0 dkms
for mx_test_item in aaa bbb ccc; do grep -Fxq "add -m mxgpu -v $mx_test_item" "$MX_TEST_DKMS_LOG"; done
grep -Fxq 'build -m mxgpu -v aaa -k 6.8.0' "$MX_TEST_DKMS_LOG"
grep -Fxq 'install -m mxgpu -v aaa -k 6.8.0 --force' "$MX_TEST_DKMS_LOG"
if grep -Fq -e 'build -m mxgpu -v bbb' -e 'install -m mxgpu -v ccc' "$MX_TEST_DKMS_LOG"; then
    printf '%s\n' 'DKMS rollback reinstalled a version that was not installed.' >&2
    exit 1
fi
export PATH="$mx_test_old_path"
unset MX_TEST_DKMS_LOG
mx_test_fresh_conf=$(mx_render_dkms_conf abc123 1)
mx_test_kept_conf=$(mx_render_dkms_conf abc123 0)
mx_test_make='make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build modules && make -C linux/kernel-modules/mxguest KDIR=/lib/modules/${kernelver}/build modules'
mx_test_clean='make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build clean && make -C linux/kernel-modules/mxguest KDIR=/lib/modules/${kernelver}/build clean'
[[ $mx_test_fresh_conf == *'PACKAGE_VERSION="abc123"'* && $mx_test_fresh_conf == *'BUILT_MODULE_NAME[0]="mxgpu"'* && $mx_test_fresh_conf == *'BUILT_MODULE_NAME[1]="mxguest"'* && $mx_test_fresh_conf == *'BUILT_MODULE_LOCATION[1]="linux/kernel-modules/mxguest"'* && $mx_test_fresh_conf == *'DEST_MODULE_LOCATION[1]="/updates/dkms"'* && $mx_test_fresh_conf == *"MAKE[0]=\"$mx_test_make\""* && $mx_test_fresh_conf == *"CLEAN=\"$mx_test_clean\""* ]]
[[ $mx_test_kept_conf == *'BUILT_MODULE_NAME[0]="mxgpu"'* && $mx_test_kept_conf != *mxguest* && $mx_test_kept_conf != *'[1]'* && $mx_test_kept_conf == *'MAKE[0]="make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build modules"'* && $mx_test_kept_conf == *'CLEAN="make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build clean"'* ]]
mx_transaction_paths=()
mx_transaction_backups=()
mx_backup_dir="$mx_test_dir/directory-backup"
mkdir "$mx_backup_dir"
mkdir -p "$mx_test_dir/staged-extension/sub" "$mx_test_dir/installed-extension"
printf new > "$mx_test_dir/staged-extension/extension.js"
printf nested > "$mx_test_dir/staged-extension/sub/file.js"
printf old > "$mx_test_dir/installed-extension/old.js"
chmod 750 "$mx_test_dir/installed-extension"
mx_replace_dir "$mx_test_dir/installed-extension" "$mx_test_dir/staged-extension"
mx_replace_dir "$mx_test_dir/extensions/fresh-extension" "$mx_test_dir/staged-extension"
[[ ! -e $mx_test_dir/installed-extension/old.js && $(cat "$mx_test_dir/installed-extension/sub/file.js") == nested && $(cat "$mx_test_dir/extensions/fresh-extension/extension.js") == new ]]
[[ -z $(find "$mx_test_dir" -name '*.mx-install-*' -o -name '*.mx-old-*') ]]
mx_restore_files
[[ $(cat "$mx_test_dir/installed-extension/old.js") == old && ! -e $mx_test_dir/installed-extension/extension.js && $(stat -c %a "$mx_test_dir/installed-extension") == 750 && ! -e $mx_test_dir/extensions/fresh-extension ]]
if mx_replace_dir "$mx_test_dir/input" "$mx_test_dir/staged-extension" 2>/dev/null; then
    printf '%s\n' 'A file collision was accepted by the directory replacement.' >&2
    exit 1
fi
[[ $(cat "$mx_test_dir/input") == replacement ]]
mx_test_repository="$mx_test_dir/source"
git init -q "$mx_test_repository"
printf '# SPDX-License-Identifier: GPL-2.0-only\n# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke\n/build/\n' > "$mx_test_repository/.gitignore"
printf '# SPDX-License-Identifier: GPL-2.0-only\n# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke\nall:\n\ttrue\n' > "$mx_test_repository/Makefile"
git -C "$mx_test_repository" add .gitignore Makefile
git -C "$mx_test_repository" -c user.name='MX installer test' -c user.email=installer-test@example.invalid commit -qm 'Create a source-validation fixture'
mx_test_revision=$(git -C "$mx_test_repository" rev-parse HEAD)
mx_verify_checkout "$mx_test_repository" "$mx_test_revision"
mkdir "$mx_test_repository/build"
printf generated > "$mx_test_repository/build/output"
mx_verify_checkout "$mx_test_repository" "$mx_test_revision"
printf override > "$mx_test_repository/GNUmakefile"
if mx_verify_checkout "$mx_test_repository" "$mx_test_revision"; then
    printf '%s\n' 'An untracked make override was accepted.' >&2
    exit 1
fi
rm "$mx_test_repository/GNUmakefile"
printf pinned-overlay > "$mx_test_dir/overlay"
printf pinned-wrap > "$mx_test_dir/wrap"
cp "$mx_test_dir/overlay" "$mx_test_repository/meson.build"
sha256sum "$mx_test_dir/wrap" | cut -d ' ' -f 1 > "$mx_test_repository/.meson-subproject-wrap-hash.txt"
mx_verify_checkout "$mx_test_repository" "$mx_test_revision" "$mx_test_dir/overlay" "$mx_test_dir/wrap"
printf changed-overlay > "$mx_test_repository/meson.build"
if mx_verify_checkout "$mx_test_repository" "$mx_test_revision" "$mx_test_dir/overlay" "$mx_test_dir/wrap"; then
    printf '%s\n' 'An altered dependency build overlay was accepted.' >&2
    exit 1
fi
cp "$mx_test_dir/overlay" "$mx_test_repository/meson.build"
printf bad-hash > "$mx_test_repository/.meson-subproject-wrap-hash.txt"
if mx_verify_checkout "$mx_test_repository" "$mx_test_revision" "$mx_test_dir/overlay" "$mx_test_dir/wrap"; then
    printf '%s\n' 'An altered dependency wrap hash was accepted.' >&2
    exit 1
fi
printf '%s\n' 'PASS actual Bash public URL, commit, licence, identity and duplicate validation' 'PASS actual Bash file/symlink/mode rollback and directory collision preservation' 'PASS uninstall path removal and rollback for file, symlink and directory, exact footprint scan, and DKMS registration removal and restoration' 'PASS DKMS configuration for a fresh and a preserved agent and transactional directory replacement with rollback' 'PASS real Git source pin, ignored build output, make override and exact Meson overlay/hash validation'
