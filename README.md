# photo

Embed any file inside a lossless PNG — and recover it byte-for-byte later.

`photo` packs the raw bytes of any file into the RGB pixel values of a
perfectly normal, spec-valid PNG, with an 8-byte header recording the
original file length. Decoding reverses the process exactly: the output is
bit-identical to the input, verified by SHA-256.

It exists to sneak arbitrary files through services that only accept images
(such as Google Photos in "original quality" mode, which stores PNGs
byte-for-byte) while keeping the payload trivially recoverable.

## Why

- **One static binary, zero dependencies.** PNG encode/decode, DEFLATE
  (RFC 1950/1951), CRC-32, Adler-32 and SHA-256 are all implemented in a
  single ~1,200-line C file. No libpng, no zlib, nothing to install.
- **Real PNGs.** The output opens in any image viewer, browser or photo
  library. To the naked eye it is a black (or noisy) square; the data lives
  in the pixel values.
- **Exact restore.** Every encode prints a SHA-256; decode prints it again
  so you can confirm the round trip. Tested byte-exact from 0 bytes to
  400 MB, including cross-checks against the original Python/Pillow
  implementation.
- **Fast.** ~0.2 s to embed a 16 MB file; a 400 MB file round-trips in
  about 10 seconds.

## Build

```sh
gcc -O2 -o photo photo.c -lm
```

That's it. Only a C compiler and libm are required.

## Usage

```sh
./photo encode <input_file> <output.png>   # file  -> PNG
./photo decode <input.png> <output_file>   # PNG   -> file
```

Example:

```sh
$ ./photo encode demo.tar demo.png
[encode] demo.tar  (0.38 MB)
         -> demo.png  (366x365 px, 0.38 MB on disk)
         SHA-256: 2f2f1d43d1271c3031869863ea44d2a05e39cde0fdd24f97bfc32beb0079f2a3

$ ./photo decode demo.png restored.tar
[decode] demo.png
         -> restored.tar  (0.38 MB)
         SHA-256: 2f2f1d43d1271c3031869863ea44d2a05e39cde0fdd24f97bfc32beb0079f2a3

$ cmp demo.tar restored.tar && echo identical
identical
```

The PNG is only marginally larger than the input (pixel data is stored
uncompressed), and it opens as a perfectly normal image — a black or noisy
square of the nearest-square dimensions that fit the payload. Decode ignores
trailing padding pixels, so the original is recovered exactly.

## Limits

Google Photos caps images at **200 megapixels**. At 3 bytes per pixel that
allows **599,999,992 bytes (~600 MB)** of payload per PNG — the tool refuses
anything larger. To store a bigger file, split it first and encode each
part:

```sh
split -b 500m -d bigfile.bin part_                 # part_00, part_01, ...
for p in part_*; do ./photo encode "$p" "pngs/$p.png"; done

# restore
for p in pngs/part_*.png; do ./photo decode "$p" "$p.out"; done
cat pngs/part_*.out > bigfile.bin
```

Name parts with fixed-width numbers so lexicographic order matches the
original. Every PNG is independently decodable; each stores its own
chunk's length in its header.

Note: the ~600 MB figure exists for Google Photos' benefit. If the PNGs
never leave your own filesystem, you can raise `MAX_PIXELS` in `photo.c`
and rebuild.

## How it works

**Encode.** The payload is an 8-byte big-endian length header followed by
the file bytes, zero-padded to fill an RGB image. Rows are filtered with
the same adaptive heuristic Pillow uses (None → Up → Sub → Paeth, scored by
signed distance from zero), and the filtered scanlines are written as
uncompressed stored blocks inside a valid zlib stream — so viewers decode
it instantly and re-saving recompresses it well.

**Decode.** A full PNG reader inflates the IDAT stream (stored, fixed and
dynamic Huffman blocks), reverses all five PNG row filters, and converts
grayscale, gray+alpha, RGB and RGBA images to RGB the way Pillow's
`convert("RGB")` does. The length header then says exactly how many bytes
to keep.

## Test suite

`stress_test.py` needs Python 3 with Pillow/numpy (the tool itself does
not) and covers ~850 checks:

- combinatorial size × content round-trips (0 B … 1 MB), C ↔ Python both ways
- real archives, media files and binaries
- adversarial PNGs: truncation, bad CRCs, bogus headers, all filter types
- ASan/UBSan fuzzing of mutated PNGs
- parallel load
- scale: 400 MB payload and the exact 599,999,992-byte boundary

```sh
python3 stress_test.py main    # ~20 s
python3 stress_test.py scale   # ~60 s, needs disk space
python3 stress_test.py all
```

## Android app

The repo also contains a standalone Android app (`com.lukag.fkggl`) that
wraps the same C code through JNI — no shelling out, no Python:

- **Encode:** pick any file, pick an output folder, optionally set the part
  size (default **500 MB**). The file is split into `<name>.partNNN.png`
  parts, each embedded in its own PNG. Per-part status and SHA-256 prefixes
  are shown as they complete.
- **Decode:** pick the PNG parts (any order — they are sorted by name),
  pick where to save, and they are decoded and merged back into the
  original file, with per-part SHA-256s displayed for verification.

The native library is prebuilt outside Gradle so the build works on any
host architecture (the NDK's prebuilt toolchain binaries are x86_64-only):

```sh
scripts/build_native.sh          # builds libphoto_jni.so for arm64 + x86_64
./gradlew assembleDebug          # packages app/build/outputs/apk/debug/
```

`photo.c` at the repo root is the single source of truth; the build script
copies it into `app/src/main/cpp/`. Parts flow through file descriptors
end-to-end (`photo_encode_fd` / `photo_decode_fd`), so even 500 MB parts
never materialize as Java byte arrays.

## Repository layout

```
photo.c               the entire C program: CLI + library (one file)
photo.h               public API of the library build
photo.py              original Python/Pillow reference implementation
stress_test.py        regression / stress suite (~850 checks)
tests/test_fd_api.c   native tests for the fd/buffer APIs
scripts/build_native.sh  builds the Android JNI libraries
app/                  Android app (Kotlin + JNI)
Makefile              build + run tests
```

## Building elsewhere

Any C99 compiler works. On Windows, MSYS2/MinGW or WSL are the easiest
routes:

```sh
cc -O2 -o photo photo.c -lm
```

## License

Apache-2.0 — see [LICENSE](LICENSE).
