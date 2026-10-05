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
mx_backup_dir="$mx_test_dir/service-backup"
mkdir "$mx_backup_dir"
mkdir -p "$mx_test_dir/commands"
cat > "$mx_test_dir/commands/systemctl" <<'EOF'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
[[ $# == 4 && $1 == show && $2 == --property=LoadState && $3 == --value && $4 == mxguest-agent.service ]] || exit 91
printf '%s\n' "$MX_TEST_SERVICE_LOAD_STATE"
exit "$MX_TEST_SERVICE_STATUS"
EOF
chmod 755 "$mx_test_dir/commands/systemctl"
mx_test_old_path=$PATH
export PATH="$mx_test_dir/commands:$PATH" MX_TEST_SERVICE_LOAD_STATE=not-found MX_TEST_SERVICE_STATUS=0
mx_test_root="$mx_test_dir/service-root"
mx_test_helper="$mx_test_root/usr/local/libexec/mxguest-power/loginctl-power-compat.sh"
mx_test_dropin="$mx_test_root/etc/systemd/system/mxguest-agent.service.d/96-power-compat.conf"
mkdir -p "$(dirname "$mx_test_helper")" "$(dirname "$mx_test_dropin")"
printf original-helper > "$mx_test_helper"
printf original-dropin > "$mx_test_dropin"
chmod 750 "$mx_test_helper"
chmod 640 "$mx_test_dropin"
mx_install_preserved_service_compat "$mx_test_script" "$mx_test_root"
[[ $(cat "$mx_test_helper") == original-helper && $(cat "$mx_test_dropin") == original-dropin && ${#mx_transaction_paths[@]} == 0 ]]
export MX_TEST_SERVICE_LOAD_STATE=loaded
mx_install_preserved_service_compat "$mx_test_script" "$mx_test_root"
cmp "$mx_test_helper" "$mx_test_script/loginctl-power-compat.sh"
cmp "$mx_test_dropin" "$mx_test_script/96-power-compat.conf"
[[ $(stat -c %a "$mx_test_helper") == 755 && $(stat -c %a "$mx_test_dropin") == 644 ]]
mx_restore_files
[[ $(cat "$mx_test_helper") == original-helper && $(stat -c %a "$mx_test_helper") == 750 && $(cat "$mx_test_dropin") == original-dropin && $(stat -c %a "$mx_test_dropin") == 640 ]]
export MX_TEST_SERVICE_STATUS=23
if mx_install_preserved_service_compat "$mx_test_script" "$mx_test_root"; then
    printf '%s\n' 'A failed service query was accepted.' >&2
    exit 1
fi
export PATH="$mx_test_old_path"
unset MX_TEST_SERVICE_LOAD_STATE MX_TEST_SERVICE_STATUS
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
printf '%s\n' 'PASS actual Bash public URL, commit, licence, identity and duplicate validation' 'PASS actual Bash file/symlink/mode rollback and directory collision preservation' 'PASS preserved loaded-service compatibility deployment, modes, refusal and rollback without power requests' 'PASS real Git source pin, ignored build output, make override and exact Meson overlay/hash validation'
