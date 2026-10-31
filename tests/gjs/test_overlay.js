// Frame validation, panel reconciliation, and Overlay1 authorization.
import {expect, run} from './harness.js';
import {
    MAX_FRAME_BYTES, MAX_OPS, MAX_PANELS, parseFrame,
} from '../../packaging/gnome-shell/layerc@enderkeera.github.io/lib/frame.js';
import {OverlayModel} from '../../packaging/gnome-shell/layerc@enderkeera.github.io/lib/overlay.js';
import {
    ACCESS_DENIED, INVALID_ARGS, OverlayService,
} from '../../packaging/gnome-shell/layerc@enderkeera.github.io/lib/service.js';

// The member order and spelling layerd's build_gnome_overlay_frame emits.
function panel(id, extra = {}) {
    return {id, x: 10, y: 20, width: 200, height: 100, ops: [
        {op: 'fill', x: 0, y: 0, width: 200, height: 100, color: '#101820'},
        {op: 'stroke', x: 0, y: 0, width: 200, height: 100, color: '#D88E4B'},
        {op: 'text', x: 4, y: 4, width: 60, height: 20, color: '#ffffff', text: 'Q 3.2', fontPx: 12},
    ], ...extra};
}

function frame(panels) {
    return JSON.stringify({version: 1, panels});
}

function fakeFactory() {
    return {
        created: 0, updated: 0, destroyed: 0,
        create(p) { ++this.created; return {panel: p}; },
        update(handle, p) { ++this.updated; handle.panel = p; },
        destroy() { ++this.destroyed; },
    };
}

function invocation(sender) {
    return {
        sender, error: null, returned: false,
        get_sender() { return this.sender; },
        return_value() { this.returned = true; },
        return_dbus_error(name) { this.error = name; },
    };
}

run([
    ['frames_parse_as_layerd_builds_them', () => {
        const parsed = parseFrame(frame([panel('a'), panel('b')]));
        expect(parsed && parsed.panels.length === 2, 'a valid frame parses');
        const [fill, , text] = parsed.panels[0].ops;
        expect(fill.color[0] === 0x10 && fill.color[1] === 0x18 && fill.color[2] === 0x20,
            '#RRGGBB colors become channel triples');
        expect(text.text === 'Q 3.2' && text.fontPx === 12, 'text ops keep label and font size');
        expect(parseFrame(frame([]))?.panels.length === 0, 'an empty frame is valid');
        const long = parseFrame(frame([panel('k'.repeat(2000), {ops: [{op: 'text', x: 0, y: 0,
            width: 1, height: 1, color: '#000000', text: 'L'.repeat(2000), fontPx: 0}]})]));
        expect(long?.panels[0].ops[0].text.length === 2000,
            'ids and labels are bounded only by the frame size, as in layerd');
    }],
    ['typed_item_panels_fit_the_raised_op_bound', () => {
        expect(MAX_OPS === 512, 'the op bound matches layerd (gnome_frame_max_ops)');
        // The largest legal typed panel as layerd builds it: background and
        // border, then 32 rows of progress fill, outline, eight pips and label.
        const ops = [
            {op: 'fill', x: 0, y: 0, width: 272, height: 1300, color: '#272D35'},
            {op: 'stroke', x: 0, y: 0, width: 272, height: 1300, color: '#50BAC8'},
        ];
        for (let row = 0; row < 32; ++row) {
            const y = 6 + row * 40;
            ops.push({op: 'fill', x: 8, y: y + 2, width: 126, height: 30, color: '#38A97C'});
            ops.push({op: 'stroke', x: 6, y, width: 260, height: 34, color: '#50BAC8'});
            for (let pip = 0; pip < 8; ++pip) {
                ops.push({op: pip < 4 ? 'fill' : 'stroke', x: 14 + pip * 11, y: y + 13,
                    width: 8, height: 8, color: '#38A97C'});
            }
            ops.push({op: 'text', x: 104, y: y + 2, width: 154, height: 30, color: '#E4E8EC',
                text: `Charges ${row}  4/8`, fontPx: 13});
        }
        expect(ops.length === 354, 'the legal maximum is 354 ops');
        const parsed = parseFrame(frame([panel('pips', {width: 272, height: 1300, ops})]));
        expect(parsed?.panels[0].ops.length === 354, 'the largest typed panel parses whole');
        const pips = parsed.panels[0].ops.filter(op => op.width === 8 && op.height === 8);
        expect(pips.length === 256 && pips.filter(op => op.op === 'fill').length === 128,
            'filled and empty pips keep their fill and outline ops');
        const exact = Array.from({length: MAX_OPS},
            () => ({op: 'fill', x: 0, y: 0, width: 1, height: 1, color: '#000000'}));
        expect(parseFrame(frame([panel('a', {ops: exact})]))?.panels[0].ops.length === 512,
            'exactly 512 ops parse');
    }],
    ['invalid_frames_are_rejected_whole', () => {
        const invalid = {
            'malformed JSON': '{"version":1,',
            'wrong version': JSON.stringify({version: 2, panels: []}),
            'too many panels': frame(Array.from({length: MAX_PANELS + 1}, (_, i) => panel(`p${i}`, {ops: []}))),
            'too many ops': frame([panel('a', {ops: Array.from({length: MAX_OPS + 1},
                () => ({op: 'fill', x: 0, y: 0, width: 1, height: 1, color: '#000000'}))})]),
            'unknown op': frame([panel('a', {ops: [{op: 'image', x: 0, y: 0, width: 1, height: 1, color: '#000000'}]})]),
            'bad color': frame([panel('a', {ops: [{op: 'fill', x: 0, y: 0, width: 1, height: 1, color: 'red'}]})]),
            'fractional geometry': frame([panel('a', {x: 1.5})]),
            'negative size': frame([panel('a', {width: -1})]),
            'text without font size': frame([panel('a', {ops: [{op: 'text', x: 0, y: 0, width: 1, height: 1, color: '#000000', text: 'x'}]})]),
            'duplicate ids': frame([panel('a'), panel('a')]),
            'empty id': frame([panel('')]),
            'oversized': frame([panel('a', {padding: 'x'.repeat(MAX_FRAME_BYTES)})]),
        };
        for (const [name, text] of Object.entries(invalid))
            expect(parseFrame(text) === null, `${name} is rejected`);
        expect(parseFrame(null) === null && parseFrame(42) === null, 'non-strings are rejected');
    }],
    ['panels_reconcile_by_id', () => {
        const factory = fakeFactory();
        const model = new OverlayModel(factory);
        model.present([panel('a'), panel('b')]);
        expect(factory.created === 2 && model.size === 2, 'new panels are created');
        model.present([panel('a'), panel('b')]);
        expect(factory.created === 2 && factory.updated === 0, 'identical frames touch no actor');
        model.present([panel('a', {x: 99}), panel('c')]);
        expect(factory.updated === 1 && factory.created === 3 && factory.destroyed === 1 &&
               model.has('a') && !model.has('b') && model.has('c'),
        'changed panels update, missing ones are removed, new ones created');
        model.clear();
        expect(model.size === 0 && factory.destroyed === 3, 'clear removes every panel');
    }],
    ['only_the_daemon_draws', () => {
        const factory = fakeFactory();
        const model = new OverlayModel(factory);
        let owner = null;
        const service = new OverlayService(model, () => owner);
        expect(service.Version === 1, 'Version is 1');
        const absent = invocation(':1.5');
        service.PresentAsync([frame([panel('a')])], absent);
        expect(absent.error === ACCESS_DENIED && model.size === 0, 'no daemon owner: Present is denied');
        owner = ':1.7';
        const rogue = invocation(':1.9');
        service.PresentAsync([frame([panel('a')])], rogue);
        expect(rogue.error === ACCESS_DENIED && model.size === 0, 'another client is denied');
        const bad = invocation(':1.7');
        service.PresentAsync(['{"version":1'], bad);
        expect(bad.error === INVALID_ARGS && model.size === 0, 'an invalid frame is rejected');
        const daemon = invocation(':1.7');
        service.PresentAsync([frame([panel('a'), panel('b')])], daemon);
        expect(daemon.returned && model.size === 2, 'the daemon presents panels');
        const rogueClear = invocation(':1.9');
        service.ClearAsync([], rogueClear);
        expect(rogueClear.error === ACCESS_DENIED && model.size === 2, 'another client cannot clear');
        const clear = invocation(':1.7');
        service.ClearAsync([], clear);
        expect(clear.returned && model.size === 0, 'the daemon clears');
        service.PresentAsync([frame([panel('a')])], invocation(':1.7'));
        service.daemonVanished();
        expect(model.size === 0, 'panels never outlive the daemon');
    }],
]);
