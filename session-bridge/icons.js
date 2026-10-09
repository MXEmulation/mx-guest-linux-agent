// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GdkPixbuf from 'gi://GdkPixbuf';
import St from 'gi://St';
import {toPremultipliedBgra} from './integration-protocol.js';

export const ICON_SIZE = 64;
const HICOLOR_SIZES = ['scalable', '256x256', '128x128', '64x64', '48x48', '32x32'];
const EXTENSIONS = ['png', 'svg'];

function pixbufToBgra(pixbuf) {
    if (pixbuf.get_colorspace() !== GdkPixbuf.Colorspace.RGB || pixbuf.get_bits_per_sample() !== 8)
        return null;
    const width = pixbuf.get_width();
    const height = pixbuf.get_height();
    const bgra = toPremultipliedBgra(pixbuf.get_pixels(), width, height, pixbuf.get_rowstride(),
        pixbuf.get_n_channels());
    return bgra && {width, height, bgra};
}

export class IconResolver {
    constructor(changed = () => {}) {
        this._cache = new Map();
        this._theme = null;
        this._themeSignal = 0;
        try {
            this._theme = new St.IconTheme();
            this._themeSignal = this._theme.connect('changed', () => {
                this._cache.clear();
                changed();
            });
        } catch (error) {
            console.error(`MX icon theme lookup unavailable, using installed icon directories: ${error.message}`);
        }
        this.ready = this._selfTest();
    }

    _selfTest() {
        try {
            const probe = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, true, 8, 1, 1);
            return pixbufToBgra(probe) !== null;
        } catch (_error) {
            return false;
        }
    }

    // Returns {width, height, bgra} for an icon descriptor of the window inventory, or null.
    resolve(descriptor) {
        const key = descriptor.serialized;
        if (this._cache.has(key))
            return this._cache.get(key);
        let icon = null;
        try {
            icon = this._load(Gio.Icon.new_for_string(key));
        } catch (error) {
            console.error(`MX icon ${key} not resolved: ${error.message}`);
        }
        this._cache.set(key, icon);
        return icon;
    }

    _load(gicon) {
        for (const path of this._paths(gicon)) {
            try {
                const icon = pixbufToBgra(GdkPixbuf.Pixbuf.new_from_file_at_size(path, ICON_SIZE, ICON_SIZE));
                if (icon)
                    return icon;
            } catch (_error) {
                // An unreadable candidate is skipped in favour of the next one.
            }
        }
        return null;
    }

    * _paths(gicon) {
        if (gicon instanceof Gio.FileIcon) {
            const path = gicon.get_file().get_path();
            if (path)
                yield path;
            return;
        }
        if (!(gicon instanceof Gio.ThemedIcon))
            return;
        if (this._theme) {
            let path = null;
            try {
                path = this._theme.lookup_by_gicon(gicon, ICON_SIZE, 0)?.get_filename() ?? null;
            } catch (_error) {
                path = null;
            }
            if (path)
                yield path;
        }
        const roots = [GLib.get_user_data_dir(), ...GLib.get_system_data_dirs()];
        for (const name of gicon.get_names()) {
            for (const root of roots) {
                for (const size of HICOLOR_SIZES) {
                    for (const extension of EXTENSIONS) {
                        if ((size === 'scalable') === (extension === 'svg')) {
                            const path = `${root}/icons/hicolor/${size}/apps/${name}.${extension}`;
                            if (GLib.file_test(path, GLib.FileTest.IS_REGULAR))
                                yield path;
                        }
                    }
                }
                for (const extension of EXTENSIONS) {
                    const path = `${root}/pixmaps/${name}.${extension}`;
                    if (GLib.file_test(path, GLib.FileTest.IS_REGULAR))
                        yield path;
                }
            }
        }
    }

    destroy() {
        if (this._theme && this._themeSignal)
            this._theme.disconnect(this._themeSignal);
        this._themeSignal = 0;
        this._theme = null;
        this._cache.clear();
    }
}
