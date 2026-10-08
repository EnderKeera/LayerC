#!/usr/bin/env python3
"""Send graceful WM_DELETE_WINDOW, not xdotool's destructive windowclose.

Test-only. The caller must be an authenticated private_session.sh fixture.
"""
import ctypes as c
import os
import pathlib
import sys

getuid = getattr(os, 'getuid', None)
if getuid is None:
    raise SystemExit('Linux-only prototype probe')
window, pid = (int(value, 0) for value in sys.argv[1:])
runtime = pathlib.Path(os.environ['XDG_RUNTIME_DIR']).resolve()
work = runtime.parent
authority = pathlib.Path(os.environ['XAUTHORITY']).resolve()
if not (work.name.startswith('layerc-editor-prototype.') and runtime.name == 'runtime'
        and authority.parent == work and work.stat().st_uid == getuid()
        and work.stat().st_mode & 0o077 == 0):
    raise SystemExit('not an owned private prototype fixture')
expected = pathlib.Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
for name in ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR'):
    entry = f'{name}={os.environ[name]}'.encode()
    if entry not in expected:
        raise SystemExit(f'window process does not belong to this fixture: {name}')

class Data(c.Union):
    _fields_ = [('b', c.c_char * 20), ('s', c.c_short * 10), ('l', c.c_long * 5)]

class ClientMessage(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('window', c.c_ulong),
                ('message_type', c.c_ulong), ('format', c.c_int), ('data', Data)]

class Event(c.Union):
    _fields_ = [('message', ClientMessage), ('pad', c.c_long * 24)]

x = c.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = c.c_void_p
x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
x.XInternAtom.restype = c.c_ulong
x.XGetWindowProperty.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_long,
    c.c_long, c.c_int, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_int),
    c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ubyte))]
x.XGetWMProtocols.argtypes = [c.c_void_p, c.c_ulong,
    c.POINTER(c.POINTER(c.c_ulong)), c.POINTER(c.c_int)]
x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.POINTER(Event)]
x.XSync.argtypes = [c.c_void_p, c.c_int]
x.XFree.argtypes = [c.c_void_p]
x.XCloseDisplay.argtypes = [c.c_void_p]
display = x.XOpenDisplay(os.environ['DISPLAY'].encode())
if not display:
    raise SystemExit('private X11 connection failed')
try:
    atom = x.XInternAtom(display, b'_NET_WM_PID', 1)
    actual, count, after = c.c_ulong(), c.c_ulong(), c.c_ulong()
    fmt, data = c.c_int(), c.POINTER(c.c_ubyte)()
    result = x.XGetWindowProperty(display, window, atom, 0, 1, 0, 6,
        c.byref(actual), c.byref(fmt), c.byref(count), c.byref(after), c.byref(data))
    try:
        if (result != 0 or actual.value != 6 or fmt.value != 32 or count.value != 1
                or not data or c.cast(data, c.POINTER(c.c_ulong))[0] != pid):
            raise SystemExit('native window PID mismatch')
    finally:
        if data:
            x.XFree(data)
    protocols, number = c.POINTER(c.c_ulong)(), c.c_int()
    delete = x.XInternAtom(display, b'WM_DELETE_WINDOW', 1)
    if not x.XGetWMProtocols(display, window, c.byref(protocols), c.byref(number)):
        raise SystemExit('window has no WM protocols')
    try:
        if delete == 0 or delete not in protocols[:number.value]:
            raise SystemExit('window does not support graceful close')
    finally:
        x.XFree(protocols)
    event = Event()
    event.message.type = 33  # ClientMessage
    event.message.send_event = 1
    event.message.display = display
    event.message.window = window
    event.message.message_type = x.XInternAtom(display, b'WM_PROTOCOLS', 1)
    event.message.format = 32
    event.message.data.l[0] = delete
    event.message.data.l[1] = 0  # CurrentTime
    if not x.XSendEvent(display, window, 0, 0, c.byref(event)):
        raise SystemExit('graceful close request failed')
    x.XSync(display, 0)
    print(f'close-request: WM_DELETE_WINDOW window={window} pid={pid}')
finally:
    x.XCloseDisplay(display)
