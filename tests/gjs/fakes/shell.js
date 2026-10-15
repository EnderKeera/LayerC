// Test doubles for the GNOME Shell and Mutter APIs the extension glue uses.
// test_extension.js rewrites the extension's shell imports to these modules;
// GLib, Gio and the session bus stay real (dbus-run-session).

export class Signals {
    constructor() {
        this._handlers = new Map();
        this._next = 1;
    }

    connect(name, callback) {
        const id = this._next++;
        this._handlers.set(id, {name, callback});
        return id;
    }

    disconnect(id) { this._handlers.delete(id); }

    get connections() { return this._handlers.size; }

    emit(name, ...args) {
        for (const {name: handler, callback} of [...this._handlers.values()])
            if (handler === name) callback(this, ...args);
    }
}

export class Widget {
    constructor(props = {}) {
        Object.assign(this, {reactive: true, x: 0, y: 0, width: 0, height: 0}, props);
        this.children = [];
        this.destroyed = false;
        this.parent = null;
    }

    add_child(child) {
        child.parent = this;
        this.children.push(child);
    }

    set_child(child) {
        this.children = [];
        this.add_child(child);
    }

    destroy_all_children() {
        for (const child of this.children) child.destroy();
        this.children = [];
    }

    set_position(x, y) { Object.assign(this, {x, y}); }

    set_size(width, height) { Object.assign(this, {width, height}); }

    destroy() {
        this.destroyed = true;
        this.destroy_all_children();
        if (this.parent) {
            this.parent.children = this.parent.children.filter(child => child !== this);
            this.parent = null;
        }
    }
}

export class Bin extends Widget {}

export class Label extends Widget {}

export const layoutManager = {
    chrome: [],
    options: [],
    addTopChrome(actor, options) {
        this.chrome.push(actor);
        this.options.push(options);
    },
    removeChrome(actor) {
        this.chrome = this.chrome.filter(entry => entry !== actor);
    },
};

export class Extension {
    constructor(metadata) {
        this.metadata = metadata;
    }
}

export const WindowClientType = {WAYLAND: 0, X11: 1};
export const WindowType = {NORMAL: 0, DOCK: 1};
export const ActorAlign = {FILL: 0, START: 1, CENTER: 3, END: 2};

export class FakeWindow extends Signals {
    constructor(sequence, props = {}) {
        super();
        Object.assign(this, {
            sequence, title: 'Game', wmClass: 'game', instance: 'game-bin', pid: 4242,
            rect: {x: 0, y: 0, width: 800, height: 600}, focus: false, fullscreen: false,
            minimized: false, type: WindowType.NORMAL, skip: false,
            client: WindowClientType.WAYLAND, description: 'Game',
        }, props);
    }

    get_stable_sequence() { return this.sequence; }
    get_title() { return this.title; }
    get_wm_class() { return this.wmClass; }
    get_wm_class_instance() { return this.instance; }
    get_pid() { return this.pid; }
    get_frame_rect() { return this.rect; }
    has_focus() { return this.focus; }
    is_fullscreen() { return this.fullscreen; }
    get_window_type() { return this.type; }
    is_skip_taskbar() { return this.skip; }
    get_client_type() { return this.client; }
    get_description() { return this.description; }
}

export function fakeGlobal(windows) {
    const display = new Signals();
    return {
        display,
        windows,
        get_window_actors() {
            return this.windows.map(window => ({get_meta_window: () => window}));
        },
    };
}
