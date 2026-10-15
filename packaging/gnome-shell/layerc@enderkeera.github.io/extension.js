// LayerC Shell extension (docs/Wayland.md, "GNOME Shell extension").
// Reports normal windows to layerd and draws its overlay panels with
// non-reactive actors. Local session-bus IPC only; it never accesses the
// network and does nothing while the LayerC daemon is absent.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {ReportScheduler, buildReport} from './lib/report.js';
import {OverlayModel} from './lib/overlay.js';
import {
    DAEMON_NAME, DAEMON_PATH, SHELL_INTERFACE, OVERLAY_NAME, OVERLAY_PATH, OVERLAY_XML,
    OverlayService,
} from './lib/service.js';

const WINDOW_SIGNALS = [
    'notify::title', 'notify::wm-class', 'notify::minimized', 'notify::fullscreen',
    'position-changed', 'size-changed',
];

function rgb([r, g, b]) {
    return `rgb(${r},${g},${b})`;
}

// X11 clients describe themselves as "0x<xid>"; native clients carry no XID.
function x11Window(window) {
    if (window.get_client_type() !== Meta.WindowClientType.X11) return 0;
    const match = /^0x([0-9a-f]+)$/i.exec(window.get_description() ?? '');
    return match ? Number.parseInt(match[1], 16) : 0;
}

function describe(window) {
    const rect = window.get_frame_rect();
    return {
        id: String(window.get_stable_sequence()),
        title: window.get_title(),
        wmClass: window.get_wm_class(),
        instance: window.get_wm_class_instance(),
        pid: window.get_pid(),
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
        active: window.has_focus(),
        fullscreen: window.is_fullscreen(),
        hidden: window.minimized,
        x11Window: x11Window(window),
    };
}

function normal(window) {
    return window && window.get_window_type() === Meta.WindowType.NORMAL &&
        !window.is_skip_taskbar();
}

export default class LayerCExtension extends Extension {
    enable() {
        this._daemonOwner = null;
        this._cancellable = new Gio.Cancellable();
        this._windows = new Map();
        this._displaySignals = [];

        this._layer = new St.Widget({name: 'layercOverlay', reactive: false, can_focus: false});
        Main.layoutManager.addTopChrome(this._layer, {affectsInputRegion: false});
        this._model = new OverlayModel({
            create: panel => this._createPanel(panel),
            update: (actor, panel) => this._paintPanel(actor, panel),
            destroy: actor => actor.destroy(),
        });
        this._service = new OverlayService(this._model, () => this._daemonOwner);
        this._exported = Gio.DBusExportedObject.wrapJSObject(OVERLAY_XML, this._service);
        this._exported.export(Gio.DBus.session, OVERLAY_PATH);
        this._ownId = Gio.bus_own_name_on_connection(
            Gio.DBus.session, OVERLAY_NAME, Gio.BusNameOwnerFlags.NONE, null, null);

        this._scheduler = new ReportScheduler({
            schedule: (ms, callback) => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
                callback();
                return GLib.SOURCE_REMOVE;
            }),
            cancel: id => GLib.source_remove(id),
            snapshot: () => buildReport(this._normalWindows().map(describe)),
            send: (payload, done) => this._send(payload, done),
        });
        const display = global.display;
        this._displaySignals.push(
            display.connect('window-created', (_display, window) => {
                this._watch(window);
                this._scheduler.changed();
            }),
            display.connect('notify::focus-window', () => this._scheduler.changed()));
        for (const actor of global.get_window_actors()) this._watch(actor.get_meta_window());

        this._watchId = Gio.bus_watch_name(
            Gio.BusType.SESSION, DAEMON_NAME, Gio.BusNameWatcherFlags.NONE,
            (_connection, _name, owner) => {
                this._daemonOwner = owner;
                this._scheduler.setDaemonPresent(true);
            },
            () => {
                this._daemonOwner = null;
                this._scheduler.setDaemonPresent(false);
                this._service.daemonVanished();
            });
    }

    disable() {
        Gio.bus_unwatch_name(this._watchId);
        this._scheduler.destroy();
        this._cancellable.cancel();
        for (const id of this._displaySignals) global.display.disconnect(id);
        for (const [window, ids] of this._windows)
            for (const id of ids) window.disconnect(id);
        this._windows.clear();
        this._model.clear();
        Gio.bus_unown_name(this._ownId);
        this._exported.unexport();
        Main.layoutManager.removeChrome(this._layer);
        this._layer.destroy();
        this._daemonOwner = null;
        this._layer = this._model = this._service = this._exported = null;
        this._scheduler = this._cancellable = null;
    }

    _normalWindows() {
        return global.get_window_actors().map(actor => actor.get_meta_window()).filter(normal);
    }

    _watch(window) {
        if (!window || this._windows.has(window)) return;
        const ids = WINDOW_SIGNALS.map(name => window.connect(name, () => this._scheduler.changed()));
        ids.push(window.connect('unmanaged', () => {
            for (const id of this._windows.get(window) ?? []) window.disconnect(id);
            this._windows.delete(window);
            this._scheduler.changed();
        }));
        this._windows.set(window, ids);
    }

    // Targets the authenticated unique owner, never activates a daemon, and
    // never retries: the next window change sends a fresh snapshot.
    _send(payload, done) {
        if (!this._daemonOwner) {
            done();
            return;
        }
        Gio.DBus.session.call(
            this._daemonOwner, DAEMON_PATH, SHELL_INTERFACE, 'ReportWindows',
            new GLib.Variant('(s)', [payload]), null, Gio.DBusCallFlags.NO_AUTO_START, -1,
            this._cancellable, (connection, result) => {
                try {
                    connection.call_finish(result);
                } catch {
                    // A rejected or cancelled report changes nothing locally.
                }
                done();
            });
    }

    _createPanel(panel) {
        const actor = new St.Widget({reactive: false, can_focus: false});
        this._paintPanel(actor, panel);
        this._layer.add_child(actor);
        return actor;
    }

    // Logical stage coordinates, exactly as layerd built the display list.
    _paintPanel(actor, panel) {
        actor.destroy_all_children();
        actor.set_position(panel.x, panel.y);
        actor.set_size(panel.width, panel.height);
        for (const op of panel.ops) actor.add_child(this._opActor(op));
    }

    _opActor(op) {
        const geometry = {x: op.x, y: op.y, width: op.width, height: op.height};
        if (op.op === 'fill') {
            return new St.Widget({...geometry, reactive: false,
                style: `background-color: ${rgb(op.color)};`});
        }
        if (op.op === 'stroke') {
            return new St.Widget({...geometry, reactive: false,
                style: `border: 1px solid ${rgb(op.color)};`});
        }
        const bin = new St.Bin({...geometry, reactive: false,
            x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER});
        bin.set_child(new St.Label({text: op.text, reactive: false,
            style: `color: ${rgb(op.color)}; font-size: ${Math.max(1, op.fontPx)}px; font-family: monospace;`}));
        return bin;
    }
}
