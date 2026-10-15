// Keeps the on-screen panels equal to the last presented frame. The actor
// factory is injected so reconciliation is testable without GNOME Shell:
//   create(panel) -> handle, update(handle, panel), destroy(handle).
// Every actor the factory makes is non-reactive and never takes input.

function same(a, b) {
    return JSON.stringify(a) === JSON.stringify(b);
}

export class OverlayModel {
    constructor(factory) {
        this._factory = factory;
        this._panels = new Map();
    }

    get size() { return this._panels.size; }

    has(id) { return this._panels.has(id); }

    // Full replacement: panels absent from `panels` are removed, unchanged
    // panels keep their actors, changed ones are updated in place.
    present(panels) {
        const wanted = new Set(panels.map(panel => panel.id));
        for (const [id, entry] of this._panels) {
            if (wanted.has(id)) continue;
            this._factory.destroy(entry.handle);
            this._panels.delete(id);
        }
        for (const panel of panels) {
            const entry = this._panels.get(panel.id);
            if (!entry) {
                this._panels.set(panel.id, {panel, handle: this._factory.create(panel)});
            } else if (!same(entry.panel, panel)) {
                this._factory.update(entry.handle, panel);
                entry.panel = panel;
            }
        }
    }

    clear() {
        for (const entry of this._panels.values()) this._factory.destroy(entry.handle);
        this._panels.clear();
    }
}
