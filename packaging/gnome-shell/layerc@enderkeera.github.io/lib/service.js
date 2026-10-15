// D-Bus names and the Overlay1 method handlers. Pure apart from the
// invocation objects GJS passes to exported `*Async` methods, which tests
// replace with plain fakes. Local session-bus IPC only; no network.

import {parseFrame} from './frame.js';

export const DAEMON_NAME = 'io.github.EnderKeera.LayerC';
export const DAEMON_PATH = '/io/github/EnderKeera/LayerC';
export const SHELL_INTERFACE = 'io.github.EnderKeera.LayerC.Shell1';
export const OVERLAY_NAME = 'io.github.EnderKeera.LayerC.GnomeShell';
export const OVERLAY_PATH = '/io/github/EnderKeera/LayerC/GnomeShell';
export const OVERLAY_INTERFACE = 'io.github.EnderKeera.LayerC.Overlay1';
export const ACCESS_DENIED = 'org.freedesktop.DBus.Error.AccessDenied';
export const INVALID_ARGS = 'org.freedesktop.DBus.Error.InvalidArgs';

export const OVERLAY_XML = `<node>
  <interface name="${OVERLAY_INTERFACE}">
    <method name="Present"><arg type="s" name="frame" direction="in"/></method>
    <method name="Clear"/>
    <property name="Version" type="u" access="read"/>
  </interface>
</node>`;

// Only the current owner of the daemon's name may draw or clear panels, so
// another same-session client cannot paint over the desktop through LayerC.
export class OverlayService {
    // model: OverlayModel; daemonOwner(): unique name owning DAEMON_NAME or null.
    constructor(model, daemonOwner) {
        this._model = model;
        this._daemonOwner = daemonOwner;
    }

    get Version() { return 1; }

    _authorized(invocation) {
        const owner = this._daemonOwner();
        if (owner && invocation.get_sender() === owner) return true;
        invocation.return_dbus_error(ACCESS_DENIED, 'only the LayerC daemon may draw panels');
        return false;
    }

    PresentAsync(parameters, invocation) {
        if (!this._authorized(invocation)) return;
        const frame = parseFrame(parameters[0]);
        if (!frame) {
            invocation.return_dbus_error(INVALID_ARGS, 'overlay frame rejected');
            return;
        }
        this._model.present(frame.panels);
        invocation.return_value(null);
    }

    ClearAsync(_parameters, invocation) {
        if (!this._authorized(invocation)) return;
        this._model.clear();
        invocation.return_value(null);
    }

    // The daemon's name vanished: no panel outlives the daemon that drew it.
    daemonVanished() {
        this._model.clear();
    }
}
