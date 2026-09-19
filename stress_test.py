#!/usr/bin/env python3
"""
stress_test.py — stress suite for the photo.c port of photos_embed.py.

SPDX-License-Identifier: Apache-2.0
Copyright 2026 Luka Gejak

Usage:
  python3 stress_test.py main    # combinatorial, real files, adversarial, fuzz, parallel
  python3 stress_test.py scale   # near-limit payloads (heavy: ~2 GB disk, a few minutes)
  python3 stress_test.py all     # everything

Requires: Pillow + numpy (only for cross-checking against the Python original).
"""

import concurrent.futures
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

BASE = os.path.dirname(os.path.abspath(__file__))
C_BIN = os.path.join(BASE, "photo")
ASAN_BIN = os.path.join(tempfile.gettempdir(), "photo_stress_asan")
PY_SCRIPT = os.path.join(BASE, "photo.py")
WORK = os.path.join(tempfile.gettempdir(), "photo_stress")

MAX_BYTES = 200_000_000 * 3 - 8  # tool's per-PNG payload limit

SAN_LABEL = "ASan+UBSan"
PASS = 0
FAIL = 0
FAILURES = []


def check(cond, label, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        FAILURES.append(f"{label}  {detail}")
        print(f"  FAIL: {label}  {detail}")


def run(cmd, timeout=120):
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def c_encode(inp, outp, timeout=120):
    return run([C_BIN, "encode", inp, outp], timeout)


def c_decode(inp, outp, timeout=120):
    return run([C_BIN, "decode", inp, outp], timeout)


def py_encode(inp, outp, timeout=300):
    return run([sys.executable, PY_SCRIPT, "encode", inp, outp], timeout)


def py_decode(inp, outp, timeout=300):
    return run([sys.executable, PY_SCRIPT, "decode", inp, outp], timeout)


def parse_sha(stdout):
    for line in stdout.splitlines():
        if line.strip().startswith("SHA-256:"):
            return line.split("SHA-256:")[1].strip()
    return None


def roundtrip_c(tag, data, py_cross=True, sha_check=True):
    """C encode -> C decode -> compare; optional Python cross-checks."""
    d = os.path.join(WORK, "rt")
    os.makedirs(d, exist_ok=True)
    f_in = os.path.join(d, f"{tag}.bin")
    f_png = os.path.join(d, f"{tag}.png")
    f_out = os.path.join(d, f"{tag}.out")
    with open(f_in, "wb") as fh:
        fh.write(data)

    r = c_encode(f_in, f_png)
    if r.returncode != 0:
        check(False, f"{tag}: C encode", r.stderr.strip()[:90])
        return
    if sha_check:
        check(parse_sha(r.stdout) == sha(data), f"{tag}: encode stdout SHA-256")

    r = c_decode(f_png, f_out)
    got = open(f_out, "rb").read() if r.returncode == 0 else b""
    check(r.returncode == 0 and got == data, f"{tag}: C->C round-trip")

    if py_cross:
        p_png = os.path.join(d, f"{tag}.pypng")
        p_out = os.path.join(d, f"{tag}.pyout")
        r = py_encode(f_in, p_png)
        if r.returncode != 0:
            check(False, f"{tag}: PY encode", r.stderr.strip()[:90])
        else:
            r = c_decode(p_png, p_out)
            got = open(p_out, "rb").read() if r.returncode == 0 else b""
            check(r.returncode == 0 and got == data, f"{tag}: PY->C round-trip")
            r = py_decode(f_png, f_out)
            got = open(f_out, "rb").read() if r.returncode == 0 else b""
            check(r.returncode == 0 and got == data, f"{tag}: C->PY round-trip")
        for p in (p_png, p_out):
            if os.path.exists(p):
                os.remove(p)
    for p in (f_in, f_png, f_out):
        if os.path.exists(p):
            os.remove(p)


# --------------------------------------------------------------------------
# payload generators
# --------------------------------------------------------------------------

def payload(kind, size):
    if kind == "rnd":
        if size > (1 << 20):          # os.urandom for big payloads (much faster)
            return os.urandom(size)
        data = bytearray()
        while len(data) < size:
            data += hashlib.sha256(len(data).to_bytes(8, "big")).digest()
        return bytes(data[:size])
    if kind == "zeros":
        return b"\x00" * size
    if kind == "ff":
        return b"\xff" * size
    if kind == "text":
        line = b"The quick brown fox jumps over the lazy dog 0123456789.\n"
        return (line * (size // len(line) + 1))[:size]
    if kind == "patt":
        return bytes(i % 251 for i in range(size))
    if kind == "pngmagic":
        return (b"\x89PNG\r\n\x1a\n" + payload("rnd", max(0, size - 8)))[:size]
    if kind == "alt":
        return (b"\x00\xff" * (size // 2 + 1))[:size]
    raise ValueError(kind)


KINDS = ["rnd", "zeros", "ff", "text", "patt", "pngmagic", "alt"]
SIZES = [0, 1, 2, 3, 7, 8, 9, 63, 64, 65, 255, 256, 257, 1023, 4095, 4096,
         65535, 65536, 65537, 131071, 262144, 1000003]


# --------------------------------------------------------------------------
# phases
# --------------------------------------------------------------------------

def phase_combinatorial():
    print("[1] combinatorial sweep: sizes x content types")
    t0 = time.time()
    py_sizes = {0, 3, 8, 256, 65535, 65537, 262144}   # subset for slow PY cross-check
    for size in SIZES:
        for kind in KINDS:
            cross = size in py_sizes
            roundtrip_c(f"{kind}_{size}", payload(kind, size), py_cross=cross)
    print(f"    done in {time.time()-t0:.1f}s")

    print("[1b] determinism: same input twice -> identical PNG bytes")
    for kind in ("rnd", "text"):
        data = payload(kind, 50000)
        d = os.path.join(WORK, "rt")
        a, b = os.path.join(d, "det1.png"), os.path.join(d, "det2.png")
        f_in = os.path.join(d, "det.bin")
        open(f_in, "wb").write(data)
        assert c_encode(f_in, a).returncode == 0
        assert c_encode(f_in, b).returncode == 0
        check(open(a, "rb").read() == open(b, "rb").read(), f"determinism ({kind})")
        os.remove(a); os.remove(b); os.remove(f_in)


def make_archives(d):
    """Real-world files: archives, executables, media, source."""
    src = os.path.join(d, "arcsrc")
    os.makedirs(src, exist_ok=True)
    files = {
        "notes.txt": b"meeting notes\n" * 400,
        "data.bin": payload("rnd", 300_000),
        "table.csv": b"id,name,val\n" + b"".join(
            b"%d,item%d,%d\n" % (i, i, i * 7) for i in range(2000)),
    }
    for name, content in files.items():
        open(os.path.join(src, name), "wb").write(content)

    import bz2, gzip, lzma, tarfile, zipfile
    out = {}
    tp = os.path.join(d, "docs.tar")
    with tarfile.open(tp, "w") as tf:
        for name in files:
            tf.add(os.path.join(src, name), arcname=name)
    out["docs.tar"] = open(tp, "rb").read()
    with tarfile.open(os.path.join(d, "docs.tar.gz"), "w:gz") as tf:
        for name in files:
            tf.add(os.path.join(src, name), arcname=name)
    out["docs.tar.gz"] = open(os.path.join(d, "docs.tar.gz"), "rb").read()
    zp = os.path.join(d, "docs.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, content in files.items():
            zf.writestr(name, content)
    out["docs.zip"] = open(zp, "rb").read()
    out["rand.gz"] = gzip.compress(payload("rnd", 250_000))
    out["data.xz"] = lzma.compress(payload("text", 400_000))
    out["data.bz2"] = bz2.compress(payload("patt", 400_000))
    out["photo.py"] = open(PY_SCRIPT, "rb").read()
    out["photo.c"] = open(os.path.join(BASE, "photo.c"), "rb").read()
    out["photo_bin"] = open(C_BIN, "rb").read()
    for p in ("/bin/ls", "/bin/cat"):
        if os.path.exists(p):
            out[os.path.basename(p) + "_bin"] = open(p, "rb").read()
    # synthetic media
    rng = np.random.default_rng(42)
    img = np.zeros((512, 512, 3), np.uint8)
    yy, xx = np.mgrid[0:512, 0:512]
    img[:, :, 0] = (xx * 255 // 511).astype(np.uint8)
    img[:, :, 1] = (yy * 255 // 511).astype(np.uint8)
    img[:, :, 2] = rng.integers(0, 256, (512, 512), dtype=np.uint8)
    mp = os.path.join(d, "photo.png")
    Image.fromarray(img).save(mp)
    out["photo_png"] = open(mp, "rb").read()
    jp = os.path.join(d, "photo.jpg")
    Image.fromarray(img).save(jp, quality=87)
    out["photo_jpg"] = open(jp, "rb").read()
    shutil.rmtree(src)
    return out


def phase_real_files():
    print("[2] real-world files (archives, binaries, media)")
    t0 = time.time()
    d = os.path.join(WORK, "real")
    os.makedirs(d, exist_ok=True)
    for name, data in make_archives(d).items():
        roundtrip_c(name.replace(".", "_"), data, py_cross=True)
    print(f"    done in {time.time()-t0:.1f}s")

    print("[2b] matryoshka: PNG inside PNG inside PNG (depth 3)")
    d = os.path.join(WORK, "mat")
    os.makedirs(d, exist_ok=True)
    inner = payload("rnd", 5000)
    f = os.path.join(d, "l0.bin"); open(f, "wb").write(inner)
    for level in (1, 2, 3):
        assert c_encode(f, os.path.join(d, f"l{level}.png")).returncode == 0
        f = os.path.join(d, f"l{level}.png")
    ok = True
    for level in (3, 2, 1):
        r = c_decode(os.path.join(d, f"l{level}.png"), os.path.join(d, f"d{level-1}.bin"))
        ok &= r.returncode == 0
        f = os.path.join(d, f"d{level-1}.bin")
    check(ok and open(f, "rb").read() == inner, "matryoshka depth-3 round-trip")


# ---- crafted / adversarial PNG helpers ------------------------------------

def pchunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def build_png(w, h, body, ctype=2, depth=8, interlace=0, extra_chunks=b""):
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    return (b"\x89PNG\r\n\x1a\n" + pchunk(b"IHDR", ihdr) + extra_chunks
            + pchunk(b"IDAT", zlib.compress(bytes(body), 6)) + pchunk(b"IEND", b""))


def scanlines(flat, w, h, filters=None):
    stride = w * 3
    body = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        cur = flat[y * stride:(y + 1) * stride]
        ft = filters[y % len(filters)] if filters else 0
        body.append(ft)
        for i in range(stride):
            a = cur[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if ft == 0:
                pred = 0
            elif ft == 1:
                pred = a
            elif ft == 2:
                pred = b
            elif ft == 3:
                pred = (a + b) >> 1
            else:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            body.append((cur[i] - pred) & 0xFF)
        prev = cur
    return body


def planted_png(data, w, h, filters=None, extra_chunks=b""):
    payload_b = len(data).to_bytes(8, "big") + data
    need = len(payload_b)
    assert w * h * 3 >= need, (w, h, need)
    flat = bytearray(payload_b) + bytearray(w * h * 3 - need)
    return build_png(w, h, scanlines(flat, w, h, filters), extra_chunks=extra_chunks)


def phase_adversarial():
    print("[3] adversarial / crafted inputs")
    t0 = time.time()
    d = os.path.join(WORK, "adv")
    os.makedirs(d, exist_ok=True)
    data = payload("rnd", 1000)

    def expect(label, png_bytes, want_ok, out_equals=None):
        p = os.path.join(d, label.replace(" ", "_") + ".png")
        o = p + ".out"
        open(p, "wb").write(png_bytes)
        r = c_decode(p, o)
        if want_ok:
            good = r.returncode == 0
            if good and out_equals is not None:
                good = open(o, "rb").read() == out_equals
            check(good, label, r.stderr.strip()[:80])
        else:
            check(r.returncode == 1, label, f"rc={r.returncode} {r.stderr.strip()[:60]}")

    # unsupported-but-valid PNGs
    im = Image.new("P", (8, 8))
    im.putpalette([i % 256 for i in range(768)])
    im.save(os.path.join(d, "pal.png"))
    check(c_decode(os.path.join(d, "pal.png"), os.path.join(d, "x")).returncode == 1,
          "palette PNG rejected gracefully")
    Image.fromarray(np.arange(64, dtype=np.uint16).reshape(8, 8), "I;16").save(
        os.path.join(d, "d16.png"))
    check(c_decode(os.path.join(d, "d16.png"), os.path.join(d, "x")).returncode == 1,
          "16-bit PNG rejected gracefully")
    body = scanlines(bytearray(9 * 9 * 3), 3, 3)          # 3x3 dummy, interlace flag set
    expect("interlaced flag rejected", build_png(3, 3, body, interlace=1), False)

    # structural corruption
    good_png = planted_png(data, 40, 12)
    expect("truncated file", good_png[: len(good_png) * 3 // 5], False)
    bad = bytearray(good_png); bad[40] ^= 0xFF            # IHDR CRC region
    expect("IHDR byte corrupted", bytes(bad), False)
    # corrupt zlib header but fix chunk CRC so corruption reaches the inflater
    off, ln = 8, None
    while True:
        cl = struct.unpack(">I", good_png[off:off + 4])[0]
        if good_png[off + 4:off + 8] == b"IDAT":
            break
        off += 12 + cl
    idat = bytearray(good_png[off + 8:off + 8 + struct.unpack(">I", good_png[off:off+4])[0]])
    idat[0] ^= 0xFF
    expect("zlib header corrupted (CRC fixed)",
           good_png[:off + 8] + bytes(idat) + pchunk(b"IEND", b""), False)
    # header claims absurd length (planted 2^64-1 header)
    bogus = b"\xff" * 8 + payload("rnd", 500)
    bogus += b"\x00" * (20 * 13 * 3 - len(bogus))      # pad to full image
    expect("length header 2^64-1",
           build_png(20, 13, scanlines(bogus, 20, 13)), False)
    expect("empty file", b"", False)
    expect("signature only", b"\x89PNG\r\n\x1a\n", False)
    ihdr0 = struct.pack(">IIBBBBB", 0, 4, 8, 2, 0, 0, 0)
    expect("zero width", b"\x89PNG\r\n\x1a\n" + pchunk(b"IHDR", ihdr0) + pchunk(b"IEND", b""), False)
    check(run([C_BIN, "decode", d, d + "/x"]).returncode == 1, "directory as input")
    check(run([C_BIN, "encode", "/nonexistent", d + "/x"]).returncode == 1, "missing input file")

    # tolerated structures
    data2 = payload("rnd", 900)
    anc = (pchunk(b"gAMA", struct.pack(">I", 45455))
           + pchunk(b"pHYs", struct.pack(">IIB", 2835, 2835, 1)))
    after = pchunk(b"tEXt", b"Comment\x00hello") + pchunk(b"tIME", b"\x00" * 7)
    zero_idat = pchunk(b"IDAT", b"")
    expect("ancillary chunks + zero-len IDATs",
           planted_png(data2, 32, 10, extra_chunks=anc) + zero_idat + after, True, data2)
    expect("trailing garbage after IEND",
           planted_png(data2, 32, 10) + b"GARBAGE" * 20, True, data2)

    # extreme geometries with planted payloads
    d1 = payload("rnd", 99)
    expect("1xN image", planted_png(d1, 1, 36), True, d1)
    expect("Nx1 image", planted_png(d1, 36, 1), True, d1)
    d3 = payload("rnd", 3000)
    expect("all filter types, planted",
           planted_png(d3, 60, 17, filters=[0, 1, 2, 3, 4]), True, d3)

    # many small IDAT chunks (re-chunk a 1 MB stream)
    big = payload("rnd", 1_000_000)
    f_in = os.path.join(d, "rc.bin"); open(f_in, "wb").write(big)
    assert c_encode(f_in, os.path.join(d, "rc.png")).returncode == 0
    raw = open(os.path.join(d, "rc.png"), "rb").read()
    off = 8; idat = b""; head = b""; iend = b""
    while off < len(raw):
        cl = struct.unpack(">I", raw[off:off + 4])[0]
        typ = raw[off + 4:off + 8]
        if typ == b"IDAT":
            idat += raw[off + 8:off + 8 + cl]
        elif typ == b"IEND":
            iend = raw[off:off + 12 + cl]
        else:
            head += raw[off:off + 12 + cl]
        off += 12 + cl
    re = b"\x89PNG\r\n\x1a\n" + head          # signature + IHDR
    for i in range(0, len(idat), 100):
        re += pchunk(b"IDAT", idat[i:i + 100])
    re += iend
    expect("IDAT re-chunked into 100-byte pieces", re, True, big)

    # awkward paths
    weird = os.path.join(d, "héllo wörld + spaces.bin")
    open(weird, "wb").write(data)
    w_png = weird + ".png"
    check(c_encode(weird, w_png).returncode == 0 and
          c_decode(w_png, weird + ".out").returncode == 0 and
          open(weird + ".out", "rb").read() == data, "unicode/space filenames")

    ro = os.path.join(d, "ro"); os.makedirs(ro, exist_ok=True); os.chmod(ro, 0o555)
    if os.geteuid() != 0:
        check(c_encode(weird, os.path.join(ro, "x.png")).returncode == 1,
              "read-only output dir")
    os.chmod(ro, 0o755)
    print(f"    done in {time.time()-t0:.1f}s")


def phase_fuzz():
    print("[4] fuzzing under sanitizers (must never crash)")
    t0 = time.time()
    if not os.path.exists(ASAN_BIN):
        r = run(["gcc", "-g", "-O1", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all",
                 "-o", ASAN_BIN, os.path.join(BASE, "photo.c"), "-lm"])
        if r.returncode != 0:
            print(r.stderr); sys.exit(1)
    # ASan needs to mmap a huge shadow region; some arm64 kernels (high
    # ASLR, no vm.mmap_rnd_bits knob) never allow it. Fall back to UBSan.
    global SAN_LABEL
    probe = os.path.join(WORK, "asan_probe.png")
    os.makedirs(WORK, exist_ok=True)
    pr = run([ASAN_BIN, "encode", os.path.join(BASE, "photo.py"), probe], timeout=60)
    pr2 = run(["setarch", os.uname().machine, "-R", ASAN_BIN, "encode",
               os.path.join(BASE, "photo.py"), probe], timeout=60)
    if pr.returncode != 0 and pr2.returncode != 0:
        print("    ASan unusable on this kernel; falling back to UBSan-only")
        r = run(["gcc", "-g", "-O1", "-fsanitize=undefined",
                 "-fno-sanitize-recover=all",
                 "-o", ASAN_BIN, os.path.join(BASE, "photo.c"), "-lm"])
        if r.returncode != 0:
            print(r.stderr); sys.exit(1)
        SAN_LABEL = "UBSan"
    else:
        SAN_LABEL = "ASan+UBSan"
    d = os.path.join(WORK, "fuzz")
    os.makedirs(d, exist_ok=True)

    seed_data = payload("rnd", 8000)
    f_in = os.path.join(d, "seed.bin"); open(f_in, "wb").write(seed_data)
    assert run([ASAN_BIN, "encode", f_in, os.path.join(d, "seed.png")]).returncode == 0
    seed = open(os.path.join(d, "seed.png"), "rb").read()
    # also a compressed real-image PNG for deeper inflate paths
    mix_src = os.path.join(d, "mix_src.png")
    rng = np.random.default_rng(11)
    g = np.zeros((120, 160, 3), np.uint8)
    yy, xx = np.mgrid[0:120, 0:160]
    g[:, :, 0] = (yy * 2).astype(np.uint8)
    g[:, :, 1] = (xx).astype(np.uint8)
    g[:, :, 2] = rng.integers(0, 256, (120, 160), dtype=np.uint8)
    Image.fromarray(g).save(mix_src)
    mix = open(mix_src, "rb").read()

    def asan_decode(png_bytes, label):
        p = os.path.join(d, "fz.png")
        open(p, "wb").write(png_bytes)
        try:
            r = run([ASAN_BIN, "decode", p, os.path.join(d, "fz.out")], timeout=30)
        except subprocess.TimeoutExpired:
            check(False, label, "TIMEOUT")
            return
        check(r.returncode in (0, 1), label,
              f"rc={r.returncode} {r.stderr.strip()[:100]}")

    rng = np.random.default_rng(1234)
    n_dumb, n_smart, n_trunc, n_garbage = 120, 120, 60, 40

    def idat_span(png):
        off = 8
        while off + 8 <= len(png):
            cl = struct.unpack(">I", png[off:off + 4])[0]
            if png[off + 4:off + 8] == b"IDAT":
                return off, cl
            off += 12 + cl
        return None

    print(f"    fuzzing {n_dumb + n_smart + n_trunc + n_garbage} mutants under {SAN_LABEL}")
    for i in range(n_dumb):
        b = bytearray(seed)
        for _ in range(rng.integers(1, 4)):
            b[rng.integers(0, len(b))] ^= 1 << rng.integers(0, 8)
        asan_decode(bytes(b), f"dumb flip #{i}")

    for i in range(n_smart):
        base = mix if i % 2 else seed
        b = bytearray(base)
        span = idat_span(b)
        if span is None:
            continue
        off, cl = span
        mode = rng.integers(0, 3)
        if mode == 0:      # flip bytes inside zlib payload, fix chunk CRC
            for _ in range(rng.integers(1, 8)):
                b[off + 8 + rng.integers(0, cl)] ^= 1 << rng.integers(0, 8)
        elif mode == 1:    # truncate zlib payload
            newlen = max(2, int(cl * rng.uniform(0.3, 0.99)))
            b = b[:off + 8 + newlen] + b[off + 8 + cl:]
            b[off:off + 4] = struct.pack(">I", newlen)
        else:              # random splice of foreign bytes
            k = rng.integers(1, 30)
            pos = off + 8 + rng.integers(0, max(1, cl - k))
            b[pos:pos + k] = bytes(rng.integers(0, 256, k))
            if rng.integers(0, 2):
                b = b[:off + 8 + cl] + b[off + 8 + cl:]  # keep len, just corrupt
        # fix IDAT chunk CRC so corruption reaches the inflater
        end = off + 8 + struct.unpack(">I", b[off:off + 4])[0]
        crc = zlib.crc32(bytes(b[off + 4:end])) & 0xFFFFFFFF
        b[end:end + 4] = struct.pack(">I", crc)
        asan_decode(bytes(b), f"smart IDAT #{i}")

    for i in range(n_trunc):
        asan_decode(seed[: rng.integers(1, len(seed))], f"truncation #{i}")

    for i in range(n_garbage):
        asan_decode(b"\x89PNG\r\n\x1a\n" + bytes(rng.integers(0, 256, rng.integers(10, 8000))),
                    f"garbage+sig #{i}")

    total = n_dumb + n_smart + n_trunc + n_garbage
    print(f"    {total} fuzz cases in {time.time()-t0:.1f}s")


def phase_parallel():
    print("[5] parallel load (8 workers)")
    t0 = time.time()
    d = os.path.join(WORK, "par")
    os.makedirs(d, exist_ok=True)
    jobs = []
    for i in range(24):
        data = payload(["rnd", "text", "patt"][i % 3], 400_000 + i * 11111)
        jobs.append((f"p{i}", data))

    def job(arg):
        tag, data = arg
        f_in = os.path.join(d, f"{tag}.bin"); f_png = f_in + ".png"; f_out = f_in + ".out"
        open(f_in, "wb").write(data)
        r1 = c_encode(f_in, f_png)
        r2 = c_decode(f_png, f_out)
        got = open(f_out, "rb").read() if r2.returncode == 0 else b""
        ok = r1.returncode == 0 and r2.returncode == 0 and got == data
        for p in (f_in, f_png, f_out):
            if os.path.exists(p):
                os.remove(p)
        return tag, ok

    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for tag, ok in ex.map(job, jobs):
            check(ok, f"parallel {tag}")
    shutil.rmtree(d)
    print(f"    done in {time.time()-t0:.1f}s")


def phase_scale():
    print("[6] scale: 400 MB payload")
    t0 = time.time()
    d = os.path.join(WORK, "scale")
    os.makedirs(d, exist_ok=True)
    data = payload("rnd", 400_000_000)
    f_in = os.path.join(d, "big.bin"); open(f_in, "wb").write(data)
    r = c_encode(f_in, os.path.join(d, "big.png"), timeout=600)
    check(r.returncode == 0, "400MB encode", r.stderr[:80])
    print(f"    encode {time.time()-t0:.1f}s")
    r = c_decode(os.path.join(d, "big.png"), os.path.join(d, "big.out"), timeout=600)
    check(r.returncode == 0 and open(os.path.join(d, "big.out"), "rb").read() == data,
          "400MB round-trip")
    os.remove(os.path.join(d, "big.out"))
    print(f"    encode+round-trip {time.time()-t0:.1f}s")

    print("[6b] exact payload limit boundary (MAX_BYTES and MAX_BYTES+1)")
    exact = MAX_BYTES
    f_in = os.path.join(d, "exact.bin")
    with open(f_in, "wb") as fh:
        fh.write(payload("rnd", 1 << 20))
        remaining = exact - (1 << 20)
        block = b"\xA5" * (1 << 20)
        while remaining > 0:
            n = min(remaining, 1 << 20)
            fh.write(block[:n]); remaining -= n
    r = c_encode(f_in, os.path.join(d, "exact.png"), timeout=600)
    check(r.returncode == 0, "encode at exactly MAX_BYTES", r.stderr[:80])
    if r.returncode == 0:
        r2 = c_decode(os.path.join(d, "exact.png"), os.path.join(d, "exact.out"), timeout=600)
        same = r2.returncode == 0 and os.path.getsize(os.path.join(d, "exact.out")) == exact
        check(same, "decode at exactly MAX_BYTES")
        os.remove(os.path.join(d, "exact.out"))
        os.remove(os.path.join(d, "exact.png"))
    os.remove(f_in)

    over = os.path.join(d, "over.bin")
    with open(over, "wb") as fh:
        fh.seek(exact)          # sparse; content irrelevant, only size matters
        fh.write(b"\0")
    r = c_encode(over, os.path.join(d, "over.png"), timeout=600)
    check(r.returncode == 1, "C rejects MAX_BYTES+1", r.stdout[:60])
    r = py_encode(over, os.path.join(d, "over.png"), timeout=600)
    check(r.returncode != 0, "Python rejects MAX_BYTES+1")
    os.remove(over)

    print("[6c] cross-check with Python at the exact limit")
    f_in = os.path.join(d, "exact2.bin")
    with open(f_in, "wb") as fh:
        fh.write(payload("patt", 1 << 20))
        remaining = exact - (1 << 20)
        block = bytes((i * 31) % 256 for i in range(1 << 12)) * 256
        while remaining > 0:
            n = min(remaining, len(block))
            fh.write(block[:n]); remaining -= n
    t1 = time.time()
    r = py_encode(f_in, os.path.join(d, "exact_py.png"), timeout=1200)
    check(r.returncode == 0, "Python encodes exact-limit payload", r.stderr[:80])
    if r.returncode == 0:
        r = c_decode(os.path.join(d, "exact_py.png"), os.path.join(d, "exact_py.out"), timeout=600)
        ok = r.returncode == 0 and os.path.getsize(os.path.join(d, "exact_py.out")) == exact
        check(ok, "C decodes Python's exact-limit PNG")
        if ok:
            os.remove(os.path.join(d, "exact_py.out"))
    print(f"    python cross-check {time.time()-t1:.1f}s")
    # python decode of a C exact-limit png
    r = c_encode(f_in, os.path.join(d, "exact2.png"), timeout=600)
    if r.returncode == 0:
        r = py_decode(os.path.join(d, "exact2.png"), os.path.join(d, "exact2.out"), timeout=1200)
        check(r.returncode == 0 and os.path.getsize(os.path.join(d, "exact2.out")) == exact,
              "Python decodes C's exact-limit PNG")
        if os.path.exists(os.path.join(d, "exact2.out")):
            os.remove(os.path.join(d, "exact2.out"))
        os.remove(os.path.join(d, "exact2.png"))
    os.remove(f_in)
    shutil.rmtree(d)
    print(f"    scale phase total {time.time()-t0:.1f}s")


# --------------------------------------------------------------------------

def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "main"
    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(C_BIN):
        r = run(["gcc", "-O2", "-o", C_BIN, os.path.join(BASE, "photo.c"), "-lm"])
        if r.returncode != 0:
            print(r.stderr); sys.exit(1)
    t0 = time.time()
    if which in ("main", "all"):
        phase_combinatorial()
        phase_real_files()
        phase_adversarial()
        phase_fuzz()
        phase_parallel()
    if which in ("scale", "all"):
        phase_scale()
    print(f"\n{'='*60}")
    print(f"RESULT: {PASS} passed, {FAIL} failed  ({time.time()-t0:.1f}s total)")
    if FAILURES:
        print("Failures:")
        for f in FAILURES:
            print("  -", f)
    shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
