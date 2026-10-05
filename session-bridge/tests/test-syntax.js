// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
if (ARGV.length !== 1)
    throw new Error('Expected an extension source or package directory');
const root = Gio.File.new_for_path(ARGV[0]);
const directory = root.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE, null);
let checked = 0;
try {
    let info;
    while ((info = directory.next_file(null))) {
        if (!info.get_name().endsWith('.js'))
            continue;
        const [, bytes] = root.get_child(info.get_name()).load_contents(null);
        Reflect.parse(new TextDecoder().decode(bytes), {target: 'module'});
        checked++;
    }
} finally {
    directory.close(null);
}
if (!checked)
    throw new Error('No JavaScript modules checked');
print(`PASS parsed ${checked} original extension modules without importing or enabling Shell code`);
