// Lexical boundary: the shipped extension uses only local session-bus IPC.
// Every file under the extension directory is scanned, so a new module
// cannot add a network client unnoticed.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {expect, run} from './harness.js';

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const root = GLib.build_filenamev([here, '..', '..', 'packaging', 'gnome-shell',
    'layerc@enderkeera.github.io']);

const FORBIDDEN = [
    [/\bSoup\b/, 'libsoup'],
    [/SocketClient|SocketService|SocketListener|Gio\.Socket\b/, 'raw sockets'],
    [/\bResolver\b|NetworkMonitor|NetworkAddress|ProxyResolver/, 'name resolution or network state'],
    [/https?:\/\//i, 'URLs'],
    [/\bfetch\s*\(|XMLHttpRequest|WebSocket/, 'web requests'],
    [/BusType\.SYSTEM|DBus\.system/, 'the system bus'],
    [/\bsubprocess\b|spawn_|Subprocess/i, 'child processes'],
];

function files(directory) {
    const found = [];
    const enumerator = Gio.File.new_for_path(directory).enumerate_children(
        'standard::name,standard::type', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
    for (let info = enumerator.next_file(null); info; info = enumerator.next_file(null)) {
        const path = GLib.build_filenamev([directory, info.get_name()]);
        if (info.get_file_type() === Gio.FileType.DIRECTORY) found.push(...files(path));
        else found.push(path);
    }
    return found;
}

run([
    ['extension_has_no_network_or_process_access', () => {
        const all = files(root);
        expect(all.some(path => path.endsWith('extension.js')) &&
               all.some(path => path.endsWith('metadata.json')), 'the extension files are scanned');
        for (const path of all) {
            const text = new TextDecoder().decode(GLib.file_get_contents(path)[1]);
            for (const [pattern, what] of FORBIDDEN)
                expect(!pattern.test(text), `${GLib.path_get_basename(path)} uses no ${what}`);
        }
    }],
]);
