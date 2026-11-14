"""The game's own icon, from the player's own dump, for the shortcut, the windows and the start-up picture. GPL-3.0-or-later.

python game_art.py --folders <exefs folder> <romfs folder> <out folder>

This project ships no artwork. Every Switch title carries its icon in its control data (icon_<Language>.dat, a 256x256
JPEG). A player who extracted that too (nxdumptool dumps it with the rest) leaves an icon_<Language>.dat file in, or
next to, the ExeFS or RomFS folder, or in a control/ folder there. Setup reads it and writes, into <out folder>
(build/game-art in an installation):
  icon.jpg     the icon as the game stores it
  icon.ico     the same at the sizes Windows asks for (shortcut, window, taskbar)
  splash.jpg   2560x1080: the icon on a blurred copy of itself, which the game window shows until the game draws
               (runtime/gfx/display.c splash_load)
Images are made with GDI+, which every Windows has: no imaging package in the player's Python.
Cosmetic only: when anything is missing, the step says why and setup goes on (plain windows, text start-up screen)."""
import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import struct
import sys
import uuid

LANGUAGES = ('AmericanEnglish', 'BritishEnglish', 'CanadianFrench', 'LatinAmericanSpanish', 'French', 'German', 'Spanish',
             'Italian', 'Dutch', 'Portuguese', 'BrazilianPortuguese', 'Russian', 'Japanese', 'Korean', 'SimplifiedChinese',
             'TraditionalChinese')
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
SPLASH = (2560, 1080, 640)       # width, height, icon side: wide enough that display.c's cover-fit only crops the backdrop


def jpeg(data):
    return data if 0 < len(data) <= 1 << 20 and data[:3] == b'\xff\xd8\xff' else None


def icon_in_folders(folders):
    """The icon JPEG from an icon_<Language>.dat in the player's folders, next to them, or in a control/ folder there."""
    places = []
    for folder in map(Path, folders):
        for place in (folder, folder.parent):
            places += [place, place / 'control']
    for language in LANGUAGES:
        for place in places:
            path = place / ('icon_%s.dat' % language)
            if path.is_file() and path.stat().st_size <= 1 << 20 and jpeg(path.read_bytes()):
                return path.read_bytes()
    raise ValueError('no icon_<Language>.dat next to the extracted folders')




class Gdiplus:
    """The few GDI+ flat-API calls needed: load, draw scaled, fill, read pixels, save."""
    class _Start(C.Structure):
        _fields_ = [('version', C.c_uint32), ('callback', C.c_void_p), ('no_thread', W.BOOL), ('no_codecs', W.BOOL)]

    class _Locked(C.Structure):
        _fields_ = [('width', C.c_uint), ('height', C.c_uint), ('stride', C.c_int), ('format', C.c_int),
                    ('scan0', C.c_void_p), ('reserved', C.c_size_t)]

    class _Guid(C.Structure):
        _fields_ = [('bytes', C.c_ubyte * 16)]

    ARGB = 0x26200A                 # PixelFormat32bppARGB
    JPEG = '557CF401-1A04-11D3-9A73-0000F81EF32E'
    PNG = '557CF406-1A04-11D3-9A73-0000F81EF32E'
    QUALITY = '1D5BE4B5-FA4A-452D-9CDD-5DB35105E7EB'

    def __init__(self):
        self.g = C.WinDLL('gdiplus')
        self.token = C.c_size_t()
        start = self._Start(1, None, False, False)
        self.ok(self.g.GdiplusStartup(C.byref(self.token), C.byref(start), None))

    def close(self): self.g.GdiplusShutdown(self.token)

    @staticmethod
    def ok(status):
        if status: raise OSError('GDI+ status %d' % status)

    def guid(self, text):
        g = self._Guid(); g.bytes[:] = uuid.UUID(text).bytes_le; return g

    def load(self, path):
        image = C.c_void_p(); self.ok(self.g.GdipCreateBitmapFromFile(W.LPCWSTR(str(path)), C.byref(image))); return image

    def size(self, image):
        w, h = C.c_uint(), C.c_uint()
        self.ok(self.g.GdipGetImageWidth(image, C.byref(w))); self.ok(self.g.GdipGetImageHeight(image, C.byref(h)))
        return w.value, h.value

    def canvas(self, w, h):
        image, graphics = C.c_void_p(), C.c_void_p()
        self.ok(self.g.GdipCreateBitmapFromScan0(w, h, 0, self.ARGB, None, C.byref(image)))
        self.ok(self.g.GdipGetImageGraphicsContext(image, C.byref(graphics)))
        self.ok(self.g.GdipSetInterpolationMode(graphics, 7))      # HighQualityBicubic
        self.ok(self.g.GdipSetPixelOffsetMode(graphics, 4))        # Half: no half-pixel shift when scaling
        self.ok(self.g.GdipSetCompositingQuality(graphics, 2))     # HighQuality
        return image, graphics

    def draw(self, graphics, image, x, y, w, h):
        """`image` scaled into x,y,w,h; edges mirrored so a strong scale does not fade into the border."""
        attributes = C.c_void_p()
        self.ok(self.g.GdipCreateImageAttributes(C.byref(attributes)))
        try:
            self.ok(self.g.GdipSetImageAttributesWrapMode(attributes, 3, 0, False))   # WrapModeTileFlipXY
            sw, sh = self.size(image)
            self.ok(self.g.GdipDrawImageRectRectI(graphics, image, x, y, w, h, 0, 0, sw, sh, 2, attributes, None, None))
        finally:
            self.g.GdipDisposeImageAttributes(attributes)

    def fill(self, graphics, argb, x, y, w, h):
        brush = C.c_void_p()
        self.ok(self.g.GdipCreateSolidFill(C.c_uint32(argb), C.byref(brush)))
        try: self.ok(self.g.GdipFillRectangleI(graphics, brush, x, y, w, h))
        finally: self.g.GdipDeleteBrush(brush)

    def pixels(self, image):
        """Top-down BGRA rows of a 32-bit ARGB bitmap."""
        w, h = self.size(image)
        rect = (C.c_int * 4)(0, 0, w, h); locked = self._Locked()
        self.ok(self.g.GdipBitmapLockBits(image, rect, 1, self.ARGB, C.byref(locked)))
        try: return [C.string_at(locked.scan0 + y * locked.stride, w * 4) for y in range(h)]
        finally: self.g.GdipBitmapUnlockBits(image, C.byref(locked))

    def save(self, image, path, codec, quality=None):
        params = None
        if quality is not None:
            class One(C.Structure):
                _fields_ = [('count', C.c_uint), ('guid', self._Guid), ('values', C.c_ulong), ('type', C.c_ulong),
                            ('value', C.c_void_p)]
            level = C.c_ulong(quality)
            params = One(1, self.guid(self.QUALITY), 1, 4, C.cast(C.byref(level), C.c_void_p))  # EncoderParameterValueTypeLong
        self.ok(self.g.GdipSaveImageToFile(image, W.LPCWSTR(str(path)), C.byref(self.guid(codec)),
                                           C.byref(params) if params else None))


def scaled(gp, source, side):
    image, graphics = gp.canvas(side, side)
    gp.draw(graphics, source, 0, 0, side, side)
    gp.g.GdipDeleteGraphics(graphics)
    return image


def write_ico(gp, source, out):
    """ICO: 256 as PNG (Vista and later), the smaller sizes as 32-bit DIBs, which every Windows icon path reads."""
    entries = []
    for side in ICO_SIZES:
        image = scaled(gp, source, side)
        try:
            if side == 256:
                temporary = out.with_suffix('.png.part')
                gp.save(image, temporary, gp.PNG)
                data = temporary.read_bytes(); temporary.unlink()
            else:
                rows = gp.pixels(image)
                mask_row = ((side + 31) // 32) * 4
                data = (struct.pack('<IiiHHIIiiII', 40, side, side * 2, 1, 32, 0, side * side * 4, 0, 0, 0, 0) +
                        b''.join(reversed(rows)) + b'\0' * (mask_row * side))      # bottom-up colour, then an empty AND mask
        finally:
            gp.g.GdipDisposeImage(image)
        entries.append((side, data))
    offset = 6 + 16 * len(entries)
    head, body = struct.pack('<HHH', 0, 1, len(entries)), b''
    for side, data in entries:
        head += struct.pack('<BBBBHHII', side % 256, side % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    part = out.with_suffix('.ico.part')
    part.write_bytes(head + body); part.replace(out)


def write_splash(gp, source, out):
    """The icon centred on a blurred, darkened copy of itself: blur = shrink to 16 px, then scale back up in steps
    (one big bicubic step leaves a visible grid of the 16 cells)."""
    width, height, side = SPLASH
    tiny = scaled(gp, source, 16)
    for step in (48, 160):
        bigger = scaled(gp, tiny, step); gp.g.GdipDisposeImage(tiny); tiny = bigger
    image, graphics = gp.canvas(width, height)
    try:
        gp.draw(graphics, tiny, 0, (height - width) // 2, width, width)
        gp.fill(graphics, 0xB4180610, 0, 0, width, height)                 # dark maroon veil, like the status bar
        x, y = (width - side) // 2, (height - side) // 2
        for i in range(16):                                                # soft shadow: 16 widening low-alpha steps
            spread = (16 - i) * 2
            gp.fill(graphics, 0x0A000000, x - spread, y - spread + 10, side + 2 * spread, side + 2 * spread)
        gp.draw(graphics, source, x, y, side, side)
        part = out.with_suffix('.jpg.part')
        gp.save(image, part, gp.JPEG, quality=92); part.replace(out)
    finally:
        gp.g.GdipDeleteGraphics(graphics); gp.g.GdipDisposeImage(image); gp.g.GdipDisposeImage(tiny)


def make(data, out):
    """Write icon.jpg, icon.ico and splash.jpg into `out` from the icon JPEG `data`."""
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    (out / 'icon.jpg.part').write_bytes(data); (out / 'icon.jpg.part').replace(out / 'icon.jpg')
    gp = Gdiplus()
    try:
        source = gp.load(out / 'icon.jpg')
        try:
            if gp.size(source) != (256, 256):
                raise ValueError('unexpected icon size %dx%d' % gp.size(source))
            write_ico(gp, source, out / 'icon.ico')
            write_splash(gp, source, out / 'splash.jpg')
        finally:
            gp.g.GdipDisposeImage(source)
    finally:
        gp.close()
    return out


def main():
    args = sys.argv[1:]
    if len(args) == 4 and args[0] == '--folders':
        read, out = (lambda: icon_in_folders(args[1:3])), args[3]
    else:
        raise SystemExit(__doc__)
    try:
        make(read(), out)
        print('game art: icon and start-up picture written to ' + os.path.basename(out.rstrip('\\/')), flush=True)
    except (Exception, SystemExit) as error:  # cosmetic: never fail setup over it, and never print key or package bytes
        print('game art: skipped (%s)' % type(error).__name__, flush=True)


if __name__ == '__main__':
    main()
