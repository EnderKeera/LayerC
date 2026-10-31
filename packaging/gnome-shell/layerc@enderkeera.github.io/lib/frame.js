// Overlay frames from io.github.EnderKeera.LayerC.Overlay1.Present
// (docs/Wayland.md, "GNOME Shell extension"). Pure validation: a frame that
// breaks any bound is rejected as a whole and never partially drawn. The
// panel, op, and byte bounds are layerd's (gnome_bridge_frame.hpp); ids and
// labels are bounded only by the frame size, as on the daemon side.

export const MAX_PANELS = 64;
export const MAX_OPS = 512;
export const MAX_FRAME_BYTES = 64 * 1024;
export const MAX_EXTENT = 32768;
export const MAX_FONT_PX = 4096;

const OPS = new Set(['fill', 'stroke', 'text']);
const COLOR = /^#[0-9A-Fa-f]{6}$/;

function utf8Length(text) {
    let bytes = 0;
    for (const piece of text) {
        const code = piece.codePointAt(0);
        bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    }
    return bytes;
}

function coordinate(value) {
    return Number.isInteger(value) && Math.abs(value) <= MAX_EXTENT * 4;
}

function extent(value) {
    return Number.isInteger(value) && value >= 0 && value <= MAX_EXTENT;
}

function parseColor(text) {
    if (typeof text !== 'string' || !COLOR.test(text)) return null;
    const value = Number.parseInt(text.slice(1), 16);
    return [(value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff];
}

function parseOp(op) {
    if (op === null || typeof op !== 'object' || !OPS.has(op.op)) return null;
    if (!coordinate(op.x) || !coordinate(op.y) || !extent(op.width) || !extent(op.height))
        return null;
    const color = parseColor(op.color);
    if (!color) return null;
    const parsed = {op: op.op, x: op.x, y: op.y, width: op.width, height: op.height, color};
    if (op.op === 'text') {
        if (typeof op.text !== 'string') return null;
        if (!Number.isInteger(op.fontPx) || op.fontPx < 0 || op.fontPx > MAX_FONT_PX) return null;
        parsed.text = op.text;
        parsed.fontPx = op.fontPx;
    }
    return parsed;
}

function parsePanel(panel) {
    if (panel === null || typeof panel !== 'object') return null;
    if (typeof panel.id !== 'string' || !panel.id) return null;
    if (!coordinate(panel.x) || !coordinate(panel.y) || !extent(panel.width) ||
        !extent(panel.height) || !Array.isArray(panel.ops) || panel.ops.length > MAX_OPS)
        return null;
    const ops = [];
    for (const op of panel.ops) {
        const parsed = parseOp(op);
        if (!parsed) return null;
        ops.push(parsed);
    }
    return {id: panel.id, x: panel.x, y: panel.y, width: panel.width, height: panel.height, ops};
}

// {panels: [...]} or null. Unknown members are ignored; duplicate panel ids
// are rejected because each id names exactly one on-screen panel.
export function parseFrame(text) {
    if (typeof text !== 'string' || utf8Length(text) > MAX_FRAME_BYTES) return null;
    let frame;
    try {
        frame = JSON.parse(text);
    } catch {
        return null;
    }
    if (frame === null || typeof frame !== 'object' || frame.version !== 1 ||
        !Array.isArray(frame.panels) || frame.panels.length > MAX_PANELS)
        return null;
    const panels = [];
    const ids = new Set();
    for (const panel of frame.panels) {
        const parsed = parsePanel(panel);
        if (!parsed || ids.has(parsed.id)) return null;
        ids.add(parsed.id);
        panels.push(parsed);
    }
    return {panels};
}
