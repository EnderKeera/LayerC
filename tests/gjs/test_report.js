// Window report building, bounds, and the coalescing scheduler.
import {expect, run} from './harness.js';
import {
    MAX_REPORT_BYTES, MAX_WINDOWS, REPORT_INTERVAL_MS, ReportScheduler, boundedText,
    buildReport, utf8Length, windowRow,
} from '../../packaging/gnome-shell/layerc@enderkeera.github.io/lib/report.js';

function window(id, extra = {}) {
    return {id, title: 'Game', wmClass: 'game', instance: 'game-bin', pid: 4242, x: -10, y: 20,
        width: 800, height: 600, active: false, fullscreen: false, hidden: false, ...extra};
}

// Deterministic platform: timers fire only when the test says so.
function fakePlatform() {
    const platform = {
        timers: new Map(), nextId: 1, sent: [], pending: [], generation: 0,
        schedule(ms, callback) {
            const id = this.nextId++;
            this.timers.set(id, {ms, callback});
            return id;
        },
        cancel(id) { this.timers.delete(id); },
        snapshot() { return `snapshot-${++this.generation}`; },
        send(payload, done) {
            this.sent.push(payload);
            this.pending.push(done);
        },
        fire() {
            const timers = [...this.timers.values()];
            this.timers.clear();
            for (const timer of timers) timer.callback();
        },
        reply() { this.pending.shift()?.(); },
    };
    return platform;
}

run([
    ['text_is_valid_bounded_utf8', () => {
        expect(boundedText('abcdef', 3) === 'abc', 'ASCII is cut at the byte limit');
        expect(boundedText('ééé', 5) === 'éé', 'two-byte characters are never split');
        expect(boundedText('a😀', 4) === 'a', 'a four-byte character that does not fit is dropped');
        expect(boundedText('x\ud800y', 16) === 'x�y', 'lone surrogates become U+FFFD');
        expect(boundedText(null, 8) === '' && boundedText(undefined, 8) === '', 'absent text is empty');
        expect(utf8Length('é😀') === 6, 'UTF-8 length counts encoded bytes');
    }],
    ['rows_follow_the_daemon_schema', () => {
        expect(windowRow(window('')) === null, 'a window without identity is dropped');
        const row = windowRow(window('7', {x11Window: 0x2a00003, active: 1}));
        expect(row.id === '7' && row.class === 'game' && row.instance === 'game-bin' &&
               row.pid === 4242 && row.x11Window === 0x2a00003 && row.active === true,
        'class, instance, pid, x11Window and booleans use the report field names');
        const native = windowRow(window('8', {pid: 0, width: -5, x: Infinity}));
        expect(!('pid' in native) && !('x11Window' in native) && native.width === 0 &&
               native.x === -2147483648, 'optional fields are omitted and integers clamped');
        expect(windowRow(window('x'.repeat(300))).id.length === 128, 'ids are bounded to 128 bytes');
    }],
    ['reports_are_bounded_full_snapshots', () => {
        const many = Array.from({length: MAX_WINDOWS + 6}, (_, i) => window(`w${i}`));
        const report = JSON.parse(buildReport(many));
        expect(report.version === 1 && report.source === 'gnome', 'GNOME reports are version 1');
        expect(report.windows.length === MAX_WINDOWS, 'excess windows are left out');
        const huge = Array.from({length: MAX_WINDOWS}, (_, i) =>
            window(`h${i}`, {title: '界'.repeat(400), wmClass: '界'.repeat(400)}));
        const text = buildReport(huge);
        expect(utf8Length(text) <= MAX_REPORT_BYTES && JSON.parse(text).windows.length > 0,
            'the payload never exceeds 64 KiB');
        expect(JSON.parse(buildReport([])).windows.length === 0, 'an empty desktop reports no windows');
    }],
    ['nothing_is_sent_without_the_daemon', () => {
        const platform = fakePlatform();
        const scheduler = new ReportScheduler(platform);
        for (let i = 0; i < 10; ++i) scheduler.changed();
        expect(platform.sent.length === 0 && platform.timers.size === 0,
            'changes while the daemon is absent arm no timer and send nothing');
        scheduler.setDaemonPresent(true);
        expect(platform.sent.length === 1, 'an initial snapshot follows the name appearing');
        scheduler.destroy();
    }],
    ['changes_coalesce_to_one_call_in_flight', () => {
        const platform = fakePlatform();
        const scheduler = new ReportScheduler(platform);
        scheduler.setDaemonPresent(true);
        for (let i = 0; i < 100; ++i) scheduler.changed();
        expect(platform.sent.length === 1 && platform.timers.size === 0,
            'changes during the call in flight neither send nor arm a timer');
        platform.reply();
        expect(platform.timers.size === 1 &&
               [...platform.timers.values()][0].ms === REPORT_INTERVAL_MS,
        'the reply arms exactly one interval timer');
        platform.fire();
        expect(platform.sent.length === 2 && platform.sent[1] === 'snapshot-2',
            'one fresh snapshot replaces the coalesced changes');
        platform.reply();
        expect(platform.timers.size === 0, 'no timer remains while idle');
        for (let i = 0; i < 50; ++i) scheduler.changed();
        expect(platform.timers.size === 1 && platform.sent.length === 2,
            'idle changes share one timer');
        platform.fire();
        expect(platform.sent.length === 3, 'at most one report per interval');
        scheduler.destroy();
    }],
    ['owner_changes_and_destroy_cancel_cleanly', () => {
        const platform = fakePlatform();
        const scheduler = new ReportScheduler(platform);
        scheduler.setDaemonPresent(true);
        scheduler.changed();
        scheduler.setDaemonPresent(false);
        platform.reply();
        expect(platform.timers.size === 0, 'a late reply for a vanished daemon arms nothing');
        scheduler.changed();
        expect(platform.sent.length === 1, 'nothing is sent after the daemon vanished');
        scheduler.setDaemonPresent(true);
        scheduler.changed();
        platform.reply();
        platform.fire();
        expect(platform.sent.length === 3, 'a replacement daemon gets its own initial snapshot');
        scheduler.changed();
        scheduler.destroy();
        expect(platform.timers.size === 0, 'destroy cancels the pending timer');
        platform.reply();
        expect(platform.timers.size === 0 && platform.sent.length === 3,
            'a reply after destroy does nothing');
    }],
]);
