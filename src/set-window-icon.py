#!/usr/bin/env python3
"""Give Parsec's X11 window an icon.

Parsec never sets _NET_WM_ICON on its window, so the Steam Frame dashboard
(which takes window icons from that property) shows it without one. This sets
the property from a PNG on every window of the given WM_CLASS, for as long as
the given process is alive. No dependencies beyond Python and libX11.

Usage: set-window-icon.py <wm-class> <icon.png> <pid-to-follow>
"""
import ctypes
import ctypes.util
import os
import struct
import sys
import time
import zlib

SIZES = (128, 64, 48, 32)  # 256 would exceed the X request size limit


def read_png_rgba(path):
    """Decode an 8-bit RGBA or RGB, non-interlaced PNG into (w, h, bytes)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("unsupported PNG format")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    bpp = 4 if ctype == 6 else 3
    raw, stride = zlib.decompress(idat), w * bpp
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        prev = line
        if bpp == 3:
            line = b"".join(bytes(line[i:i + 3]) + b"\xff" for i in range(0, stride, 3))
        out += line
    return w, h, bytes(out)


def scale(w, h, rgba, size):
    """Box-filter downscale (or nearest upscale) to size x size."""
    out = []
    for y in range(size):
        y0, y1 = y * h // size, max((y + 1) * h // size, y * h // size + 1)
        for x in range(size):
            x0, x1 = x * w // size, max((x + 1) * w // size, x * w // size + 1)
            acc = [0, 0, 0, 0]
            for yy in range(y0, y1):
                row = yy * w * 4
                for xx in range(x0, x1):
                    p = row + xx * 4
                    alpha = rgba[p + 3]
                    acc[0] += rgba[p] * alpha
                    acc[1] += rgba[p + 1] * alpha
                    acc[2] += rgba[p + 2] * alpha
                    acc[3] += alpha
            n = (x1 - x0) * (y1 - y0)
            a = acc[3] // n
            r, g, b = ((acc[i] // acc[3]) if acc[3] else 0 for i in range(3))
            out.append((a << 24) | (r << 16) | (g << 8) | b)  # _NET_WM_ICON is ARGB
    return out


def icon_property(path):
    w, h, rgba = read_png_rgba(path)
    values = []
    for size in SIZES:
        if size <= max(w, h):
            values += [size, size] + scale(w, h, rgba, size)
    return values


class X11:
    def __init__(self):
        self.x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        x = self.x
        x.XOpenDisplay.restype = ctypes.c_void_p
        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XDefaultRootWindow.restype = ctypes.c_ulong
        x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x.XInternAtom.restype = ctypes.c_ulong
        x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                 ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                                 ctypes.POINTER(ctypes.c_uint)]
        x.XGetClassHint.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p]
        x.XChangeProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                      ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        x.XFree.argtypes = [ctypes.c_void_p]
        x.XFlush.argtypes = [ctypes.c_void_p]
        x.XSetErrorHandler.restype = ctypes.c_void_p
        # Windows can vanish between listing and touching them; ignore those errors.
        self._handler = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(lambda d, e: 0)
        x.XSetErrorHandler(self._handler)
        self.dpy = x.XOpenDisplay(None)
        if not self.dpy:
            raise RuntimeError("can't open X display " + os.environ.get("DISPLAY", "(unset)"))
        self.root = x.XDefaultRootWindow(self.dpy)
        self.atom_icon = x.XInternAtom(self.dpy, b"_NET_WM_ICON", 0)
        self.atom_cardinal = x.XInternAtom(self.dpy, b"CARDINAL", 0)

    def windows(self, win=None):
        win = win or self.root
        root, parent = ctypes.c_ulong(), ctypes.c_ulong()
        children, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not self.x.XQueryTree(self.dpy, win, ctypes.byref(root), ctypes.byref(parent),
                                 ctypes.byref(children), ctypes.byref(n)):
            return
        kids = [children[i] for i in range(n.value)]
        if children:
            self.x.XFree(children)
        for kid in kids:
            yield kid
            yield from self.windows(kid)

    def wm_class(self, win):
        hint = (ctypes.c_void_p * 2)()  # XClassHint { res_name, res_class }
        if not self.x.XGetClassHint(self.dpy, win, hint):
            return None
        name = ctypes.string_at(hint[1]).decode(errors="replace") if hint[1] else ""
        for p in hint:
            if p:
                self.x.XFree(p)
        return name

    def set_icon(self, win, values):
        # Format-32 property data is passed to Xlib as an array of C longs.
        arr = (ctypes.c_long * len(values))(*values)
        self.x.XChangeProperty(self.dpy, win, self.atom_icon, self.atom_cardinal, 32, 0,
                               arr, len(values))
        self.x.XFlush(self.dpy)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    wm_class, png, pid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    values = icon_property(png)
    x = X11()
    done = set()
    while alive(pid):
        for win in list(x.windows()):
            if win not in done and x.wm_class(win) == wm_class:
                x.set_icon(win, values)
                done.add(win)
        time.sleep(1)


if __name__ == "__main__":
    main()
