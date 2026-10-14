// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

// Panel photo decoder. The picture is not an image file but a recipe
// (tools/photo_codec.py, which also holds the reference decoder): the
// object's shape, a smooth surface from a coarse grid (gradients, shadows,
// depth) and wavelet corrections where that surface is not enough, all
// through one adaptive range coder. This script rebuilds it into the canvas
// #ph, the background transparent. tools/embed_web.py puts the bytes of
// web/esp01.img in the atob() string, in base64.
//
// Same arithmetic as the Python decoder, in the same order, so the pixels
// are the same. The minifier only strips comments and spaces: short names,
// no division (multiply by the inverse), no regex literal, no template string.

(() => {
    var D = [...atob('@PHOTO@')].map(c => c.charCodeAt()), i, j, x, y, c, k, l, u, v, a, b, e, t,
        W = D[0] * 256 + D[1], H = D[2] * 256 + D[3], Q = (D[4] * 256 + D[5]) * 0.125, T = D[6], n = W * H,
        // CDF 9/7 lifting steps of the inverse (-D, -G, -B, -A), K, 1 / K
        LF = [-0.443507, -0.882911, 0.05298, 1.586134], K = 1.149604, IK = 0.869865,
        // 1 / gain of a coefficient: coarse grid, then (HL and LH, HH) of levels 1..5
        IG = [0.05682, 0.9901, 1.678, 0.5747, 1.034, 0.3135, 0.5495, 0.1761, 0.3049, 0.09434, 0.157],
        // Contexts: 46 per (luma or chroma, level 0..5), coarse grid at 552, shape at 644
        P = new Uint16Array(1668).fill(16384), N = new Int32Array(1668),
        R = 4294967295, C = ((D[7] * 256 + D[8]) * 256 + D[9]) * 256 + D[10], o = 11,
        ab = v => v < 0 ? -v : v,
        sg = v => v < 0 ? 0 : v ? 2 : 1,
        M = [], S = [], w = W, h = H, bl = [], at = {}, Y = [], O = [], Z = [], I = new Uint8ClampedArray(n * 4);

    // One bit with context i, its probability learning at about 1/(n+1);
    // without i, a bit at 1/2
    function B(i) {
        var r = 0, p, b, h;
        if (i == null) {
            R = R >>> 1;
            if (C >= R) { C -= R; r = 1; }
        } else {
            p = P[i]; b = (R >>> 15) * p; h = 32 - Math.clz32(++N[i]);
            if (h > 5) h = 5;
            if (C < b) { R = b; p += (32768 - p) >> h; }
            else { C -= b; R -= b; r = 1; p -= p >> h; }
            P[i] = p < 32 ? 32 : p > 32736 ? 32736 : p;
        }
        while (R < 16777216) { R *= 256; C = C * 256 + (D[o++] | 0); }
        return r;
    }

    // A signed value: zero?, >1?, >2?, Exp-Golomb, sign
    function V(b, m, w, g) {
        var z = m ? m < 3 ? 1 : m < 6 ? 2 : m < 11 ? 3 : 4 : 0, a = 1, k = 0, v = 1;
        if (!B(b + z * 3 + w)) return 0;
        if (B(b + 15 + z)) {
            a = 2;
            if (B(b + 20 + z)) {
                while (B(b + 25 + (k < 11 ? k : 11))) k++;
                while (k--) v = v * 2 + B();
                a = v + 2;
            }
        }
        return B(b + 37 + g) ? -a : a;
    }

    // Inverse lifting of one line
    function inv(L) {
        var n = L.length, h = (n + 1) >> 1, X = [], i, j, k;
        if (n < 2) return L;
        for (i = 0; i < n; i++) X[i] = i % 2 ? L[h + (i >> 1)] * IK : L[i >> 1] * K;
        for (j = 0; j < 4; j++) for (k = LF[j], i = j % 2; i < n; i += 2)
            X[i] += k * ((i ? X[i - 1] : X[1]) + (i + 1 < n ? X[i + 1] : X[i - 1]));
        return X;
    }

    // Shape: each pixel from the 10 known ones around it
    var m = (dx, dy) => x + dx >= 0 && x + dx < W && y + dy >= 0 ? M[(y + dy) * W + x + dx] : 0;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++)
        M[y * W + x] = B(644 + (m(-1, 0) | m(-2, 0) << 1 | m(-3, 0) << 2 | m(-2, -1) << 3 | m(-1, -1) << 4 |
            m(0, -1) << 5 | m(1, -1) << 6 | m(2, -1) << 7 | m(-1, -2) << 8 | m(1, -2) << 9));

    // Level sizes, and the detail bands coarse to fine: [level, orientation, x0, y0, w, h]
    for (i = 0; i < 5; i++) { S.push([w, h]); w = (w + 1) >> 1; h = (h + 1) >> 1; }
    for (l = 5; l; l--) {
        [a, b] = S[l - 1]; u = (a + 1) >> 1; v = (b + 1) >> 1;
        [[l, 0, u, 0, a - u, v], [l, 1, 0, v, u, b - v], [l, 2, u, v, a - u, b - v]].map(e => bl.push(at[l * 3 + e[1]] = e));
    }

    // Each channel: coefficients, then back to values, then the inverse wavelet
    for (c = 0; c < 3; c++) {
        var A = new Int32Array(n), F = new Float64Array(n), cl = c ? 46 : 0, st = Q * (c ? 1.5 : 1),
            g = (t, X, Y) => ab(A[(t[3] + Math.min(Y, t[5] - 1)) * W + t[2] + Math.min(X, t[4] - 1)]);
        // Coarse grid, predicted from left and upper (MED)
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            j = y * W + x;
            l = x ? A[j - 1] : y ? A[j - W] : 0;
            u = y ? A[j - W] : l;
            a = x && y ? A[j - W - 1] : u;
            k = (ab(l - a) + ab(u - a)) >> 1;
            A[j] = Math.max(Math.min(l, u), Math.min(Math.max(l, u), l + u - a)) + V(552 + cl, k < 20 ? k : 20, 0, 4);
            F[j] = A[j] * (st * IG[0]);
        }
        for (b of bl) {
            var [lv, or, x0, y0, bw, bh] = b, pa = at[lv * 3 + 3 + or], s = st * IG[lv * 2 - 1 + (or >> 1)];
            for (y = 0; y < bh; y++) for (x = 0; x < bw; x++) {
                j = (y0 + y) * W + x0 + x;
                l = x ? A[j - 1] : 0;
                u = y ? A[j - W] : 0;
                k = 2 * (ab(l) + ab(u));
                if (y) {
                    if (x) k += ab(A[j - W - 1]);
                    if (x + 1 < bw) k += ab(A[j - W + 1]);
                }
                e = pa ? 2 * g(pa, x >> 1, y >> 1) : 0;
                for (t = 0; t < or; t++) e += g(at[lv * 3 + t], x, y);
                v = A[j] = V(cl * 6 + lv * 46, k, e ? e < 3 ? 1 : 2 : 0, sg(l) * 3 + sg(u));
                a = ab(v) - 0.05;
                if (a < 0) a = 0;
                F[j] = (v < 0 ? -a : a) * s;
            }
        }
        for (i = 4; i >= 0; i--) {
            [a, b] = S[i];
            for (x = 0; x < a; x++) {
                for (t = [], y = 0; y < b; y++) t[y] = F[y * W + x];
                t = inv(t);
                for (y = 0; y < b; y++) F[y * W + x] = t[y];
            }
            for (y = 0; y < b; y++) F.set(inv([...F.subarray(y * W, y * W + a)]), y * W);
        }
        Y.push(F);
    }

    // YCoCg to RGB, then the post-filter: the mean of the object pixels around
    // whose luma is within T (65536 / count instead of a division)
    for (i = 0; i < n; i++) {
        t = Y[0][i] + 128 - Y[2][i] * 0.5;
        b = t - Y[1][i] * 0.5;
        [b + Y[1][i], Y[2][i] + t, b].map((v, j) => {
            v = Math.floor(v + 0.5);
            O[i * 3 + j] = v < 0 ? 0 : v > 255 ? 255 : v;
        });
        Z[i] = (O[i * 3] * 77 + O[i * 3 + 1] * 150 + O[i * 3 + 2] * 29) >> 8;
    }
    for (i = y = 0; y < H; y++) for (x = 0; x < W; x++, i++) {
        a = [0, 0, 0]; k = 0;
        if (T && M[i]) for (e = -2; e < 3; e++) for (l = -2; l < 3; l++) {
            j = i + e * W + l;
            if (y + e >= 0 && y + e < H && x + l >= 0 && x + l < W && M[j] && ab(Z[j] - Z[i]) <= T) {
                for (t = 0; t < 3; t++) a[t] += O[j * 3 + t];
                k++;
            }
        }
        u = Math.round(65536 * Math.pow(k, -1));
        for (j = 0; j < 3; j++) I[i * 4 + j] = k ? (a[j] * u + 32768) >> 16 : O[i * 3 + j];
        I[i * 4 + 3] = M[i] ? 255 : 0;
    }
    c = document.getElementById('ph');
    c.width = W;
    c.height = H;
    c.getContext('2d').putImageData(new ImageData(I, W, H), 0, 0);
})();
