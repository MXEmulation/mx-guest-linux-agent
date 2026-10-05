// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import {Files} from './files.js';
import {recognizeConnector} from './sysfs.js';
import {pciIdentity} from './identity.js';

const APPLICATION_TYPES = new Set([Meta.WindowType.NORMAL, Meta.WindowType.DIALOG,
    Meta.WindowType.MODAL_DIALOG, Meta.WindowType.UTILITY, Meta.WindowType.TOOLBAR,
    Meta.WindowType.SPLASHSCREEN]);

export function rectangle(value) {
    return {x: value.x, y: value.y, width: value.width, height: value.height};
}

export function iconDescriptor(icon) {
    if (!icon)
        return null;
    const descriptor = {serialized: icon.to_string()};
    if (icon instanceof Gio.ThemedIcon)
        return {...descriptor, kind: 'themed', names: icon.get_names()};
    if (icon instanceof Gio.FileIcon)
        return {...descriptor, kind: 'file', uri: icon.get_file().get_uri()};
    return {...descriptor, kind: 'gicon', type: icon.constructor.name};
}

function desktopRevision(info) {
    const [loaded, contents] = Gio.File.new_for_path(info.get_filename()).load_contents(null);
    if (!loaded)
        throw new Error('Installed desktop entry is no longer readable');
    return GLib.compute_checksum_for_data(GLib.ChecksumType.SHA256, contents);
}

export class Backend {
    constructor(shellGlobal) {
        this.global = shellGlobal;
        this.tracker = Shell.WindowTracker.get_default();
        this.monitorManager = shellGlobal.backend.get_monitor_manager();
        this.files = new Files();
    }

    includes(window) {
        return !window.is_override_redirect() && APPLICATION_TYPES.has(window.get_window_type());
    }

    windows() {
        return this.global.display.list_all_windows().filter(window => this.includes(window));
    }

    scan() {
        const display = this.global.display;
        const physical = this.monitorManager.get_monitors();
        const monitors = [];
        for (let index = 0; index < display.get_n_monitors(); index++) {
            const outputs = physical.filter(monitor =>
                this.monitorManager.get_monitor_for_connector(monitor.get_connector()) === index)
                .map(monitor => ({connector: monitor.get_connector(), active: monitor.is_active(),
                    display_name: monitor.get_display_name(), vendor: monitor.get_vendor(),
                    product: monitor.get_product(), serial: monitor.get_serial(),
                    device: recognizeConnector(this.files, monitor.get_connector(), pciIdentity)}))
                .sort((a, b) => a.connector.localeCompare(b.connector));
            monitors.push({index, geometry: rectangle(display.get_monitor_geometry(index)),
                scale: display.get_monitor_scale(index), primary: index === display.get_primary_monitor(),
                outputs});
        }
        const windows = display.sort_windows_by_stacking(this.windows()).map((window, stackIndex) => {
            const app = this.tracker.get_window_app(window);
            const actor = window.get_compositor_private();
            const shapedTexture = actor?.get_texture();
            const texture = shapedTexture?.get_texture();
            const clientType = window.get_client_type();
            if (clientType !== Meta.WindowClientType.WAYLAND && clientType !== Meta.WindowClientType.X11)
                throw new Error('Unknown native window client type');
            const workspace = window.get_workspace();
            return {handle: window, native_sequence: window.get_stable_sequence(),
                client_type: clientType === Meta.WindowClientType.WAYLAND ? 'wayland' : 'x11',
                window_type: window.get_window_type(), stack_index: stackIndex,
                title: window.get_title(), app_id: app?.get_id() ?? window.get_gtk_application_id(),
                app_name: app?.get_name() ?? null, icon: iconDescriptor(app?.get_icon()),
                gtk_application_id: window.get_gtk_application_id(), wm_class: window.get_wm_class(),
                wm_class_instance: window.get_wm_class_instance(), pid: window.get_pid(),
                frame: rectangle(window.get_frame_rect()), buffer: rectangle(window.get_buffer_rect()),
                client_content: rectangle(window.get_client_content_rect()),
                texture_pixels: texture ? {width: texture.get_width(), height: texture.get_height()} : null,
                monitor_index: window.get_monitor(), workspace: workspace?.index() ?? null,
                all_workspaces: window.is_on_all_workspaces(), focused: window.has_focus(),
                minimized: window.minimized, hidden: window.is_hidden(),
                showing_on_workspace: window.showing_on_its_workspace(), fullscreen: window.is_fullscreen(),
                mapped: window.mapped, can_close: window.can_close()};
        });
        return {coordinate_space: 'mutter-stage', monitors, windows};
    }

    activate(window) {
        const workspace = window.get_workspace() ?? this.global.workspace_manager.get_active_workspace();
        window.activate_with_workspace(this.global.get_current_time(), workspace);
    }

    close(window) {
        if (!window.can_close())
            throw new Error('Window does not permit close');
        window.delete(this.global.get_current_time());
    }

    catalogue() {
        return Gio.AppInfo.get_all().filter(info => info instanceof GioUnix.DesktopAppInfo &&
            info.get_id() && info.should_show() && !info.get_is_hidden() && !info.get_nodisplay())
            .map(info => ({handle: info, id: info.get_id(), name: info.get_name(),
                display_name: info.get_display_name(), comment: info.get_description(),
                generic_name: info.get_generic_name(), icon: iconDescriptor(info.get_icon()),
                categories: (info.get_categories() ?? '').split(';').filter(Boolean),
                keywords: info.get_keywords() ?? [], desktop_file: info.get_filename(),
                desktop_entry_sha256: desktopRevision(info),
                can_launch: Boolean(info.get_executable()) || info.get_boolean('DBusActivatable'),
                dbus_activatable: info.get_boolean('DBusActivatable'), supports_files: info.supports_files(),
                supports_uris: info.supports_uris(), terminal: info.get_boolean('Terminal')}));
    }

    launch(info) {
        const context = this.global.create_app_launch_context(this.global.get_current_time(), -1);
        if (!info.launch([], context))
            throw new Error('Desktop application launch request failed');
    }
}
