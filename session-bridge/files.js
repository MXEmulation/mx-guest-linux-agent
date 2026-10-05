// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';

function absent(error) {
    return typeof error.matches === 'function' &&
        (error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND) ||
        error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_DIRECTORY) ||
        error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.IS_DIRECTORY));
}

export class Files {
    read(path) {
        try {
            const [success, bytes] = Gio.File.new_for_path(path).load_contents(null);
            if (!success)
                throw new Error(`Unable to read ${path}`);
            return new TextDecoder('utf-8', {fatal: true}).decode(bytes);
        } catch (error) {
            if (absent(error))
                return null;
            throw error;
        }
    }

    link(path) {
        try {
            const info = Gio.File.new_for_path(path).query_info('standard::symlink-target',
                Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
            return info.has_attribute('standard::symlink-target') ? info.get_symlink_target() : null;
        } catch (error) {
            if (absent(error))
                return null;
            throw error;
        }
    }

    list(path) {
        const directory = Gio.File.new_for_path(path);
        const enumerator = directory.enumerate_children('standard::name',
            Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
        const names = [];
        try {
            let info;
            while ((info = enumerator.next_file(null)))
                names.push(info.get_name());
        } finally {
            enumerator.close(null);
        }
        return names;
    }
}
