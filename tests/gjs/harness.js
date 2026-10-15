// Minimal gjs test harness: synchronous main-context pumping, no compositor,
// no network, no production state.
import GLib from 'gi://GLib';
import System from 'system';

let failures = 0;
let checks = 0;

export function expect(value, message) {
    ++checks;
    if (value) return true;
    ++failures;
    printerr(`FAIL: ${message}`);
    return false;
}

// Iterates the default main context until `done()` or the timeout elapses.
export function pump(done, timeoutMs = 2000) {
    const context = GLib.MainContext.default();
    const end = GLib.get_monotonic_time() + timeoutMs * 1000;
    // A periodic wake keeps a blocking iteration bounded.
    const tick = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 5, () => GLib.SOURCE_CONTINUE);
    try {
        while (!done() && GLib.get_monotonic_time() < end) context.iteration(true);
    } finally {
        GLib.source_remove(tick);
    }
    return done();
}

export function settle(ms) {
    pump(() => false, ms);
}

export function run(cases) {
    for (const [name, body] of cases) {
        const before = failures;
        try {
            body();
        } catch (error) {
            ++failures;
            printerr(`FAIL: ${name} threw ${error}\n${error.stack ?? ''}`);
        }
        print(`${failures === before ? 'PASS' : 'CASE FAILED'}: ${name}`);
    }
    print(`${checks} check(s), ${failures} failure(s)`);
    System.exit(failures === 0 ? 0 : 1);
}
