// Executes the actual embedded script against fake KWin 6/5.27 objects.
// No compositor, desktop IPC or production state.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

function expect(value, message) {
    if (!value) throw new Error(message);
}
const [, bytes] = Gio.File.new_for_path(ARGV[0]).load_contents(null);
const source = new TextDecoder().decode(bytes).split('R"JS(')[1].split(')JS"')[0];
function signal() {
    const callbacks = [];
    return {
        connect(callback) { callbacks.push(callback); },
        emit(...args) { for (const callback of callbacks) callback(...args); },
    };
}
function window(id) {
    return {
        internalId: id, windowId: 7, pid: 42, normalWindow: true,
        caption: 'Game', resourceClass: 'game', resourceName: 'game-instance',
        frameGeometry: {x: -100, y: 20, width: 800, height: 600},
        fullScreen: false, minimized: false,
        captionChanged: signal(), frameGeometryChanged: signal(),
        fullScreenChanged: signal(), minimizedChanged: signal(),
    };
}
for (const modern of [true, false]) {
    const game = window('stable-id');
    const dock = window('dock');
    dock.normalWindow = false;
    const all = [game, dock];
    const ws = modern ? {windowList: () => all, activeWindow: game} :
                        {clientList: () => all, activeClient: game};
    const activated = modern ? 'windowActivated' : 'clientActivated';
    const added = modern ? 'windowAdded' : 'clientAdded';
    const removed = modern ? 'windowRemoved' : 'clientRemoved';
    ws[activated] = signal(); ws[added] = signal(); ws[removed] = signal();
    const frames = [], pending = [];
    const dbus = (name, path, iface, method, payload, callback) => {
        expect(name === 'io.github.EnderKeera.LayerC' &&
               path === '/io/github/EnderKeera/LayerC' &&
               iface === 'io.github.EnderKeera.LayerC.Shell1' && method === 'ReportWindows',
               'script calls only the documented local endpoint');
        expect(new TextEncoder().encode(payload).length <= 65536, 'payload is bounded');
        frames.push(JSON.parse(payload));
        pending.push(callback);
    };
    const finish = () => {
        expect(pending.length === 1, 'at most one request is in flight');
        pending.shift()();
    };
    new Function('workspace', 'callDBus', source)(ws, dbus);
    expect(frames.length === 1 && frames[0].windows.length === 1, 'initial normal-window snapshot');
    let row = frames[0].windows[0];
    expect(row.id === 'stable-id' && row.active && row.x === -100 && row.width === 800 &&
           row.pid === 42 && row.x11Window === 7, 'logical fields and optional identifiers');
    finish();
    game.caption = 'changed'; game.fullScreen = true; game.minimized = true;
    game.captionChanged.emit();
    game.fullScreenChanged.emit(); game.minimizedChanged.emit(); ws[activated].emit();
    expect(pending.length === 1, 'bursty signals coalesce while in flight');
    finish(); finish();
    row = frames[frames.length - 1].windows[0];
    expect(row.title === 'changed' && row.fullscreen && row.hidden, 'new full replacement snapshot');

    // KWin emits remove before removing it from its own list.
    ws[removed].emit(game);
    all.splice(0, 1);
    expect(frames[frames.length - 1].windows.length === 0, 'removed window excluded immediately');
    finish();
    all.unshift(game); ws[added].emit(game); finish();
    game.caption = '\ud800' + '😀'.repeat(500);
    game.captionChanged.emit(); finish();
    row = frames[frames.length - 1].windows[0];
    expect(new TextEncoder().encode(row.title).length <= 512 &&
           !/[\ud800-\udfff]/u.test(row.title), 'UTF-8 truncation preserves complete characters');

    const many = Array.from({length: 70}, (_, i) => {
        const w = window('w' + i);
        w.caption = '\\'.repeat(512); w.resourceClass = '\\'.repeat(512);
        w.resourceName = '\\'.repeat(512);
        return w;
    });
    all.splice(0, all.length, ...many);
    ws[activated].emit(); finish();
    expect(frames[frames.length - 1].windows.length > 0, 'bounded large reports still retain windows');
}
print('KWin 6/5.27 embedded-script tests passed');
