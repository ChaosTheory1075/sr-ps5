"""Just enough SWF/GFX for the button glyphs: tags, DefineFont2/3 glyph shapes (parse, write)."""
import struct


class Bits:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos * 8

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def s(self, n):
        v = self.u(n)
        return v - (1 << n) if n and v >> (n - 1) else v


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, v, n):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def s(self, v, n):
        self.u(v & ((1 << n) - 1), n)

    def bytes(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def sbits(*vals):
    """Bits for signed values."""
    n = 1
    for v in vals:
        while not (-(1 << (n - 1)) <= v < (1 << (n - 1))):
            n += 1
    return n


def tags(d):
    """(offset of the tag header, header length, code, body) of each tag of a decompressed movie."""
    nb = d[8] >> 3
    p = 8 + (5 + nb * 4 + 7) // 8 + 4
    out = []
    while p + 2 <= len(d):
        start = p
        tc = struct.unpack('<H', d[p:p + 2])[0]
        p += 2
        t, ln = tc >> 6, tc & 63
        if ln == 63:
            ln = struct.unpack('<I', d[p:p + 4])[0]
            p += 4
        out.append((start, p - start, t, d[p:p + ln]))
        p += ln
        if t == 0:
            break
    return out


def parse_shape(b, pos):
    """A glyph's SHAPE: a list of contours, each [(x, y) start, then ('L', x, y) / ('Q', cx, cy, x, y)]."""
    r = Bits(b, pos)
    nf, nl = r.u(4), r.u(4)
    contours, cur, x, y = [], None, 0, 0
    while True:
        if r.u(1) == 0:
            flags = r.u(5)
            if flags == 0:
                break
            if flags & 1:
                n = r.u(5)
                x, y = r.s(n), r.s(n)
                cur = [(x, y)]
                contours.append(cur)
            if flags & 2:
                r.u(nf)
            if flags & 4:
                r.u(nf)
            if flags & 8:
                r.u(nl)
            if flags & 16:
                raise ValueError('new styles in a glyph')
        elif r.u(1) == 1:  # straight
            n = r.u(4) + 2
            if r.u(1):
                dx, dy = r.s(n), r.s(n)
            elif r.u(1):
                dx, dy = 0, r.s(n)
            else:
                dx, dy = r.s(n), 0
            x, y = x + dx, y + dy
            cur.append(('L', x, y))
        else:  # curved
            n = r.u(4) + 2
            cx, cy = x + r.s(n), y + r.s(n)
            x, y = cx + r.s(n), cy + r.s(n)
            cur.append(('Q', cx, cy, x, y))
    return contours


def first_fill(b):
    """Which fill style a glyph's first style change sets: 2 (FillStyle0) or 4 (FillStyle1)."""
    r = Bits(b, 0)
    r.u(8)
    if r.u(1) == 0:
        flags = r.u(5)
        if flags & 2:
            return 2
    return 4


def write_shape(contours, fill=4):
    """SHAPE bytes for contours as parse_shape gives them (fill style 1 on the `fill` side: 2 for
    FillStyle0, 4 for FillStyle1; no lines)."""
    w = BitWriter()
    w.u(1, 4)
    w.u(0, 4)
    first = True
    for c in contours:
        sx, sy = c[0]
        w.u(0, 1)
        w.u(1 | (fill if first else 0), 5)  # move to (and the fill style, first time)
        n = sbits(sx, sy)
        w.u(n, 5)
        w.s(sx, n)
        w.s(sy, n)
        if first:
            w.u(1, 1)
            first = False
        x, y = sx, sy
        for e in c[1:]:
            if e[0] == 'L':
                dx, dy = e[1] - x, e[2] - y
                if dx == 0 and dy == 0:
                    continue
                n = max(2, sbits(dx, dy))
                w.u(1, 1)
                w.u(1, 1)
                w.u(n - 2, 4)
                if dx and dy:
                    w.u(1, 1)
                    w.s(dx, n)
                    w.s(dy, n)
                elif dy:
                    w.u(0, 1)
                    w.u(1, 1)
                    w.s(dy, n)
                else:
                    w.u(0, 1)
                    w.u(0, 1)
                    w.s(dx, n)
                x, y = e[1], e[2]
            else:
                cdx, cdy, adx, ady = e[1] - x, e[2] - y, e[3] - e[1], e[4] - e[2]
                n = max(2, sbits(cdx, cdy, adx, ady))
                w.u(1, 1)
                w.u(0, 1)
                w.u(n - 2, 4)
                for v in (cdx, cdy, adx, ady):
                    w.s(v, n)
                x, y = e[3], e[4]
    w.u(0, 6)  # end
    return w.bytes()


class Font:
    """A DefineFont2/3 tag's glyphs: shapes (raw bytes), codes; the rest kept as it was."""

    def __init__(self, code, body):
        self.tag = code
        self.id, self.flags, self.lang, nl = struct.unpack('<HBBB', body[:5])
        self.name = body[5:5 + nl].rstrip(b'\0').decode('latin-1')
        ng = struct.unpack('<H', body[5 + nl:7 + nl])[0]
        q = 7 + nl
        self.wide_offsets = bool(self.flags & 0x08)
        osz = 4 if self.wide_offsets else 2
        offs = [struct.unpack('<I' if self.wide_offsets else '<H', body[q + i * osz:q + i * osz + osz])[0]
                for i in range(ng + 1)]
        self.shapes = [body[q + offs[i]:q + offs[i + 1]] for i in range(ng)]
        ct = q + offs[ng]
        csz = 2 if self.flags & 0x04 else 1
        self.codes = [struct.unpack('<H' if csz == 2 else '<B', body[ct + i * csz:ct + i * csz + csz])[0]
                      for i in range(ng)]
        self.head = body[:q]
        self.tail = body[ct:]  # code table, layout
        self.shape_area = offs[ng] - offs[0]
        self.table_size = offs[0]

    def body(self, keep_size=None):
        """The tag body again; with keep_size, padded (before the code table) to that length."""
        ng = len(self.shapes)
        osz = 4 if self.wide_offsets else 2
        first = (ng + 1) * osz
        offs, o = [], first
        for s in self.shapes:
            offs.append(o)
            o += len(s)
        pad = 0
        if keep_size is not None:
            pad = keep_size - (len(self.head) + o + len(self.tail))
            if pad < 0:
                raise ValueError(f'{self.name}: {-pad} bytes too many')
        offs.append(o + pad)
        if not self.wide_offsets and offs[-1] > 0xFFFF:
            raise ValueError('offsets too wide')
        table = b''.join(struct.pack('<I' if self.wide_offsets else '<H', v) for v in offs)
        return self.head + table + b''.join(self.shapes) + b'\0' * pad + self.tail
