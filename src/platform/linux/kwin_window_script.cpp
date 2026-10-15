#include "kwin_script.hpp"

namespace layerc::platform {

std::string_view kwin_window_script() noexcept {
  // Deliberately ES5: KWin 5.27 and 6 expose different workspace names,
  // but both embed this JavaScript dialect.
  return R"JS((function () {
    var modern = typeof workspace.windowList === "function";
    var watched = [];
    var removed = [];
    var reporting = false;
    var again = false;

    function windows() {
        return modern ? workspace.windowList() : workspace.clientList();
    }
    function active() {
        return modern ? workspace.activeWindow : workspace.activeClient;
    }
    function connect(object, name, callback) {
        try {
            if (object[name] && typeof object[name].connect === "function")
                object[name].connect(callback);
        } catch (error) {}
    }
    function integer(value, low, high) {
        var n = Number(value);
        return isFinite(n) ? Math.max(low, Math.min(high, Math.round(n))) : low;
    }
    // Valid UTF-8, byte bounded, never split a supplementary character.
    function text(value, limit) {
        var source = value === undefined || value === null ? "" : String(value);
        var out = "", bytes = 0;
        for (var i = 0; i < source.length; ++i) {
            var c = source.charCodeAt(i), piece = source.charAt(i), size;
            if (c >= 0xd800 && c <= 0xdbff && i + 1 < source.length &&
                source.charCodeAt(i + 1) >= 0xdc00 && source.charCodeAt(i + 1) <= 0xdfff) {
                piece += source.charAt(++i);
                size = 4;
            } else if (c >= 0xd800 && c <= 0xdfff) {
                piece = "\uFFFD";
                size = 3;
            } else {
                size = c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
            }
            if (bytes + size > limit) break;
            out += piece;
            bytes += size;
        }
        return out;
    }
    function byteLength(value) {
        var n = 0;
        for (var i = 0; i < value.length; ++i) {
            var c = value.charCodeAt(i);
            if (c >= 0xd800 && c <= 0xdbff && i + 1 < value.length) {
                ++i;
                n += 4;
            } else n += c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
        }
        return n;
    }
    function row(window) {
        var geometry = window.frameGeometry || window.geometry;
        if (!geometry) return null;
        var id = text(window.internalId || window.windowId, 128);
        if (!id) return null;
        var result = {
            id: id, title: text(window.caption, 512),
            "class": text(window.resourceClass, 512),
            instance: text(window.resourceName, 512),
            x: integer(geometry.x, -2147483648, 2147483647),
            y: integer(geometry.y, -2147483648, 2147483647),
            width: integer(geometry.width, 0, 2147483647),
            height: integer(geometry.height, 0, 2147483647),
            active: window === active(), fullscreen: !!window.fullScreen,
            hidden: !!window.minimized
        };
        if (window.pid > 0)
            result.pid = integer(window.pid, 1, 4294967295);
        if (window.windowId > 0)
            result.x11Window = integer(window.windowId, 1, 4294967295);
        return result;
    }
    function report() {
        // Do not queue unbounded calls if the daemon is slow. Coalesce any
        // changes during the in-flight call into one fresh full snapshot.
        if (reporting) { again = true; return; }
        var all = windows(), rows = [], bytes = 45;
        for (var i = 0; i < all.length; ++i) {
            var window = all[i];
            if (!window.normalWindow || removed.indexOf(window) >= 0) continue;
            watch(window);
            var next = row(window);
            if (!next) continue;
            var size = byteLength(JSON.stringify(next)) + 1;
            if (bytes + size > 65536) break;
            rows.push(next);
            bytes += size;
        }
        var payload = JSON.stringify({version: 1, source: "kwin", windows: rows});
        reporting = true;
        callDBus("io.github.EnderKeera.LayerC", "/io/github/EnderKeera/LayerC",
                 "io.github.EnderKeera.LayerC.Shell1", "ReportWindows", payload,
                 function () {
                     reporting = false;
                     if (again) { again = false; report(); }
                 });
    }
    function watch(window) {
        if (watched.indexOf(window) >= 0) return;
        watched.push(window);
        var names = ["captionChanged", "frameGeometryChanged", "geometryChanged",
                     "fullScreenChanged", "minimizedChanged", "resourceClassChanged"];
        for (var i = 0; i < names.length; ++i) connect(window, names[i], report);
    }
    connect(workspace, modern ? "windowActivated" : "clientActivated", report);
    connect(workspace, modern ? "windowAdded" : "clientAdded", function (window) {
        var old = removed.indexOf(window);
        if (old >= 0) removed.splice(old, 1);
        watch(window);
        report();
    });
    connect(workspace, modern ? "windowRemoved" : "clientRemoved", function (window) {
        // Some KWin versions emit before windowList removes the object.
        removed.push(window);
        report();
        var at = watched.indexOf(window);
        if (at >= 0) watched.splice(at, 1);
        removed.splice(removed.indexOf(window), 1);
    });
    report();
})();
)JS";
}

} // namespace layerc::platform
