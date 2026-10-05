# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

mx_transaction_paths=()
mx_transaction_backups=()
mx_transaction_active=0

mx_replace_file() {
    local destination=$1 input=$2 mode=${3:-644} previous
    mkdir -p -- "$(dirname -- "$destination")"
    previous="$mx_backup_dir/${#mx_transaction_paths[@]}"
    if [[ -L $destination || -f $destination ]]; then
        cp -a -- "$destination" "$previous"
    elif [[ -e $destination ]]; then
        printf 'Refusing to replace a directory: %s\n' "$destination" >&2
        return 1
    fi
    mx_transaction_paths+=("$destination")
    mx_transaction_backups+=("$previous")
    install -m "$mode" -- "$input" "$destination.mx-install-$$"
    mv -fT -- "$destination.mx-install-$$" "$destination"
    printf '%s\t%s\n' "$destination" "$previous" >> "$mx_backup_dir/files.tsv"
}

mx_replace_link() {
    local destination=$1 target=$2 previous
    mkdir -p -- "$(dirname -- "$destination")"
    previous="$mx_backup_dir/${#mx_transaction_paths[@]}"
    if [[ -L $destination || -f $destination ]]; then
        cp -a -- "$destination" "$previous"
    elif [[ -e $destination ]]; then
        printf 'Refusing to replace a directory: %s\n' "$destination" >&2
        return 1
    fi
    mx_transaction_paths+=("$destination")
    mx_transaction_backups+=("$previous")
    ln -s -- "$target" "$destination.mx-install-$$"
    mv -fT -- "$destination.mx-install-$$" "$destination"
    printf '%s\t%s\n' "$destination" "$previous" >> "$mx_backup_dir/files.tsv"
}

mx_restore_files() {
    local index destination previous failed=0
    for ((index=${#mx_transaction_paths[@]}-1; index>=0; index--)); do
        destination=${mx_transaction_paths[index]}
        previous=${mx_transaction_backups[index]}
        rm -f -- "$destination" "$destination.mx-install-$$" || { failed=1; continue; }
        if [[ -L $previous || -f $previous ]]; then
            cp -a -- "$previous" "$destination" || failed=1
        fi
    done
    return "$failed"
}

mx_install_preserved_service_compat() {
    local source_directory=$1 destination_root=${2:-} load_state
    load_state=$(timeout 10 systemctl show --property=LoadState --value mxguest-agent.service) || return
    [[ $load_state == loaded ]] || return 0
    mx_replace_file "$destination_root/usr/local/libexec/mxguest-power/loginctl-power-compat.sh" "$source_directory/loginctl-power-compat.sh" 755 || return
    mx_replace_file "$destination_root/etc/systemd/system/mxguest-agent.service.d/96-power-compat.conf" "$source_directory/96-power-compat.conf" 644
}

mx_verify_checkout() {
    local repository=$1 revision=$2 overlay=${3:-} wrap=${4:-} filename expected
    [[ $(git -C "$repository" rev-parse HEAD) == "$revision" && -z $(git -C "$repository" status --porcelain --untracked-files=no) ]] || return 1
    while IFS= read -r -d '' filename; do
        case $filename in
            meson.build)
                [[ -n $overlay && -f $repository/$filename && ! -L $repository/$filename ]] || return 1
                cmp -- "$overlay" "$repository/$filename" > /dev/null || return 1 ;;
            .meson-subproject-wrap-hash.txt)
                [[ -n $wrap && -f $repository/$filename && ! -L $repository/$filename ]] || return 1
                expected=$(sha256sum "$wrap") || return
                [[ $(cat "$repository/$filename") == "${expected%% *}" ]] || return 1 ;;
            *) return 1 ;;
        esac
    done < <(git -C "$repository" ls-files --others --exclude-standard -z)
}

mx_dkms_installed_version() {
    local kernel=$1 status line version=
    shift
    status=$(timeout 30 "$@" status -m mxgpu -k "$kernel" -a "$(uname -m)") || return
    while IFS= read -r line; do
        [[ $line == *': installed'* ]] || continue
        [[ $line =~ ^mxgpu/([A-Za-z0-9._+-]+),[[:space:]]+([^,]+),[[:space:]]+([^:]+):[[:space:]]+installed && ${BASH_REMATCH[2]} == "$kernel" && -z $version ]] || return 1
        version=${BASH_REMATCH[1]}
    done <<< "$status"
    printf '%s' "$version"
}

mx_restore_prior_dkms() {
    local kernel=$1 previous=$2 current
    shift 2
    [[ -n $previous ]] || return 0
    current=$(mx_dkms_installed_version "$kernel" "$@") || return
    [[ $current == "$previous" ]] || timeout 900 "$@" install -m mxgpu -v "$previous" -k "$kernel" --force
}

mx_export_source() {
    local repository=$1 destination=$2 tree_file metadata filename mode kind object_id link
    mkdir -p -- "$destination"
    tree_file=$(mktemp "$mx_build_workspace/source-tree-XXXXXXXX")
    timeout 900 git -C "$repository" ls-tree -r -z HEAD > "$tree_file"
    while IFS=$'\t' read -r -d '' metadata filename; do
        read -r mode kind object_id <<< "$metadata"
        [[ $kind != commit ]] || continue
        [[ $kind == blob && $filename != /* && /$filename/ != *'/../'* ]] || return 1
        mkdir -p -- "$(dirname -- "$destination/$filename")"
        case $mode in
            100644|100755)
                timeout 900 git -C "$repository" cat-file blob "$object_id" > "$destination/$filename"
                if [[ $mode == 100755 ]]; then chmod 755 "$destination/$filename"; else chmod 644 "$destination/$filename"; fi ;;
            120000)
                IFS= read -r -d '' link < <(git -C "$repository" cat-file blob "$object_id"; printf '\0')
                ln -s -- "$link" "$destination/$filename" ;;
            *) return 1 ;;
        esac
    done < "$tree_file"
    rm -f -- "$tree_file"
}
