#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
set -Eeuo pipefail

mx_script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$mx_script_dir/install-common.sh"
mx_sources="$mx_script_dir/sources.tsv"
mx_cache=/var/cache/mxguest/sources
mx_workspace=/var/lib/mxguest/installer
mx_mode=install
mx_dkms_added=0
mx_prefix=
mx_dkms_source=
mx_fingerprint=
mx_keep_dkms_source=0
mx_keep_prefix=0
mx_dkms=(dkms --dkmstree /var/lib/dkms --sourcetree /usr/src --installtree /lib/modules --directive modprobe_on_install= --directive post_transaction=)

while (($#)); do
    case $1 in
        --sources|--cache|--workspace)
            (($# >= 2)) || { printf 'Missing value for %s\n' "$1" >&2; exit 2; }
            case $1 in
                --sources) mx_sources=$2 ;;
                --cache) mx_cache=$2 ;;
                --workspace) mx_workspace=$2 ;;
            esac
            shift 2 ;;
        --plan) mx_mode=plan; shift ;;
        --validate) mx_mode=validate; shift ;;
        --prepare-source) mx_mode=prepare; shift ;;
        --build-only) mx_mode=build; shift ;;
        --help)
            printf '%s\n' 'Usage: install.sh [--sources FILE] [--plan|--validate|--prepare-source|--build-only] [--cache DIR] [--workspace DIR]'
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

mx_fail() { printf 'MX installation stopped: %s\n' "$*" >&2; exit 1; }
mx_run() { printf '+ '; printf '%q ' "$@"; printf '\n'; "$@"; }
declare -A mx_url mx_commit mx_license mx_relative mx_expected_url
mx_relative=([core]=core [common]=linux/common [kernel]=linux/kernel-modules [agent]=linux/guest-agent [mesa]=linux/mesa)
mx_expected_url=([core]=https://github.com/MXEmulation/mx-guest-core.git [common]=https://github.com/MXEmulation/mx-guest-linux-common.git [kernel]=https://github.com/MXEmulation/mx-guest-linux-kernel.git [agent]=https://github.com/MXEmulation/mx-guest-linux-agent.git [mesa]=https://github.com/MXEmulation/mesa-mxgpu.git)
mx_keys=(core common kernel agent mesa)
mx_header=0
mx_pci=0
[[ -f $mx_sources ]] || mx_fail "Missing source manifest: $mx_sources"
while IFS=$'\t' read -r mx_kind mx_key mx_value mx_revision mx_terms mx_extra || [[ -n ${mx_kind:-} ]]; do
    case $mx_kind in
        ''|'#'*) continue ;;
        MX_SOURCE_MANIFEST)
            [[ $mx_key == 1 && -z $mx_value && -z $mx_revision && -z $mx_terms && -z $mx_extra && $mx_header == 0 ]] || mx_fail 'Invalid source manifest header'
            mx_header=1 ;;
        COMPONENT)
            [[ -n ${mx_expected_url[$mx_key]:-} && -z ${mx_url[$mx_key]:-} && -z $mx_extra ]] || mx_fail 'Invalid or duplicate source component'
            [[ $mx_value == "${mx_expected_url[$mx_key]}" && $mx_revision =~ ^[0-9a-f]{40}$ ]] || mx_fail "Invalid public URL or commit: $mx_key"
            case $mx_key in
                core|common) [[ $mx_terms == MIT ]] || mx_fail 'Invalid component licence' ;;
                kernel|agent) [[ $mx_terms == GPL-2.0-only ]] || mx_fail 'Invalid component licence' ;;
                mesa) [[ $mx_terms == LicenseRef-Mesa-Multi-License ]] || mx_fail 'Invalid component licence' ;;
            esac
            mx_url[$mx_key]=$mx_value; mx_commit[$mx_key]=$mx_revision; mx_license[$mx_key]=$mx_terms ;;
        PCI)
            [[ $mx_key == mxgpu && $mx_pci == 0 && -z $mx_terms && -z $mx_extra && $mx_value =~ ^[1-9][0-9]{0,4}$ && $mx_revision =~ ^[1-9][0-9]{0,4}$ ]] || mx_fail 'Invalid GPU identity record'
            ((mx_value <= 65535 && mx_revision <= 65535)) || mx_fail 'GPU identity is out of range'
            mx_vendor=$mx_value; mx_device=$mx_revision; mx_pci=1 ;;
        *) mx_fail 'Unknown source manifest record' ;;
    esac
done < "$mx_sources"
[[ $mx_header == 1 && $mx_pci == 1 && ${#mx_url[@]} == 5 ]] || mx_fail 'Incomplete source manifest'
if [[ $mx_mode == validate ]]; then
    printf '%s\n' 'Verified the five exact public source URLs, commits, licences and GPU identity.'
    exit 0
fi
[[ $(uname -s) == Linux ]] || mx_fail 'This installer requires Linux'
mx_arch=$(uname -m)
[[ $mx_arch == x86_64 || $mx_arch == aarch64 ]] || mx_fail 'Supported architectures are x86_64 and aarch64'
mx_kernel=$(uname -r)
[[ $mx_kernel =~ ^([0-9]+)\.([0-9]+) ]] || mx_fail 'Invalid running kernel release'
((BASH_REMATCH[1] > 6 || (BASH_REMATCH[1] == 6 && BASH_REMATCH[2] >= 6))) || mx_fail 'The GPU requires Linux 6.6 or newer'
mx_gpu_present=0
for mx_function in /sys/bus/pci/devices/*; do
    [[ -r $mx_function/vendor && -r $mx_function/device ]] || continue
    read -r mx_sys_vendor < "$mx_function/vendor"
    read -r mx_sys_device < "$mx_function/device"
    [[ $mx_sys_vendor =~ ^0x[0-9a-fA-F]{1,4}$ && $mx_sys_device =~ ^0x[0-9a-fA-F]{1,4}$ ]] || continue
    if ((mx_sys_vendor == mx_vendor && mx_sys_device == mx_device)); then mx_gpu_present=1; break; fi
done
if command -v apt-get >/dev/null; then
    mx_packages=(apt-get install -y build-essential git dkms systemd coreutils "linux-headers-$mx_kernel" pkg-config meson ninja-build python3 python3-mako python3-yaml python3-packaging python3-venv python3-pip bison flex cmake libdrm-dev libglvnd-dev libexpat1-dev libudev-dev libx11-dev libxext-dev libxfixes-dev libx11-xcb-dev libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-randr0-dev libxcb-glx0-dev libxrandr-dev libxshmfence-dev libxxf86vm-dev libwayland-dev wayland-protocols libzstd-dev zlib1g-dev libelf-dev libvulkan-dev)
elif command -v dnf >/dev/null; then
    mx_packages=(dnf install -y gcc gcc-c++ make git dkms systemd coreutils "kernel-devel-$mx_kernel" pkgconf-pkg-config meson ninja-build python3 python3-mako python3-pyyaml python3-packaging python3-pip bison flex cmake libdrm-devel libglvnd-devel expat-devel systemd-devel libX11-devel libXext-devel libXfixes-devel libXrandr-devel libxcb-devel libxshmfence-devel libXxf86vm-devel wayland-devel wayland-protocols-devel libzstd-devel zlib-devel elfutils-libelf-devel vulkan-headers)
elif command -v pacman >/dev/null; then
    mx_kernel_package=$(pacman -Qqo "/usr/lib/modules/$mx_kernel/vmlinuz")
    [[ $mx_kernel_package =~ ^linux(-[a-z0-9]+)*$ ]] || mx_fail 'Cannot identify the running kernel package'
    mx_packages=(pacman -S --needed --noconfirm base-devel git dkms coreutils "$mx_kernel_package-headers" pkgconf meson ninja python python-mako python-yaml python-packaging python-pip cmake libdrm libglvnd expat systemd libx11 libxext libxfixes libxrandr libxcb libxshmfence libxxf86vm wayland wayland-protocols zstd zlib libelf vulkan-headers)
else
    mx_fail 'Supported package managers are apt-get, dnf and pacman'
fi
if [[ $mx_mode == plan ]]; then
    printf 'Kernel: %s\nArchitecture: %s\nGPU present: %s\nActivation: next boot\n' "$mx_kernel" "$mx_arch" "$mx_gpu_present"
    printf 'Packages: '; printf '%q ' "${mx_packages[@]}"; printf '\n'
    for mx_key in "${mx_keys[@]}"; do printf '%s\t%s\t%s\n' "$mx_key" "${mx_url[$mx_key]}" "${mx_commit[$mx_key]}"; done
    printf '%s\n' 'Uninstall before installing (running services and loaded modules are not stopped; activation is at next boot):'
    mx_plan_found=0
    if command -v dkms >/dev/null; then
        for mx_name in mxguest-agent mxgpu; do
            while IFS=$'\t' read -r mx_version mx_flag; do
                [[ -n $mx_version ]] || continue
                printf '  dkms:%s/%s\n' "$mx_name" "$mx_version"; mx_plan_found=1
            done < <(mx_dkms_registrations "$mx_name" "$mx_kernel" "${mx_dkms[@]}" 2>/dev/null || true)
        done
    fi
    while IFS= read -r mx_item; do printf '  %s\n' "$mx_item"; mx_plan_found=1; done < <(mx_scan_footprint '' "$mx_kernel")
    ((mx_plan_found)) || printf '%s\n' '  (no previous guest additions found)'
    printf '%s\n' 'Install: the mxgpu and mxguest modules through DKMS, the graphics stack, the guest agent service and, when GNOME Shell is installed, the session bridge extension with its autostart entry.'
    exit 0
fi
mx_exit() {
    local status=$? rollback_failed=0 registered
    if ((status && mx_transaction_active)); then
        set +e
        mx_restore_files || rollback_failed=1
        if ((mx_dkms_added)); then
            if registered=$(timeout 30 "${mx_dkms[@]}" status -m mxgpu -v "$mx_fingerprint"); then
                if [[ -n $registered ]]; then
                    timeout 900 "${mx_dkms[@]}" remove -m mxgpu -v "$mx_fingerprint" --all || rollback_failed=1
                fi
            else
                rollback_failed=1
            fi
        fi
        mx_restore_removed_dkms "$mx_kernel" "${mx_dkms[@]}" || rollback_failed=1
        [[ -z $mx_dkms_source ]] || ((mx_keep_dkms_source)) || rm -rf -- "$mx_dkms_source"
        [[ -z $mx_prefix ]] || ((mx_keep_prefix)) || rm -rf -- "$mx_prefix"
        timeout 60 depmod -a "$mx_kernel" || rollback_failed=1
        timeout 30 systemctl daemon-reload || rollback_failed=1
        if ((rollback_failed)); then
            printf 'Installation failed and restoration was incomplete. Retained backup: %s\n' "$mx_backup_dir" >&2
        else
            printf 'Installation failed. Previous files and DKMS registrations were restored. Backup: %s\n' "$mx_backup_dir" >&2
        fi
    fi
    exit "$status"
}
trap mx_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ $mx_mode == install ]]; then
    ((EUID == 0)) || mx_fail 'Installation requires root; use the media launcher or sudo'
    ((mx_gpu_present)) || mx_fail 'The manifest GPU is absent; no system changes were made'
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    exec 9>/run/mxguest-installer.lock
    flock -n 9 || mx_fail 'Another installer is running'
    unset LD_PRELOAD LD_LIBRARY_PATH PYTHONPATH PYTHONHOME
    for mx_name in ${!MESA_@} ${!LIBGL_@} ${!VK_@} ${!GBM_@} ${!__EGL_@}; do unset "$mx_name"; done
    if [[ ${mx_packages[0]} == apt-get ]]; then mx_run timeout 900 apt-get update; fi
    mx_run timeout 1800 "${mx_packages[@]}"
fi
command -v git >/dev/null || mx_fail 'Git is required to prepare source'
command -v timeout >/dev/null || mx_fail 'The coreutils timeout command is required'
mx_digest=$(sha256sum "$mx_sources")
mx_fingerprint=${mx_digest:0:24}
mx_checkout="$mx_cache/$mx_fingerprint"
mkdir -p -- "$mx_checkout" "$mx_workspace"
declare -A mx_path
for mx_key in "${mx_keys[@]}"; do
    mx_path[$mx_key]="$mx_checkout/${mx_relative[$mx_key]}"
    mx_source=${mx_path[$mx_key]}
    if [[ -e $mx_source ]]; then
        [[ -e $mx_source/.git ]] || mx_fail "Source cache is not a checkout: $mx_key"
        mx_verify_checkout "$mx_source" "${mx_commit[$mx_key]}" || mx_fail "Source cache differs from its pin: $mx_key"
    else
        mkdir -p -- "$(dirname -- "$mx_source")"
        mx_partial=$(mktemp -d "$(dirname -- "$mx_source")/mx-source-XXXXXXXX")
        mx_run git init "$mx_partial"
        mx_run git -C "$mx_partial" remote add origin "${mx_url[$mx_key]}"
        mx_run timeout 900 git -C "$mx_partial" -c credential.helper= fetch --depth=1 origin "${mx_commit[$mx_key]}"
        mx_run git -C "$mx_partial" checkout --detach FETCH_HEAD
        [[ $(git -C "$mx_partial" rev-parse HEAD) == "${mx_commit[$mx_key]}" ]] || mx_fail "Fetched commit mismatch: $mx_key"
        mv -- "$mx_partial" "$mx_source"
    fi
done
for mx_key in kernel agent; do
    for mx_dep in core linux-common; do
        mx_owner=core; [[ $mx_dep != linux-common ]] || mx_owner=common
        read -r mx_type mx_object mx_pin mx_entry < <(git -C "${mx_path[$mx_key]}" ls-tree HEAD "deps/$mx_dep")
        [[ $mx_type == 160000 && $mx_pin == "${mx_commit[$mx_owner]}" ]] || mx_fail "Inconsistent dependency pin: $mx_key/$mx_dep"
        [[ $(git config -f "${mx_path[$mx_key]}/.gitmodules" --get "submodule.deps/$mx_dep.path") == "deps/$mx_dep" && $(git config -f "${mx_path[$mx_key]}/.gitmodules" --get "submodule.deps/$mx_dep.url") == "${mx_url[$mx_owner]}" ]] || mx_fail 'Unexpected public submodule URL'
    done
    mx_run timeout 900 git -C "${mx_path[$mx_key]}" -c "submodule.deps/core.url=${mx_url[core]}" -c "submodule.deps/linux-common.url=${mx_url[common]}" submodule update --init --depth=1 -- deps/core deps/linux-common
    for mx_dep in core linux-common; do
        mx_owner=core; [[ $mx_dep != linux-common ]] || mx_owner=common
        mx_verify_checkout "${mx_path[$mx_key]}/deps/$mx_dep" "${mx_commit[$mx_owner]}" || mx_fail "Dependency checkout differs from its pin: $mx_key/$mx_dep"
    done
done
for mx_dep in mx-guest-core mx-guest-linux-common; do
    mx_owner=core; [[ $mx_dep != mx-guest-linux-common ]] || mx_owner=common
    mx_wrap_revision=; mx_wrap_url=
    while IFS= read -r mx_line; do
        if [[ $mx_line =~ ^[[:space:]]*revision[[:space:]]*=[[:space:]]*([0-9a-f]{40})[[:space:]]*$ ]]; then mx_wrap_revision=${BASH_REMATCH[1]}; fi
        if [[ $mx_line =~ ^[[:space:]]*url[[:space:]]*=[[:space:]]*([^[:space:]]+)[[:space:]]*$ ]]; then mx_wrap_url=${BASH_REMATCH[1]}; fi
    done < "${mx_path[mesa]}/subprojects/$mx_dep.wrap"
    [[ $mx_wrap_revision == "${mx_commit[$mx_owner]}" && $mx_wrap_url == "${mx_url[$mx_owner]}" ]] || mx_fail "Inconsistent Mesa dependency: $mx_dep"
    mx_target="${mx_path[mesa]}/subprojects/$mx_dep"
    [[ -e $mx_target ]] || mx_run git clone --no-hardlinks "${mx_path[$mx_owner]}" "$mx_target"
    mx_overlay="${mx_path[mesa]}/subprojects/packagefiles/$mx_dep/meson.build"
    mx_verify_checkout "$mx_target" "${mx_commit[$mx_owner]}" "$mx_overlay" "${mx_path[mesa]}/subprojects/$mx_dep.wrap" || mx_fail 'Mesa dependency cache changed'
    if [[ -e $mx_target/meson.build ]]; then
        cmp -- "$mx_overlay" "$mx_target/meson.build" > /dev/null || mx_fail 'Mesa dependency build definition differs from its pinned overlay'
    else
        cp -- "$mx_overlay" "$mx_target/meson.build"
    fi
done
if [[ $mx_mode == prepare ]]; then printf 'Verified exact public source commits and consumer pins: %s\n' "$mx_fingerprint"; exit 0; fi
mx_build_workspace="$mx_workspace/$mx_fingerprint"
mkdir -p -- "$mx_build_workspace"
mx_headers="/lib/modules/$mx_kernel/build"
[[ -d $mx_headers ]] || mx_fail 'The running kernel has no matching build headers'
mx_run timeout 900 make -C "${mx_path[core]}" test
mx_run timeout 900 make -C "${mx_path[common]}" test
mx_run timeout 900 make -C "${mx_path[agent]}" check
mx_jobs=$(getconf _NPROCESSORS_ONLN); ((mx_jobs <= 8)) || mx_jobs=8
mx_run timeout 1800 make -C "$mx_headers" "M=${mx_path[kernel]}/mxgpu" "-j$mx_jobs" modules
mx_run timeout 1800 make -C "$mx_headers" "M=${mx_path[kernel]}/mxguest" "-j$mx_jobs" modules
for mx_module in "${mx_path[kernel]}/mxgpu/mxgpu.ko" "${mx_path[kernel]}/mxguest/mxguest.ko"; do
    mx_vermagic=$(modinfo -F vermagic "$mx_module")
    [[ ${mx_vermagic%% *} == "$mx_kernel" && $(modinfo -F license "$mx_module") == GPL ]] || mx_fail "Built module kernel or licence mismatch: $mx_module"
done
mx_run python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)'
rm -rf -- "$mx_build_workspace/session-bridge"
mx_run python3 "${mx_path[agent]}/session-bridge/build-package.py" --core "${mx_path[agent]}/deps/core" --output "$mx_build_workspace/session-bridge"
mx_bridge_uuid=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["uuid"])' "${mx_path[agent]}/session-bridge/metadata.json") || mx_fail 'Cannot read the session bridge identifier'
[[ $mx_bridge_uuid =~ ^[A-Za-z0-9._@-]+$ ]] || mx_fail 'Invalid session bridge identifier'
mx_bridge_package="$mx_build_workspace/session-bridge/$mx_bridge_uuid"
[[ -d $mx_bridge_package ]] || mx_fail 'Session bridge package was not staged'
mx_meson=$(command -v meson)
mx_meson_version=$($mx_meson --version)
IFS=. read -r mx_major mx_minor mx_patch <<< "$mx_meson_version"
if ((mx_major < 1 || (mx_major == 1 && mx_minor < 4))); then
    [[ -d $mx_build_workspace/build-tools ]] || mx_run python3 -m venv --system-site-packages "$mx_build_workspace/build-tools"
    mx_run timeout 900 "$mx_build_workspace/build-tools/bin/python" -m pip install 'meson>=1.4' mako PyYAML packaging
    mx_meson="$mx_build_workspace/build-tools/bin/meson"
fi
mx_prefix="/opt/mxgpu/releases/$mx_fingerprint"
mx_dkms_source="/usr/src/mxgpu-$mx_fingerprint"
mx_setup=("$mx_meson" setup)
[[ ! -f $mx_build_workspace/mesa-build/build.ninja ]] || mx_setup+=(--reconfigure)
mx_run timeout 900 "${mx_setup[@]}" "$mx_build_workspace/mesa-build" "${mx_path[mesa]}" -Dgallium-drivers=mxgpu -Dvulkan-drivers=mxgpu -Dplatforms=x11,wayland -Dllvm=disabled -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dbuild-tests=false -Dvideo-codecs= --wrap-mode=nodownload --buildtype=release "--prefix=$mx_prefix" --libdir=lib
mx_run timeout 3600 ninja -C "$mx_build_workspace/mesa-build" "-j$mx_jobs"
mx_stage=$(mktemp -d "$mx_build_workspace/mesa-stage-XXXXXXXX")
mx_run timeout 900 env "DESTDIR=$mx_stage" "$mx_meson" install -C "$mx_build_workspace/mesa-build" --no-rebuild
for mx_file in lib/dri/mxgpu_dri.so lib/libEGL_mesa.so.0 lib/libgbm.so.1 lib/libvulkan_mxgpu.so; do [[ -f $mx_stage$mx_prefix/$mx_file ]] || mx_fail "Mesa staging is missing $mx_file"; done
if [[ $mx_mode == build ]]; then
    printf 'stage\t%s\nprefix\t%s\nkernel\t%s\n' "$mx_stage" "$mx_prefix" "$mx_kernel" > "$mx_build_workspace/build-stage.tsv"
    printf 'Built and staged the exact public source release without installing drivers or services: %s\n' "$mx_stage"
    exit 0
fi
mx_backup_dir=$(mktemp -d "$mx_build_workspace/rollback-XXXXXXXX")
mx_transaction_active=1
for mx_name in mxguest-agent mxgpu; do
    mx_remove_dkms_package "$mx_name" "$mx_kernel" "${mx_dkms[@]}" || mx_fail "Cannot remove the DKMS registrations of $mx_name"
done
mx_deferred=()
mx_footprint_list=$(mx_scan_footprint '' "$mx_kernel")
while IFS= read -r mx_item; do
    [[ -n $mx_item ]] || continue
    case $mx_item in
        "$mx_dkms_source") mx_keep_dkms_source=1; mx_remove_path "$mx_item" ;;
        "$mx_prefix") mx_keep_prefix=1; mx_remove_path "$mx_item" ;;
        /usr/src/mxguest-agent-*|/usr/src/mxgpu-*|/opt/mxgpu/releases/*) mx_deferred+=("$mx_item") ;;
        *) mx_remove_footprint_item "$mx_item" ;;
    esac
    mx_uninstalled+=("$mx_item")
done <<< "$mx_footprint_list"
mkdir -p -- "$(dirname -- "$mx_prefix")"
cp -a -- "$mx_stage$mx_prefix" "$mx_prefix"
for mx_key in core common kernel; do
    mx_target="$mx_dkms_source/${mx_relative[$mx_key]}"
    mkdir -p -- "$mx_target"
    mx_export_source "${mx_path[$mx_key]}" "$mx_target"
done
for mx_dep in core linux-common; do
    mx_owner=core; [[ $mx_dep != linux-common ]] || mx_owner=common
    mx_target="$mx_dkms_source/linux/kernel-modules/deps/$mx_dep"
    mkdir -p -- "$mx_target"
    mx_export_source "${mx_path[$mx_owner]}" "$mx_target"
done
mx_render_dkms_conf "$mx_fingerprint" 1 > "$mx_dkms_source/dkms.conf"
mx_dkms_added=1
mx_run timeout 900 "${mx_dkms[@]}" add -m mxgpu -v "$mx_fingerprint"
mx_run timeout 1800 "${mx_dkms[@]}" build -m mxgpu -v "$mx_fingerprint" -k "$mx_kernel"
mx_run timeout 900 "${mx_dkms[@]}" install -m mxgpu -v "$mx_fingerprint" -k "$mx_kernel" --force
mx_replace_link /opt/mxgpu/current "$mx_prefix"
mx_configuration="$mx_build_workspace/configuration"
mkdir -p -- "$mx_configuration"
cat > "$mx_configuration/environment" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
LD_LIBRARY_PATH=/opt/mxgpu/current/lib
LIBGL_DRIVERS_PATH=/opt/mxgpu/current/lib/dri
GBM_BACKENDS_PATH=/opt/mxgpu/current/lib/gbm
MESA_LOADER_DRIVER_OVERRIDE=mxgpu
__EGL_VENDOR_LIBRARY_FILENAMES=/opt/mxgpu/current/share/glvnd/egl_vendor.d/50_mesa.json
COGL_DRIVER=gles2
EOF
mx_replace_file /etc/environment.d/70-mxgpu.conf "$mx_configuration/environment"
{ printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-only' '# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke'; while IFS= read -r mx_line; do [[ $mx_line == \#* ]] || printf 'export %s\n' "$mx_line"; done < "$mx_configuration/environment"; } > "$mx_configuration/profile"
mx_replace_file /etc/profile.d/mxgpu.sh "$mx_configuration/profile"
printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-only' '# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke' mxgpu mxguest > "$mx_configuration/modules"
mx_replace_file /etc/modules-load.d/mxgpu.conf "$mx_configuration/modules"
mx_run python3 "$mx_script_dir/manifest-helper.py" --icd "$mx_prefix/share/vulkan/icd.d/mxgpu_icd.json" --output "$mx_configuration/icd.json"
mx_replace_file /usr/share/vulkan/icd.d/mxgpu_icd.json "$mx_configuration/icd.json"
mx_bridge_installed=none
mx_replace_file /usr/local/sbin/mxguest-agentd "${mx_path[agent]}/build/mxguest-agentd" 755
cat > "$mx_configuration/agent.service" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
[Unit]
Description=MX guest agent
ConditionPathExists=/dev/mxguest-agent
After=systemd-udev-settle.service

[Service]
Type=simple
ExecStart=/usr/local/sbin/mxguest-agentd
RuntimeDirectory=mxguest-agent
RuntimeDirectoryMode=0755
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
mx_replace_file /etc/systemd/system/mxguest-agent.service "$mx_configuration/agent.service"
mx_replace_link /etc/systemd/system/multi-user.target.wants/mxguest-agent.service /etc/systemd/system/mxguest-agent.service
if command -v gnome-shell >/dev/null; then
    mx_replace_dir "/usr/share/gnome-shell/extensions/$mx_bridge_uuid" "$mx_bridge_package"
    cat > "$mx_configuration/session-bridge.desktop" <<EOF
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
[Desktop Entry]
Type=Application
Name=MX guest session bridge
Exec=gnome-extensions enable $mx_bridge_uuid
OnlyShowIn=GNOME;
NoDisplay=true
EOF
    mx_replace_file /etc/xdg/autostart/mxguest-session-bridge.desktop "$mx_configuration/session-bridge.desktop"
    mx_bridge_installed=$mx_bridge_uuid
fi
mx_run timeout 30 systemctl daemon-reload
mx_run depmod -a "$mx_kernel"
mx_uninstalled_list=none
if ((${#mx_uninstalled[@]})); then mx_uninstalled_list=$(IFS=,; printf '%s' "${mx_uninstalled[*]}"); fi
printf 'release\t%s\nkernel\t%s\nbackup\t%s\nuninstalled\t%s\nsession_bridge\t%s\ntransport_module\tmxguest\nactivation\tnext boot\n' "$mx_prefix" "$mx_kernel" "$mx_backup_dir" "$mx_uninstalled_list" "$mx_bridge_installed" > "$mx_build_workspace/installed.tsv"
mx_transaction_active=0
mx_discard_retired
for mx_item in ${mx_deferred[@]+"${mx_deferred[@]}"}; do rm -rf -- "$mx_item"; done
mx_summary='the mxgpu and mxguest modules, the graphics stack and the guest agent'
[[ $mx_bridge_installed == none ]] || mx_summary+=" and the session bridge ($mx_bridge_installed)"
printf 'Uninstalled %s previous items and installed %s. Running services and loaded modules were not stopped or unloaded; reboot to activate.\n' "${#mx_uninstalled[@]}" "$mx_summary"
