# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Image codec for the panel photo: encoder and reference decoder.

The firmware stores the encoded bytes and web/photo.js decodes them in the
browser; nothing here runs on the device. tools/embed_assets.py calls
encode() when Assets.h is generated.

The picture is an object on a plain background, stored as a recipe:

1. Shape: which pixels belong to the object, each coded with its 10 known
   neighbours as context. The background is transparent on the page.
2. Surface: per colour channel (YCoCg), a CDF 9/7 wavelet in lifting form.
   Its coarse grid interpolates back into the gradients, shadows and depth;
   the detail bands hold only what that smooth surface misses (edges, text,
   pins). The coarse grid is predicted from its own neighbours.
3. Freedom outside the shape: the encoder fills the background with the
   smoothest continuation of the object (harmonic, Laplace's equation), so
   the board's outline costs nothing, and zeroes every coefficient that
   cannot reach an object pixel (a zero where zeros are the rule costs
   almost nothing, so the decoder does not need to know which ones).
4. Choice per coefficient: each value is kept, lowered or dropped by error
   plus LAMBDA x the bits it would cost at that point (rate-distortion).
5. A post-filter in the decoder, whose strength the encoder picks, smooths
   the leftover grain on flat areas without crossing edges.

Every symbol goes through one adaptive binary range coder (15-bit
probabilities, a learning rate of about 1/(n+1) per context). The context
of a coefficient is the size of its known neighbours, of its parent in the
coarser band and of the sibling bands at the same place, and the signs of
its left and upper neighbours.

The arithmetic is written so that web/photo.js gives the same pixels: the
same constants, multiplications only, the same order of operations.

Format (integers big-endian):
    u16 width, u16 height, u16 step (x 1/8 before the band gains),
    u8 post-filter threshold (0: off), range coder bytes
"""
import base64
import math
import os
import subprocess
import sys

import numpy as np

LEVELS = 5
# CDF 9/7 lifting, to 6 or 7 digits: lifting inverts exactly whatever the
# constants, so they only need to match web/photo.js (K x IK is 1 to 1e-6)
A = -1.586134
B = -0.05298
G = 0.882911
D = 0.443507
K = 1.149604
IK = 0.869865
CHROMA = 1.5        # chroma steps are this much coarser
# 1 / synthesis gain of one coefficient (L2 norm of its basis function):
# coarse grid, then (HL and LH, HH) for levels 1..LEVELS. Scaling the step by
# it spreads the error evenly over the bands.
INV_LL = 0.05682
INV = ((0.9901, 1.678), (0.5747, 1.034), (0.3135, 0.5495), (0.1761, 0.3049), (0.09434, 0.157))
DEADZONE = 0.5      # |c| / step below this is zero before the rate-distortion choice
RECON = 0.05        # a nonzero value is rebuilt at (|q| - RECON) * step
LAMBDA = 0.1        # rate-distortion trade-off, in squared steps per bit
REACH = 4           # band cells a coefficient reaches, at its own scale
BG_WHITE = 245      # lighter than this in every channel and joined to the border: background
POST_R = 2          # post-filter window: (2 * POST_R + 1) squared

PB = 15             # probability bits
ONE = 1 << PB
LIMIT = 5           # learning rate: 1/2, 1/4, 1/4, 1/8 ... down to 1/2^LIMIT
EG_CTX = 12
# Per (luma or chroma, level 1..LEVELS): zero (5 near x 3 wide), >1 (5),
# >2 (5), length of the rest (EG_CTX), sign (9)
ZC, G1, G2, EP, SG = 0, 15, 20, 25, 25 + EG_CTX
PER = SG + 9
GRID = 2 * (LEVELS + 1) * PER          # coarse grid contexts (luma, chroma)
SHAPE = GRID + 2 * PER                 # shape contexts
NCTX = SHAPE + 1024


# ── colour ──────────────────────────────────────────────────────────────────

def rgb_to_ycocg(rgb):
    r, g, b = (rgb[..., i].astype(np.float64) for i in range(3))
    co = r - b
    t = b + co * 0.5
    cg = g - t
    y = t + cg * 0.5
    return [y - 128.0, co, cg]


def ycocg_to_rgb(y, co, cg):
    y = y + 128.0
    t = y - cg * 0.5
    g = cg + t
    b = t - co * 0.5
    r = b + co
    out = np.stack([r, g, b], -1)
    return np.clip(np.floor(out + 0.5), 0, 255).astype(np.int64)


# ── wavelet ─────────────────────────────────────────────────────────────────

def _lift(x, n, c, odd):
    """x[i] += c * (x[i-1] + x[i+1]) for i of one parity, mirrored ends."""
    for i in range(1 if odd else 0, n, 2):
        l = x[i - 1] if i > 0 else x[1]
        r = x[i + 1] if i + 1 < n else x[i - 1]
        x[i] += c * (l + r)


def fwd1(v):
    n = len(v)
    x = list(v)
    if n < 2:
        return x
    _lift(x, n, A, True)
    _lift(x, n, B, False)
    _lift(x, n, G, True)
    _lift(x, n, D, False)
    return [x[i] * IK for i in range(0, n, 2)] + [x[i] * K for i in range(1, n, 2)]


def inv1(v):
    n = len(v)
    if n < 2:
        return list(v)
    h = (n + 1) >> 1
    x = [0.0] * n
    for i in range(h):
        x[2 * i] = v[i] * K
    for i in range(n - h):
        x[2 * i + 1] = v[h + i] * IK
    _lift(x, n, -D, False)
    _lift(x, n, -G, True)
    _lift(x, n, -B, False)
    _lift(x, n, -A, True)
    return x


def sizes(w, h):
    out = []
    for _ in range(LEVELS):
        out.append((w, h))
        w, h = (w + 1) >> 1, (h + 1) >> 1
    return out, (w, h)


def dwt2(a):
    a = a.copy()
    for w, h in sizes(a.shape[1], a.shape[0])[0]:
        for y in range(h):
            a[y, :w] = fwd1(a[y, :w])
        for x in range(w):
            a[:h, x] = fwd1(a[:h, x])
    return a


def idwt2(a):
    a = a.copy()
    for w, h in reversed(sizes(a.shape[1], a.shape[0])[0]):
        for x in range(w):
            a[:h, x] = inv1(a[:h, x])
        for y in range(h):
            a[y, :w] = inv1(a[y, :w])
    return a


def bands(w, h):
    """Detail bands, coarse to fine: (level, orientation, x0, y0, bw, bh).
    Level LEVELS is the coarsest; orientation 0 HL, 1 LH, 2 HH."""
    sz, grid = sizes(w, h)
    out = []
    for lv in range(LEVELS, 0, -1):
        W, H = sz[lv - 1]
        lw, lh = (W + 1) >> 1, (H + 1) >> 1
        out.append((lv, 0, lw, 0, W - lw, lh))
        out.append((lv, 1, 0, lh, lw, H - lh))
        out.append((lv, 2, lw, lh, W - lw, H - lh))
    return out, grid


def step_of(step8, c, lv, o):
    """Quantizer step; lv 0 is the coarse grid."""
    inv = INV_LL if lv == 0 else INV[lv - 1][o >> 1]
    return step8 * 0.125 * (CHROMA if c else 1.0) * inv


# ── shape ───────────────────────────────────────────────────────────────────

def shape_of(rgb):
    """Object pixels: everything but the light pixels joined to the border."""
    from scipy import ndimage
    return ndimage.binary_fill_holes(rgb.min(-1) < BG_WHITE)


def reach(mask):
    """need[lv]: the cells of the level-lv grid (the bands of level lv; at
    LEVELS also the coarse grid) that can change an object pixel. Halve
    with OR, then grow REACH cells across and REACH down. Encoder only."""
    need = {}
    m = mask.astype(np.uint8)
    for lv in range(1, LEVELS + 1):
        h, w = m.shape
        p = np.zeros(((h + 1) >> 1, (w + 1) >> 1), np.uint8)
        for dy in (0, 1):
            for dx in (0, 1):
                s = m[dy::2, dx::2]
                p[:s.shape[0], :s.shape[1]] |= s
        m = p
        g = m.copy()
        for _ in range(REACH):
            g = g | np.pad(g, 1)[1:-1, 2:] | np.pad(g, 1)[1:-1, :-2]
        for _ in range(REACH):
            g = g | np.pad(g, 1)[2:, 1:-1] | np.pad(g, 1)[:-2, 1:-1]
        need[lv] = g
    return need


def fill(plane, mask, iters=600):
    """Harmonic continuation of the object into the background."""
    out = plane.copy()
    bg = ~mask
    out[bg] = plane[mask].mean()
    for _ in range(iters):
        p = np.pad(out, 1, mode="edge")
        avg = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]) * 0.25
        out[bg] = avg[bg]
    return out


# ── range coder ─────────────────────────────────────────────────────────────

def _adapt(rc, i, b):
    """The probability of a 0 moves towards the bit seen: halfway the first
    time, then about 1/(n+1), down to 1/2^LIMIT. Never 0 or certain."""
    n = rc.n[i]
    sh = min(LIMIT, (n + 1).bit_length())
    p = rc.p[i]
    p = p - (p >> sh) if b else p + ((ONE - p) >> sh)
    rc.p[i] = min(max(p, 32), ONE - 32)
    rc.n[i] = n + 1


class Encoder:
    def __init__(self):
        self.low, self.range, self.cache, self.size = 0, 0xFFFFFFFF, 0, 1
        self.out = bytearray()
        self.p = [ONE >> 1] * NCTX
        self.n = [0] * NCTX
        self.pick = None

    def _shift(self):
        if self.low < 0xFF000000 or self.low >= 1 << 32:
            carry = self.low >> 32
            t = self.cache
            while True:
                self.out.append((t + carry) & 0xFF)
                t = 0xFF
                self.size -= 1
                if not self.size:
                    break
            self.cache = (self.low >> 24) & 0xFF
        self.size += 1
        self.low = (self.low & 0x00FFFFFF) << 8

    def _norm(self):
        while self.range < 1 << 24:
            self.range = (self.range << 8) & 0xFFFFFFFF
            self._shift()

    def bit(self, i, b):
        bound = (self.range >> PB) * self.p[i]
        if b:
            self.low += bound
            self.range -= bound
        else:
            self.range = bound
        _adapt(self, i, b)
        self._norm()
        return b

    def raw(self, b):
        self.range >>= 1
        if b:
            self.low += self.range
        self._norm()
        return b

    def finish(self):
        for _ in range(5):
            self._shift()
        return bytes(self.out[1:])          # the first byte is always 0


class Decoder:
    def __init__(self, data):
        self.d, self.o = data, 4
        self.code = int.from_bytes((bytes(data[:4]) + b"\0\0\0\0")[:4], "big")
        self.range = 0xFFFFFFFF
        self.p = [ONE >> 1] * NCTX
        self.n = [0] * NCTX
        self.pick = None

    def _norm(self):
        while self.range < 1 << 24:
            self.range <<= 8
            self.code = (self.code << 8) | (self.d[self.o] if self.o < len(self.d) else 0)
            self.o += 1

    def bit(self, i, _=0):
        bound = (self.range >> PB) * self.p[i]
        if self.code < bound:
            self.range = bound
            b = 0
        else:
            self.code -= bound
            self.range -= bound
            b = 1
        _adapt(self, i, b)
        self._norm()
        return b

    def raw(self, _=0):
        self.range >>= 1
        b = 0
        if self.code >= self.range:
            self.code -= self.range
            b = 1
        self._norm()
        return b


class _Cost:
    """Bits a value would take at the coder's current probabilities,
    without coding it or moving them."""
    def __init__(self, rc):
        self.p, self.bits = rc.p, 0.0

    def bit(self, i, b):
        q = self.p[i] / ONE
        self.bits -= math.log2(1 - q if b else q)
        return b

    def raw(self, b):
        self.bits += 1
        return b


# ── model: the same walk encodes and decodes ────────────────────────────────

def _bucket(m):
    return 0 if m == 0 else 1 if m <= 2 else 2 if m <= 5 else 3 if m <= 10 else 4


def _value(rc, base, m, v, wide, sgn):
    """One signed integer with the contexts at base. m: activity of the
    known neighbours in the band; wide (0..2): of the parent and siblings;
    sgn (0..8): signs of the left and upper neighbours. The decoder passes
    v = 0 and gets the value back."""
    a = abs(v)
    z = _bucket(m)
    if not rc.bit(base + ZC + z * 3 + wide, int(a != 0)):
        return 0
    if not rc.bit(base + G1 + z, int(a > 1)):
        a = 1
    elif not rc.bit(base + G2 + z, int(a > 2)):
        a = 2
    else:
        # Exp-Golomb of a - 2: unary length with contexts, then plain bits
        r = a - 2
        nb = r.bit_length() - 1
        k = 0
        while rc.bit(base + EP + min(k, EG_CTX - 1), int(k < nb)):
            k += 1
        val = 1
        for j in range(k - 1, -1, -1):
            val = (val << 1) | rc.raw((r >> j) & 1)
        a = val + 2
    return -a if rc.bit(base + SG + sgn, int(v < 0)) else a


def _sign(v):
    return 0 if v < 0 else 2 if v > 0 else 1


def _shape(rc, m):
    """Codes (encoder) or fills (decoder) the shape, row by row."""
    h, w = m.shape
    for y in range(h):
        for x in range(w):
            def at(dx, dy):
                xx, yy = x + dx, y + dy
                return int(m[yy, xx]) if 0 <= xx < w and yy >= 0 else 0
            c = (at(-1, 0) | at(-2, 0) << 1 | at(-3, 0) << 2 | at(-2, -1) << 3 | at(-1, -1) << 4 |
                 at(0, -1) << 5 | at(1, -1) << 6 | at(2, -1) << 7 | at(-1, -2) << 8 | at(1, -2) << 9)
            m[y, x] = rc.bit(SHAPE + c, int(m[y, x]))


def _walk(rc, q, w, h):
    """Codes (encoder) or fills (decoder) the quantized channels q[c]."""
    blist, (cw, chh) = bands(w, h)
    at = {(lv, o): (x0, y0, bw, bh) for lv, o, x0, y0, bw, bh in blist}
    for c in range(3):
        a = q[c]
        cls = 1 if c else 0
        # Coarse grid: each value from its left and upper neighbours (MED)
        base = GRID + cls * PER
        for y in range(chh):
            for x in range(cw):
                l = int(a[y, x - 1]) if x else (int(a[y - 1, x]) if y else 0)
                u = int(a[y - 1, x]) if y else l
                ul = int(a[y - 1, x - 1]) if x and y else u
                pred = max(min(l, u), min(max(l, u), l + u - ul))
                m = min((abs(l - ul) + abs(u - ul)) >> 1, 20)
                a[y, x] = pred + _value(rc, base, m, int(a[y, x]) - pred, 0, 4)
        for lv, o, x0, y0, bw, bh in blist:
            base = (cls * (LEVELS + 1) + lv) * PER
            par = at.get((lv + 1, o))
            sibs = [at[(lv, s)] for s in range(o)]
            for y in range(bh):
                for x in range(bw):
                    l = int(a[y0 + y, x0 + x - 1]) if x else 0
                    u = int(a[y0 + y - 1, x0 + x]) if y else 0
                    m = 2 * (abs(l) + abs(u))
                    if y:
                        if x:
                            m += abs(int(a[y0 + y - 1, x0 + x - 1]))
                        if x + 1 < bw:
                            m += abs(int(a[y0 + y - 1, x0 + x + 1]))
                    wd = 0
                    if par:
                        px0, py0, pbw, pbh = par
                        wd += 2 * abs(int(a[py0 + min(y >> 1, pbh - 1), px0 + min(x >> 1, pbw - 1)]))
                    for sx0, sy0, sbw, sbh in sibs:
                        wd += abs(int(a[sy0 + min(y, sbh - 1), sx0 + min(x, sbw - 1)]))
                    wide = 0 if wd == 0 else 1 if wd <= 2 else 2
                    sgn = _sign(l) * 3 + _sign(u)
                    v = int(a[y0 + y, x0 + x])
                    if rc.pick:
                        v = rc.pick(c, y0 + y, x0 + x, (base, m, wide, sgn), v)
                    a[y0 + y, x0 + x] = _value(rc, base, m, v, wide, sgn)


# ── post-filter ─────────────────────────────────────────────────────────────

def recip():
    """65536 / n, rounded, for the post-filter's mean (no division in JS)."""
    return [0] + [round(65536 / n) for n in range(1, (2 * POST_R + 1) ** 2 + 1)]


def post(rgb, mask, t):
    """Each object pixel becomes the mean of the object pixels around it
    whose luma is within t of its own. Integer, as in web/photo.js."""
    if not t:
        return rgb
    h, w, _ = rgb.shape
    y = (rgb[..., 0] * 77 + rgb[..., 1] * 150 + rgb[..., 2] * 29) >> 8
    acc = np.zeros_like(rgb)
    n = np.zeros((h, w), np.int64)
    for dy in range(-POST_R, POST_R + 1):
        for dx in range(-POST_R, POST_R + 1):
            sy = slice(max(0, -dy), h - max(0, dy))
            sx = slice(max(0, -dx), w - max(0, dx))
            ty = slice(max(0, dy), h - max(0, -dy))
            tx = slice(max(0, dx), w - max(0, -dx))
            ok = mask[ty, tx] & (np.abs(y[ty, tx] - y[sy, sx]) <= t)
            acc[sy, sx] += rgb[ty, tx] * ok[..., None]
            n[sy, sx] += ok
    rc = np.array(recip())
    out = (acc * rc[n][..., None] + 32768) >> 16
    return np.where(mask[..., None], out, rgb)


# ── encode / decode ─────────────────────────────────────────────────────────

def _quantize(planes, step8, need):
    """Quantized channels, and each detail coefficient in units of its step."""
    h, w = planes[0].shape
    blist, (cw, chh) = bands(w, h)
    q, frac = [], []
    for c, plane in enumerate(planes):
        t = dwt2(plane)
        out = np.zeros((h, w), np.int64)
        f = np.zeros((h, w))
        out[:chh, :cw] = np.round(t[:chh, :cw] / step_of(step8, c, 0, 0)) * need[LEVELS][:chh, :cw]
        for lv, o, x0, y0, bw, bh in blist:
            v = t[y0:y0 + bh, x0:x0 + bw] / step_of(step8, c, lv, o)
            f[y0:y0 + bh, x0:x0 + bw] = v
            v = np.sign(v) * np.floor(np.abs(v) + (1 - DEADZONE))
            out[y0:y0 + bh, x0:x0 + bw] = v * need[lv][:bh, :bw]
        q.append(out)
        frac.append(f)
    return q, frac


def _rdo(rc, frac):
    """rc.pick: of the quantized value and the ones one and two steps closer
    to zero, the one with the least squared error + LAMBDA * bits, the bits
    at the coder's probabilities at that point. Every band has the same error
    per squared step: the steps carry the band gains."""
    def pick(c, y, x, ctx, v):
        if v == 0:
            return v
        f = frac[c][y, x]
        s = 1 if v > 0 else -1
        best, bj = v, None
        for cand in (v, v - s, v - 2 * s):
            if cand * s < 0:
                break
            cost = _Cost(rc)
            _value(cost, ctx[0], ctx[1], cand, ctx[2], ctx[3])
            rec = 0.0 if cand == 0 else s * (abs(cand) - RECON)
            j = (f - rec) ** 2 + LAMBDA * cost.bits
            if bj is None or j < bj:
                best, bj = cand, j
        return best
    return pick


def _surface(q, w, h, step8):
    blist, (cw, chh) = bands(w, h)
    planes = []
    for c in range(3):
        t = np.zeros((h, w))
        t[:chh, :cw] = q[c][:chh, :cw] * step_of(step8, c, 0, 0)
        for lv, o, x0, y0, bw, bh in blist:
            v = q[c][y0:y0 + bh, x0:x0 + bw]
            mag = np.maximum(np.abs(v) - RECON, 0)
            t[y0:y0 + bh, x0:x0 + bw] = np.where(v < 0, -mag, mag) * step_of(step8, c, lv, o)
        planes.append(idwt2(t))
    return ycocg_to_rgb(*planes)


def _pack(w, h, step8, t, body):
    data = w.to_bytes(2, "big") + h.to_bytes(2, "big") + step8.to_bytes(2, "big") + bytes([t]) + body
    # Trailing zeros: the decoder reads past the end as zeros anyway
    while len(data) > 7 and data[-1] == 0:
        data = data[:-1]
    return data


def encode(rgb, step8, score=None, mask=None):
    """Bytes for rgb (h x w x 3, uint8) at quantizer step step8. score(rgba)
    rates a decoded picture (higher is better) to choose the post-filter;
    without it, the post-filter is off."""
    h, w, _ = rgb.shape
    if mask is None:
        mask = shape_of(rgb)
    need = reach(mask)
    planes = [fill(p, mask) for p in rgb_to_ycocg(rgb)]
    q, frac = _quantize(planes, step8, need)
    rc = Encoder()
    rc.pick = _rdo(rc, frac)
    _shape(rc, mask.astype(np.uint8))
    _walk(rc, q, w, h)
    body = rc.finish()
    t = 0
    if score:
        surf = _surface(q, w, h, step8)
        best = None
        for cand in range(0, 33, 4):
            s = score(_rgba(post(surf, mask, cand), mask))
            if best is None or s > best:
                best, t = s, cand
    return _pack(w, h, step8, t, body)


def _rgba(rgb, mask):
    return np.dstack([rgb, mask * 255]).astype(np.uint8)


def decode(data):
    """RGBA (h x w x 4, uint8), the background transparent."""
    w = int.from_bytes(data[0:2], "big")
    h = int.from_bytes(data[2:4], "big")
    step8 = int.from_bytes(data[4:6], "big")
    t = data[6]
    rc = Decoder(data[7:])
    mask = np.zeros((h, w), np.uint8)
    _shape(rc, mask)
    mask = mask.astype(bool)
    q = [np.zeros((h, w), np.int64) for _ in range(3)]
    _walk(rc, q, w, h)
    return _rgba(post(_surface(q, w, h, step8), mask, t), mask)


# ── quality and size ────────────────────────────────────────────────────────

def ssim(ref_rgb, rgba):
    """Structural similarity of the luma, the decoded picture over white."""
    from scipy import ndimage
    a = rgba[..., 3:4] / 255.0
    out = rgba[..., :3] * a + 255 * (1 - a)

    def luma(x):
        x = x.astype(np.float64)
        return x[..., 0] * 0.299 + x[..., 1] * 0.587 + x[..., 2] * 0.114

    x, y = luma(ref_rgb), luma(out)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2

    def f(v):
        return ndimage.gaussian_filter(v, 1.5, truncate=3.5)

    mx, my = f(x), f(y)
    vx, vy, cxy = f(x * x) - mx * mx, f(y * y) - my * my, f(x * y) - mx * my
    return float((((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2))).mean())


def encode_to(rgb, size):
    """The finest step whose bytes fit in size."""
    lo, hi, best = 16, 4096, None
    while lo <= hi:
        mid = (lo + hi) >> 1
        data = encode(rgb, mid, lambda rgba: ssim(rgb, rgba))
        if len(data) <= size:
            best, hi = data, mid - 1
        else:
            lo = mid + 1
    return best


# ── check (CI) ──────────────────────────────────────────────────────────────

def check():
    """web/photo.js as the device serves it (minified, with web/esp01.img in
    it) must rebuild exactly the pixels of decode(). Needs node."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import embed_web

    with open(embed_web.PHOTO, "rb") as f:
        data = f.read()
    js = embed_web.build()[1]["WEB_PHOTO"][1].decode("utf-8")
    harness = (
        "let got;"
        "globalThis.ImageData=class{constructor(d,w,h){this.data=d;this.width=w;this.height=h}};"
        "globalThis.document={getElementById:()=>({getContext:()=>({putImageData:i=>{got=i}})})};"
        + js +
        ";process.stdout.write(Buffer.from(got.data.buffer).toString('base64'))"
    )
    out = subprocess.run(["node", "-"], input=harness.encode("utf-8"), capture_output=True, check=True)
    want = decode(data)
    got = np.frombuffer(base64.b64decode(out.stdout), np.uint8)
    if got.size != want.size or not np.array_equal(got.reshape(want.shape), want):
        sys.exit("web/photo.js does not rebuild the pixels of tools/photo_codec.py")
    w, h = want.shape[1], want.shape[0]
    print(f"web/photo.js rebuilds web/esp01.img ({w}x{h}, {len(data)} B) exactly")


if __name__ == "__main__":
    if sys.argv[1:] != ["--check"]:
        sys.exit("usage: photo_codec.py --check   (encoding: python tools/embed_assets.py)")
    check()
