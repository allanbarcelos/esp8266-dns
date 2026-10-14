# Panel photo codec

The ESP-01 photo on the panel is not an image file. The firmware stores a
recipe of about 1.3 KB, and `web/photo.js` rebuilds the picture in the
browser. The device only serves bytes; all the work happens in the browser.

| Piece | Where | Size |
| --- | --- | --- |
| Encoder and reference decoder | `tools/photo_codec.py` (build time only) | — |
| Encoded photo, 220×209 | `web/esp01.img` | 1,342 B |
| Decoder + photo, as served (`/photo.js`) | `WebAssets.h` | 3,193 B gzip |

## The idea: a smooth surface plus corrections

Shadows, depth and gradients change slowly, so a coarse grid of numbers can
describe them and a smooth function can fill in the rest. Edges, text and
pins are what that surface misses, and only those get stored as corrections.
A wavelet expresses exactly that split:

```
RGB ─► YCoCg ─► CDF 9/7 wavelet, 5 levels ─► quantizer ─► context model ─► range coder
                 │                             (rate-distortion)
                 ├─ coarse grid 7×7: the smooth surface (gradients, shadows)
                 └─ detail bands: corrections, mostly zero
```

Inverse lifting turns the coarse grid back into a smooth surface. The
predict steps are interpolations between neighbours, so the coarse grid acts
as the parameters of the generating function. Each finer level adds only the
difference between that interpolation and the picture.

## Steps

1. **Shape.** The object is the set of non-white pixels connected to the
   picture, with holes filled. The shape is coded pixel by pixel, each pixel
   in the context of its 10 decoded neighbours (a JBIG-style template, ~117 B).
   On the page the background is transparent, so the photo fits the light
   and dark themes.
2. **Freedom outside the shape.** The decoder throws the background away, so
   the encoder may put anything there. It puts the smoothest continuation of
   the object (a harmonic fill: Laplace's equation solved by Jacobi
   iterations). The board's black-on-white edge, the costliest feature for a
   wavelet, then no longer exists. The encoder also zeroes every coefficient
   whose support cannot reach an object pixel. A zero where zeros are the
   rule costs almost nothing, so the decoder does not need to know which
   coefficients these are.
3. **Colour.** YCoCg separates brightness from colour. Chroma steps are
   1.5× coarser.
4. **Quantizer.** Each band's step is divided by the L2 norm of its synthesis
   basis function, so a step of error costs the same in every band. Then a
   rate-distortion choice looks at each value, and at the values one and two
   steps closer to zero. It keeps the one with the least `error² + λ·bits`,
   with the bits counted at the coder's probabilities at that point.
   Isolated values, which cost the most, are the ones dropped.
5. **Context model.** Each coefficient is binarized as zero? / >1? / >2? /
   Exp-Golomb rest / sign. The zero flag depends on the size of the known
   neighbours in the band (5 classes) and of the parent and sibling bands at
   the same place (3 classes). The sign depends on the signs of the left and
   upper neighbours. The coarse grid is predicted from its own neighbours
   (MED, as in LOCO-I).
6. **Range coder.** Binary, LZMA-style, with 15-bit probabilities. Each
   context learns at about `1/(n+1)` (halfway the first time, then smaller
   steps, down to `1/32`). The picture is small (a few thousand nonzero values), so
   fast learning matters more than more contexts. A slower rate or more
   contexts both measured worse.
7. **Post-filter.** In the decoder, each object pixel becomes the mean of
   the object pixels in its 5×5 window whose luma is within `T` of its own.
   This smooths the leftover grain on the flat board without crossing edges.
   The encoder picks `T` by structural similarity (SSIM).

## Format

```
u16 width, u16 height     (big-endian)
u16 step                  quantizer step × 8, before the band gains
u8  T                     post-filter threshold, 0 = off
…   range coder bytes     shape, then Y, Co, Cg: coarse grid, bands coarse to fine
```

Trailing zero bytes are dropped. The decoder reads past the end as zeros.

## Decoder constraints

`web/photo.js` repeats the Python decoder operation by operation, with the
same constants and in the same order, so both give the same pixels. CI
checks this with `python3 tools/photo_codec.py --check`, which runs the
script as served (minified, with the photo in it) in Node and compares every
byte. The project's minifier only strips comments and spaces, so the script
uses short names, has no `/` (it multiplies by inverses: `0.125`,
`Math.pow(n, -1)`), and has no regex literal or template string. The wavelet
constants have 6 to 7 digits: lifting inverts exactly whatever the constants
are, as long as both sides use the same ones. The decoder takes about
60–100 ms.

## Results

220×209, SSIM of the luma over white, against WebP (`method=6`) at the same
size or just under:

| Bytes | This codec | WebP |
| ---: | ---: | ---: |
| 1,100 | 0.868 | 0.841 (quality 0: WebP cannot go lower) |
| 1,350 | **0.886** (shipped) | 0.875 |
| 1,700 | 0.906 | 0.901 |
| 2,048 | 0.925 | 0.919 |
| 2,560 | 0.939 | 0.945 |

Below about 2.2 KB the codec beats WebP, and it also carries transparency,
which WebP would pay extra for. Above that, WebP's block prediction wins.
The browser decodes WebP for free, while this decoder costs about 1.8 KB of
gzipped script. That is why it pays off only because the firmware has no room
for anything bigger: under the 500,000-byte cap, decoder and photo together
fit in about 3.2 KB.

## Changing the photo

```
pip install numpy scipy pillow
python tools/embed_assets.py      # docs/esp-01.jpg → web/esp01.img (PHOTO_BYTES)
python tools/embed_web.py         # web/photo.js + web/esp01.img → WebAssets.h
python tools/photo_codec.py --check
```

`PHOTO_BYTES` in `tools/embed_assets.py` sets the size. The encoder looks
for the finest step that fits.
