// Runs the real extension glue on an isolated dbus-run-session bus with fake
// Shell/Mutter objects: window reports to a fake daemon, Overlay1 drawing
// and authorization, daemon loss, and disable(). No compositor is used.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {expect, pump, run, settle} from './harness.js';
import {FakeWindow, WindowClientType, WindowType, fakeGlobal, layoutManager} from './fakes/shell.js';

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const source = GLib.build_filenamev([here, '..', '..', 'packaging', 'gnome-shell',
    'layerc@enderkeera.github.io']);
const fakes = `file://${GLib.build_filenamev([here, 'fakes'])}`;

const DAEMON = 'io.github.EnderKeera.LayerC';
const OVERLAY = 'io.github.EnderKeera.LayerC.GnomeShell';
const OVERLAY_PATH = '/io/github/EnderKeera/LayerC/GnomeShell';
const OVERLAY_IFACE = 'io.github.EnderKeera.LayerC.Overlay1';

function read(path) {
    return new TextDecoder().decode(GLib.file_get_contents(path)[1]);
}

// Copies the extension and points its shell-only imports at the fakes.
function stage() {
    const dir = GLib.dir_make_tmp('layerc-gnome-XXXXXX');
    GLib.mkdir_with_parents(`${dir}/lib`, 0o700);
    const rewrites = [
        [`'gi://St'`, `'${fakes}/st.js'`],
        [`'gi://Clutter'`, `'${fakes}/clutter.js'`],
        [`'gi://Meta'`, `'${fakes}/meta.js'`],
        [`'resource:///org/gnome/shell/ui/main.js'`, `'${fakes}/shell.js'`],
        [`'resource:///org/gnome/shell/extensions/extension.js'`, `'${fakes}/shell.js'`],
    ];
    let glue = read(`${source}/extension.js`);
    for (const [from, to] of rewrites) {
        expect(glue.includes(from), `extension imports ${from}`);
        glue = glue.split(from).join(to);
    }
    GLib.file_set_contents(`${dir}/extension.js`, glue);
    for (const name of ['report.js', 'frame.js', 'overlay.js', 'service.js'])
        GLib.file_set_contents(`${dir}/lib/${name}`, read(`${source}/lib/${name}`));
    return dir;
}

function call(connection, name, path, iface, method, parameters) {
    const box = {done: false, value: null, error: null};
    connection.call(name, path, iface, method, parameters, null, Gio.DBusCallFlags.NONE, 2000,
        null, (conn, result) => {
            try {
                box.value = conn.call_finish(result);
            } catch (error) {
                box.error = Gio.DBusError.get_remote_error(error) ?? String(error);
            }
            box.done = true;
        });
    pump(() => box.done);
    return box;
}

function owner(name) {
    const box = call(Gio.DBus.session, 'org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'GetNameOwner', new GLib.Variant('(s)', [name]));
    return box.error ? null : box.value.deep_unpack()[0];
}

const DAEMON_XML = `<node><interface name="io.github.EnderKeera.LayerC.Shell1">
  <method name="ReportWindows"><arg type="s" direction="in"/></method></interface></node>`;

const PANEL = {id: 'p', x: 10, y: 20, width: 200, height: 100, ops: [
    {op: 'fill', x: 0, y: 0, width: 200, height: 100, color: '#101820'},
    {op: 'stroke', x: 0, y: 0, width: 200, height: 100, color: '#D88E4B'},
    {op: 'text', x: 4, y: 4, width: 60, height: 20, color: '#FFFFFF', text: 'Q', fontPx: 12},
]};

const game = new FakeWindow(11, {focus: true, rect: {x: -10, y: 20, width: 800, height: 600}});
const legacy = new FakeWindow(12, {client: WindowClientType.X11, description: '0x2a00003',
    title: 'Legacy', pid: 77});
const dock = new FakeWindow(13, {type: WindowType.DOCK});
globalThis.global = fakeGlobal([game, legacy, dock]);

const directory = stage();
const {default: LayerCExtension} = await import(`file://${directory}/extension.js`);
const metadata = JSON.parse(read(`${source}/metadata.json`));
const extension = new LayerCExtension(metadata);
const reports = [];
const daemon = {
    ReportWindowsAsync([payload], invocation) {
        reports.push(JSON.parse(payload));
        invocation.return_value(null);
    },
};
let daemonExport = null;
let daemonOwn = 0;

run([
    ['metadata_declares_gnome_45_to_50', () => {
        expect(metadata.uuid === 'layerc@enderkeera.github.io', 'uuid matches the install directory');
        expect(JSON.stringify(metadata['shell-version']) === '["45","46","47","48","49","50"]',
            'GNOME Shell 45-50 are declared');
        expect(!('url' in metadata), 'metadata carries no URL');
    }],
    ['enable_exports_overlay_and_waits_for_the_daemon', () => {
        extension.enable();
        expect(layoutManager.chrome.length === 1 && layoutManager.chrome[0].reactive === false &&
               layoutManager.options[0].affectsInputRegion === false,
        'one non-reactive layer outside the input region');
        expect(pump(() => owner(OVERLAY) !== null), 'the extension owns the Overlay1 name');
        settle(250);
        expect(reports.length === 0, 'nothing is reported while the daemon is absent');
        const version = call(Gio.DBus.session, OVERLAY, OVERLAY_PATH,
            'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', [OVERLAY_IFACE, 'Version']));
        expect(!version.error && version.value.deep_unpack()[0].deep_unpack() === 1,
            'Version is 1');
    }],
    ['reports_follow_the_daemon_name', () => {
        daemonExport = Gio.DBusExportedObject.wrapJSObject(DAEMON_XML, daemon);
        daemonExport.export(Gio.DBus.session, '/io/github/EnderKeera/LayerC');
        daemonOwn = Gio.bus_own_name_on_connection(Gio.DBus.session, DAEMON,
            Gio.BusNameOwnerFlags.NONE, null, null);
        expect(pump(() => reports.length === 1), 'an initial snapshot follows the name appearing');
        const [first] = reports;
        const rows = first?.windows ?? [];
        expect(first?.source === 'gnome' && rows.length === 2, 'only normal windows are reported');
        const native = rows.find(row => row.id === '11');
        const xwayland = rows.find(row => row.id === '12');
        expect(native?.active === true && native.x === -10 && !('x11Window' in native),
            'a native window has geometry, focus, and no XID');
        expect(xwayland?.x11Window === 0x2a00003 && xwayland.pid === 77,
            'an X11 window carries its XID and PID');
        for (let i = 0; i < 50; ++i) {
            game.title = `Game ${i}`;
            game.emit('notify::title');
        }
        expect(pump(() => reports.at(-1)?.windows.find(row => row.id === '11')?.title === 'Game 49'),
            'title changes reach the daemon');
        settle(300);
        expect(reports.length <= 3, `changes coalesce (${reports.length} reports for 50 changes)`);
        const before = reports.length;
        const added = new FakeWindow(14, {title: 'New'});
        global.windows.push(added);
        global.display.emit('window-created', added);
        expect(pump(() => reports.length > before &&
                          reports.at(-1).windows.some(row => row.id === '14')),
        'a created window is reported');
        global.windows.pop();
        added.emit('unmanaged');
        expect(pump(() => !reports.at(-1).windows.some(row => row.id === '14')) &&
               added.connections === 0, 'an unmanaged window is dropped and disconnected');
    }],
    ['only_the_daemon_draws_panels', () => {
        const address = GLib.getenv('DBUS_SESSION_BUS_ADDRESS');
        const rogue = Gio.DBusConnection.new_for_address_sync(address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, null, null);
        const frame = JSON.stringify({version: 1, panels: [PANEL]});
        const denied = call(rogue, OVERLAY, OVERLAY_PATH, OVERLAY_IFACE, 'Present',
            new GLib.Variant('(s)', [frame]));
        const layer = layoutManager.chrome[0];
        expect(denied.error === 'org.freedesktop.DBus.Error.AccessDenied' &&
               layer.children.length === 0, 'another client cannot draw');
        rogue.close_sync(null);
        const shown = call(Gio.DBus.session, OVERLAY, OVERLAY_PATH, OVERLAY_IFACE, 'Present',
            new GLib.Variant('(s)', [frame]));
        const [panel] = layer.children;
        expect(!shown.error && layer.children.length === 1 && panel.x === 10 &&
               panel.children.length === 3, 'the daemon draws one panel with its ops');
        const all = [panel, ...panel.children, ...panel.children.flatMap(child => child.children)];
        expect(all.every(actor => actor.reactive === false), 'every panel actor is non-reactive');
        const label = panel.children[2].children[0];
        expect(label?.text === 'Q' && /font-size: 12px/.test(label.style), 'labels keep text and size');
        const bad = call(Gio.DBus.session, OVERLAY, OVERLAY_PATH, OVERLAY_IFACE, 'Present',
            new GLib.Variant('(s)', ['{"version":1']));
        expect(bad.error === 'org.freedesktop.DBus.Error.InvalidArgs' && layer.children.length === 1,
            'an invalid frame keeps the last good one');
        const cleared = call(Gio.DBus.session, OVERLAY, OVERLAY_PATH, OVERLAY_IFACE, 'Clear', null);
        expect(!cleared.error && layer.children.length === 0 && panel.destroyed, 'Clear removes panels');
        call(Gio.DBus.session, OVERLAY, OVERLAY_PATH, OVERLAY_IFACE, 'Present',
            new GLib.Variant('(s)', [frame]));
        expect(layer.children.length === 1, 'panels are presented again');
    }],
    ['daemon_loss_clears_and_silences', () => {
        const layer = layoutManager.chrome[0];
        Gio.bus_unown_name(daemonOwn);
        expect(pump(() => layer.children.length === 0), 'panels are removed when the daemon vanishes');
        const before = reports.length;
        game.emit('notify::title');
        settle(300);
        expect(reports.length === before, 'nothing is reported after the daemon vanished');
        daemonExport.unexport();
    }],
    ['disable_releases_everything', () => {
        const layer = layoutManager.chrome[0];
        extension.disable();
        expect(layoutManager.chrome.length === 0 && layer.destroyed, 'the layer is removed');
        expect(global.display.connections === 0 && game.connections === 0 &&
               legacy.connections === 0, 'every signal is disconnected');
        expect(pump(() => owner(OVERLAY) === null), 'the Overlay1 name is released');
    }],
    ['remove_staged_copy', () => {
        for (const name of ['report.js', 'frame.js', 'overlay.js', 'service.js'])
            GLib.unlink(`${directory}/lib/${name}`);
        GLib.rmdir(`${directory}/lib`);
        GLib.unlink(`${directory}/extension.js`);
        expect(GLib.rmdir(directory) === 0, 'the staged copy is removed');
    }],
]);
