// Window reports for io.github.EnderKeera.LayerC.Shell1.ReportWindows
// (docs/Wayland.md, "Window report contract"). Pure: no Shell, Mutter, or
// bus object reaches this module, so gjs unit tests run it directly.

export const MAX_WINDOWS = 64;
export const MAX_TEXT_BYTES = 512;
export const MAX_ID_BYTES = 128;
export const MAX_REPORT_BYTES = 64 * 1024;
// At most 10 reports per second, measured on GLib's monotonic timeouts.
export const REPORT_INTERVAL_MS = 100;

const INT_MIN = -2147483648;
const INT_MAX = 2147483647;
const UINT_MAX = 4294967295;

function integer(value, low, high) {
    const n = Number(value);
    return Number.isFinite(n) ? Math.max(low, Math.min(high, Math.round(n))) : low;
}

function encodedSize(code) {
    if (code < 0x80) return 1;
    if (code < 0x800) return 2;
    return code < 0x10000 ? 3 : 4;
}

// Valid UTF-8 within `limit` bytes; never splits a character and replaces a
// lone surrogate, exactly like the KWin script.
export function boundedText(value, limit) {
    const source = value === undefined || value === null ? '' : String(value);
    let out = '';
    let bytes = 0;
    for (const piece of source) {
        const code = piece.codePointAt(0);
        const safe = code >= 0xd800 && code <= 0xdfff ? '\uFFFD' : piece;
        const size = encodedSize(safe.codePointAt(0));
        if (bytes + size > limit) break;
        out += safe;
        bytes += size;
    }
    return out;
}

export function utf8Length(text) {
    let bytes = 0;
    for (const piece of text) bytes += encodedSize(piece.codePointAt(0));
    return bytes;
}

// `window` is a plain description: {id, title, wmClass, instance, pid, x, y,
// width, height, active, fullscreen, hidden, x11Window}. null drops a window
// that has no usable identity.
export function windowRow(window) {
    const id = boundedText(window.id, MAX_ID_BYTES);
    if (!id) return null;
    const row = {
        id,
        title: boundedText(window.title, MAX_TEXT_BYTES),
        class: boundedText(window.wmClass, MAX_TEXT_BYTES),
        instance: boundedText(window.instance, MAX_TEXT_BYTES),
        x: integer(window.x, INT_MIN, INT_MAX),
        y: integer(window.y, INT_MIN, INT_MAX),
        width: integer(window.width, 0, INT_MAX),
        height: integer(window.height, 0, INT_MAX),
        active: !!window.active,
        fullscreen: !!window.fullscreen,
        hidden: !!window.hidden,
    };
    if (Number(window.pid) > 0) row.pid = integer(window.pid, 1, UINT_MAX);
    if (Number(window.x11Window) > 0) row.x11Window = integer(window.x11Window, 1, UINT_MAX);
    return row;
}

// A full snapshot, never a delta. Excess windows are left out rather than
// exceeding the daemon's bounds, which would reject the whole report.
export function buildReport(windows) {
    const rows = [];
    const envelope = utf8Length(JSON.stringify({version: 1, source: 'gnome', windows: []}));
    let bytes = envelope;
    for (const window of windows) {
        if (rows.length === MAX_WINDOWS) break;
        const row = windowRow(window);
        if (!row) continue;
        const size = utf8Length(JSON.stringify(row)) + (rows.length ? 1 : 0);
        if (bytes + size > MAX_REPORT_BYTES) break;
        rows.push(row);
        bytes += size;
    }
    return JSON.stringify({version: 1, source: 'gnome', windows: rows});
}

// Coalesces window changes into at most one report per interval with at most
// one call in flight. The platform is injected:
//   schedule(ms, callback) -> handle, cancel(handle),
//   snapshot() -> payload string, send(payload, done).
export class ReportScheduler {
    constructor(platform) {
        this._platform = platform;
        this._present = false;
        this._timer = null;
        this._inFlight = false;
        this._again = false;
        this._generation = 0;
    }

    get present() { return this._present; }

    // The daemon's name appeared or vanished. Nothing is sent while absent.
    setDaemonPresent(present) {
        if (present === this._present) return;
        this._present = present;
        this._cancelTimer();
        this._again = false;
        // A reply for the previous owner must not drive this one.
        ++this._generation;
        this._inFlight = false;
        if (present) this._send();
    }

    changed() {
        if (!this._present) return;
        if (this._inFlight) {
            this._again = true;
            return;
        }
        this._arm();
    }

    destroy() {
        this._present = false;
        this._cancelTimer();
        this._again = false;
        this._inFlight = false;
        ++this._generation;
    }

    _arm() {
        if (this._timer !== null) return;
        this._timer = this._platform.schedule(REPORT_INTERVAL_MS, () => {
            this._timer = null;
            if (this._inFlight) this._again = true;
            else this._send();
        });
    }

    _cancelTimer() {
        if (this._timer === null) return;
        this._platform.cancel(this._timer);
        this._timer = null;
    }

    _send() {
        if (!this._present) return;
        const generation = this._generation;
        this._inFlight = true;
        this._platform.send(this._platform.snapshot(), () => {
            if (generation !== this._generation) return;
            this._inFlight = false;
            if (this._again) {
                this._again = false;
                this._arm();
            }
        });
    }
}
