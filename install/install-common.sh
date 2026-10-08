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

mx_replace_dir() {
    local destination=$1 input=$2 previous temporary="$1.mx-install-$$" retired="$1.mx-old-$$"
    [[ -d $input && ! -L $input ]] || { printf 'Not a directory: %s\n' "$input" >&2; return 1; }
    mkdir -p -- "$(dirname -- "$destination")"
    previous="$mx_backup_dir/${#mx_transaction_paths[@]}"
    if [[ -d $destination && ! -L $destination ]]; then
        cp -a -- "$destination" "$previous"
    elif [[ -e $destination || -L $destination ]]; then
        printf 'Refusing to replace a non-directory: %s\n' "$destination" >&2
        return 1
    fi
    mx_transaction_paths+=("$destination")
    mx_transaction_backups+=("$previous")
    rm -rf -- "$temporary" "$retired"
    cp -a -- "$input" "$temporary"
    chmod -R u=rwX,go=rX -- "$temporary"
    if [[ -d $destination ]]; then
        mv -T -- "$destination" "$retired"
        mv -T -- "$temporary" "$destination"
        rm -rf -- "$retired"
    else
        mv -T -- "$temporary" "$destination"
    fi
    printf '%s\t%s\n' "$destination" "$previous" >> "$mx_backup_dir/files.tsv"
}

mx_restore_files() {
    local index destination previous failed=0
    for ((index=${#mx_transaction_paths[@]}-1; index>=0; index--)); do
        destination=${mx_transaction_paths[index]}
        previous=${mx_transaction_backups[index]}
        rm -rf -- "$destination.mx-install-$$" "$destination.mx-old-$$" || { failed=1; continue; }
        if [[ -d $destination && ! -L $destination ]]; then
            rm -rf -- "$destination" || { failed=1; continue; }
        else
            rm -f -- "$destination" || { failed=1; continue; }
        fi
        if [[ $previous == "$destination.mx-removed-$$" && -d $previous ]]; then
            mv -T -- "$previous" "$destination" || failed=1
        elif [[ -L $previous || -f $previous || -d $previous ]]; then
            cp -a -- "$previous" "$destination" || failed=1
        fi
    done
    return "$failed"
}

mx_render_dkms_conf() {
    local fingerprint=$1 with_transport=$2 build clean
    build='make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build modules'
    clean='make -C linux/kernel-modules/mxgpu KDIR=/lib/modules/${kernelver}/build clean'
    if [[ $with_transport == 1 ]]; then
        build+=' && make -C linux/kernel-modules/mxguest KDIR=/lib/modules/${kernelver}/build modules'
        clean+=' && make -C linux/kernel-modules/mxguest KDIR=/lib/modules/${kernelver}/build clean'
    fi
    printf '# SPDX-%s\n' 'License-Identifier: GPL-2.0-only' 'FileCopyrightText: 2026 Zak Noble-Clarke'
    printf '%s\n' 'PACKAGE_NAME="mxgpu"' "PACKAGE_VERSION=\"$fingerprint\"" \
        'BUILT_MODULE_NAME[0]="mxgpu"' 'BUILT_MODULE_LOCATION[0]="linux/kernel-modules/mxgpu"' 'DEST_MODULE_LOCATION[0]="/updates/dkms"'
    if [[ $with_transport == 1 ]]; then
        printf '%s\n' 'BUILT_MODULE_NAME[1]="mxguest"' 'BUILT_MODULE_LOCATION[1]="linux/kernel-modules/mxguest"' 'DEST_MODULE_LOCATION[1]="/updates/dkms"'
    fi
    printf '%s\n' 'AUTOINSTALL="yes"' "MAKE[0]=\"$build\"" "CLEAN=\"$clean\""
}

mx_remove_path() {
    local target=$1 previous
    [[ -e $target || -L $target ]] || return 0
    if [[ -d $target && ! -L $target ]]; then
        previous="$target.mx-removed-$$"
        rm -rf -- "$previous"
        mv -T -- "$target" "$previous"
        mx_transaction_paths+=("$target")
        mx_transaction_backups+=("$previous")
        printf '%s\t%s\n' "$target" "$previous" >> "$mx_backup_dir/files.tsv"
        return 0
    fi
    previous="$mx_backup_dir/${#mx_transaction_paths[@]}"
    cp -a -- "$target" "$previous"
    mx_transaction_paths+=("$target")
    mx_transaction_backups+=("$previous")
    printf '%s\t%s\n' "$target" "$previous" >> "$mx_backup_dir/files.tsv"
    rm -f -- "$target"
}

mx_remove_empty_dir() {
    local target=$1
    [[ -d $target && ! -L $target && -z $(ls -A -- "$target") ]] || return 0
    mx_remove_path "$target"
}

mx_remove_footprint_item() {
    local target=$1 parent
    mx_remove_path "$target" || return
    parent=$(dirname -- "$target")
    case $target in
        */gdm.service.d/90-mxgpu-dev.conf|*/org.gnome.Shell@.service.d/90-mxgpu-dev.conf) mx_remove_empty_dir "$parent" ;;
    esac
}

mx_discard_retired() {
    local index
    for ((index=0; index<${#mx_transaction_paths[@]}; index++)); do
        [[ ${mx_transaction_backups[index]} == "${mx_transaction_paths[index]}.mx-removed-$$" ]] || continue
        rm -rf -- "${mx_transaction_backups[index]}"
    done
}

mx_footprint_fixed=(
    /etc/systemd/system/mxguest-agent.service
    /etc/systemd/system/mxguest-agent.service.d
    /etc/systemd/system/mxguest-mxfs.service
    /etc/systemd/system/multi-user.target.wants/mxguest-agent.service
    /etc/systemd/system/multi-user.target.wants/mxguest-mxfs.service
    /etc/systemd/user/mxguest-clipboard.service
    /etc/systemd/user/mxguest-integrate.service
    /etc/systemd/user/graphical-session.target.wants/mxguest-clipboard.service
    /etc/systemd/user/graphical-session.target.wants/mxguest-integrate.service
    /usr/local/bin/mxguest-clipboard
    /usr/local/bin/mxguest-integrate
    /usr/local/bin/mxguest-verify
    /usr/local/sbin/mxguest-agentd
    /usr/local/sbin/mxguest-mxfs
    /usr/local/libexec/mxguest-integrate-atspi
    /usr/local/libexec/mxguest-integrate-session
    /usr/local/libexec/mxguest-integrate-x11-helper
    /usr/local/libexec/mxguest-vector-guard
    /usr/local/libexec/mxguest_integration_geometry.py
    /usr/local/libexec/mxguest-power
    /opt/mxgpu/current
    /opt/mxgpu-dev
    /etc/mxgpu-dev
    /etc/systemd/system/gdm.service.d/90-mxgpu-dev.conf
)

mx_footprint_home=(
    .config/environment.d/90-mxgpu-dev.conf
    .config/systemd/user/org.gnome.Shell@.service.d/90-mxgpu-dev.conf
)

mx_scan_footprint() {
    local root=${1:-} kernel=${2:-} path passwd_data= user secret uid gid comment home shell relative
    local -A seen=()
    local -a homes=(/root)
    for path in "${mx_footprint_fixed[@]}"; do
        if [[ -e $root$path || -L $root$path ]]; then printf '%s\n' "$root$path"; fi
    done
    for path in "$root"/usr/src/mxguest-agent-* "$root"/usr/src/mxgpu-* "$root"/opt/mxgpu/releases/*; do
        [[ $path != *.mx-removed-* ]] || continue
        if [[ -e $path || -L $path ]]; then printf '%s\n' "$path"; fi
    done
    if [[ -n $kernel ]]; then
        for path in "$root/lib/modules/$kernel/updates"/mxgpu.ko* "$root/lib/modules/$kernel/updates"/mxguest*.ko*; do
            if [[ -f $path || -L $path ]]; then printf '%s\n' "$path"; fi
        done
    fi
    if [[ -z $root ]] && command -v getent >/dev/null; then
        passwd_data=$(getent passwd) || passwd_data=
    elif [[ -r $root/etc/passwd ]]; then
        passwd_data=$(< "$root/etc/passwd")
    fi
    while IFS=: read -r user secret uid gid comment home shell; do
        [[ $uid =~ ^[0-9]+$ && $home == /* ]] || continue
        if ((10#$uid == 0 || 10#$uid >= 1000)); then homes+=("$home"); fi
    done <<< "$passwd_data"
    for home in "${homes[@]}"; do
        [[ -z ${seen[$home]:-} ]] || continue
        seen[$home]=1
        for relative in "${mx_footprint_home[@]}"; do
            path="$root$home/$relative"
            if [[ -e $path || -L $path ]]; then printf '%s\n' "$path"; fi
        done
    done
    return 0
}

mx_removed_dkms=()
mx_uninstalled=()

mx_dkms_registrations() {
    local name=$1 kernel=$2 status line version flag expression
    local -A flags=()
    local -a order=()
    shift 2
    status=$(timeout 30 "$@" status -m "$name") || return
    expression="^$name(/|,[[:space:]]+)([A-Za-z0-9._+-]+)(,[[:space:]]+([^,]+),[[:space:]]+[^:,]+)?:[[:space:]]*(.*)$"
    while IFS= read -r line; do
        [[ $line =~ $expression ]] || continue
        version=${BASH_REMATCH[2]}
        flag=0
        [[ ${BASH_REMATCH[4]} == "$kernel" && ${BASH_REMATCH[5]} == installed* ]] && flag=1
        if [[ -z ${flags[$version]:-} ]]; then order+=("$version"); flags[$version]=$flag
        elif ((flag)); then flags[$version]=1
        fi
    done <<< "$status"
    for version in ${order[@]+"${order[@]}"}; do printf '%s\t%s\n' "$version" "${flags[$version]}"; done
}

mx_remove_dkms_package() {
    local name=$1 kernel=$2 version installed registrations
    shift 2
    registrations=$(mx_dkms_registrations "$name" "$kernel" "$@") || return
    while IFS=$'\t' read -r version installed; do
        [[ -n $version ]] || continue
        mx_removed_dkms+=("$name $version $installed")
        mx_uninstalled+=("dkms:$name/$version")
        timeout 900 "$@" remove -m "$name" -v "$version" --all < /dev/null || return
    done <<< "$registrations"
}

mx_restore_removed_dkms() {
    local kernel=$1 index name version installed registered failed=0
    shift
    for ((index=${#mx_removed_dkms[@]}-1; index>=0; index--)); do
        read -r name version installed <<< "${mx_removed_dkms[index]}"
        registered=$(timeout 30 "$@" status -m "$name" -v "$version") || { failed=1; continue; }
        if [[ -z $registered ]]; then timeout 900 "$@" add -m "$name" -v "$version" < /dev/null || { failed=1; continue; }; fi
        if [[ $installed == 1 ]]; then
            { timeout 1800 "$@" build -m "$name" -v "$version" -k "$kernel" && timeout 900 "$@" install -m "$name" -v "$version" -k "$kernel" --force; } < /dev/null || failed=1
        fi
    done
    return "$failed"
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
