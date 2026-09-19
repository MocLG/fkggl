/*
 * photo — embed any file into a lossless PNG and recover it byte-for-byte.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Luka Gejak
 *
 * File bytes are packed into RGB pixels of a valid PNG, preceded by an
 * 8-byte header holding the original length. PNG encode/decode, DEFLATE,
 * CRC-32, Adler-32 and SHA-256 are implemented here; no dependencies.
 *
 * Build as a CLI program:   gcc -O2 -o photo photo.c -lm
 * Build as a library:       cc -O2 -DPHOTO_STATIC -c photo.c
 * (PHOTO_STATIC exposes photo_encode_buf / photo_decode_buf /
 * photo_last_error and drops the file-based command layer.)
 *
 * Max payload: 599,999,992 bytes (Google Photos caps images at 200 MP;
 * at 3 bytes/pixel that is ~600 MB). Larger files must be split first.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER_SIZE  8ULL                        /* big-endian uint64 length header */
#define MAX_PIXELS   200000000ULL                /* Google Photos 200 MP limit */
#define MAX_BYTES    (MAX_PIXELS * 3 - HEADER_SIZE)
#define IDAT_CHUNK_CAP 65536u                    /* max bytes per IDAT chunk */
#define MAX_DECOMP   ((uint64_t)1 << 33)         /* 8 GiB decompression sanity cap */

/* --- errors --- */

static char g_error[512];

const char *photo_last_error(void)
{
    return g_error[0] ? g_error : "unknown error";
}

static void set_error_v(const char *fmt, va_list ap)
{
    vsnprintf(g_error, sizeof g_error, fmt, ap);
}

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), unused));
static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_error_v(fmt, ap);
    va_end(ap);
}

#define FAIL(...) do { set_error(__VA_ARGS__); return -1; } while (0)

#ifdef PHOTO_STATIC
#define DIE(...) do { set_error(__VA_ARGS__); return NULL; } while (0)
#else
static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_error_v(fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s\n", g_error);
    exit(1);
}

#define DIE(...) fatal(__VA_ARGS__)
#endif

/* --- byte order --- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static uint64_t get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void to_hex(const uint8_t d[32], char out[65])
{
    static const char hd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hd[d[i] >> 4];
        out[2 * i + 1] = hd[d[i] & 15];
    }
    out[64] = '\0';
}

/* --- CRC-32 (PNG chunk checksums) --- */

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_begin(void)
{
    if (!crc_ready)
        crc_init();
    return 0xFFFFFFFFu;
}

static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static uint32_t crc32_end(uint32_t c)
{
    return c ^ 0xFFFFFFFFu;
}

/* --- Adler-32 (zlib checksum) --- */

#define ADLER_MOD 65521u

static uint32_t adler32(const uint8_t *p, uint64_t n)
{
    uint32_t a = 1, b = 0;
    while (n > 0) {
        uint64_t chunk = n > 5552 ? 5552 : n; /* 5552 keeps b below 2^32 */
        for (uint64_t i = 0; i < chunk; i++) {
            a += p[i];
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

/* --- SHA-256 --- */

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256(const uint8_t *data, uint64_t len, uint8_t digest[32])
{
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint64_t total = len;
    const uint8_t *p = data;

    uint8_t block[64];
    while (len >= 64) {
        memcpy(block, p, 64);
        p += 64;
        len -= 64;

        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
                   ((uint32_t)block[4 * i + 2] << 8) |  (uint32_t)block[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + sha_k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    /* last block(s) with padding */
    uint8_t last[128];
    uint64_t rem = len;
    memcpy(last, p, rem);
    last[rem++] = 0x80;
    size_t padblocks = (rem + 8 > 64) ? 2 : 1;
    memset(last + rem, 0, padblocks * 64 - rem);
    uint64_t bits = total * 8;
    put_be64(last + padblocks * 64 - 8, bits);

    for (size_t pb = 0; pb < padblocks; pb++) {
        memcpy(block, last + pb * 64, 64);
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
                   ((uint32_t)block[4 * i + 2] << 8) |  (uint32_t)block[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + sha_k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    for (int i = 0; i < 8; i++)
        put_be32(digest + 4 * i, h[i]);
}

/* --- core: buffer-based embed/extract (no file I/O; used by the JNI layer) --- */

static uint8_t *png_to_memory(const uint8_t *pixels, uint32_t w, uint32_t h,
                              uint64_t *out_len);
static uint8_t *png_decode_mem(const uint8_t *buf, uint64_t fsize, const char *name,
                               uint32_t *w_out, uint32_t *h_out);

/* Encode len bytes into a malloc'd PNG image. Returns NULL on error (see
 * photo_last_error()); on success stores the buffer size in *png_len_out
 * and the image geometry in the w_out and h_out outputs. */
uint8_t *photo_encode_buf(const uint8_t *data, uint64_t len,
                          uint64_t *png_len_out, uint32_t *w_out, uint32_t *h_out)
{
    if (!png_len_out || !w_out || !h_out)
        DIE("encode: null output pointer");
    if (!data && len)
        DIE("encode: null input");
    if (len > MAX_BYTES)
        DIE("encode: %.1f MB exceeds the %.0f MB payload limit; split first",
            (double)len / 1e6, (double)MAX_BYTES / 1e6);

    uint64_t payload_len = HEADER_SIZE + len;
    uint64_t n_pixels = (payload_len + 2) / 3;
    uint64_t width = (uint64_t)ceil(sqrt((double)n_pixels));
    if (width < 1)
        width = 1;
    uint64_t height = (n_pixels + width - 1) / width;
    if (width * height < n_pixels) { /* float-rounding safety net */
        width++;
        height = (n_pixels + width - 1) / width;
    }

    uint8_t *pixels = (uint8_t *)calloc(1, (size_t)(width * height * 3));
    if (!pixels)
        DIE("encode: out of memory");
    put_be64(pixels, len);
    if (len)
        memcpy(pixels + HEADER_SIZE, data, (size_t)len);

    uint64_t png_len = 0;
    uint8_t *png = png_to_memory(pixels, (uint32_t)width, (uint32_t)height, &png_len);
    free(pixels);
    if (!png)
        return NULL;
    *png_len_out = png_len;
    *w_out = (uint32_t)width;
    *h_out = (uint32_t)height;
    return png;
}

/* Extract the embedded file from a PNG buffer. Returns a malloc'd buffer
 * and its size in *len_out, or NULL on error (see photo_last_error()). */
uint8_t *photo_decode_buf(const uint8_t *png, uint64_t png_len, uint64_t *len_out)
{
    if (!png || png_len < 8 || !len_out)
        DIE("decode: not a PNG buffer");

    uint32_t w, h;
    uint8_t *pixels = png_decode_mem(png, png_len, "input", &w, &h);
    if (!pixels)
        return NULL;

    uint64_t raw_len = (uint64_t)w * h * 3;
    if (raw_len < HEADER_SIZE) {
        free(pixels);
        DIE("decode: image too small to contain the length header");
    }
    uint64_t file_len = get_be64(pixels);
    if (file_len > raw_len - HEADER_SIZE) {
        free(pixels);
        DIE("decode: header claims %llu bytes but image only holds %llu",
            (unsigned long long)file_len,
            (unsigned long long)(raw_len - HEADER_SIZE));
    }

    uint8_t *out = (uint8_t *)malloc(file_len ? (size_t)file_len : 1);
    if (!out) {
        free(pixels);
        DIE("decode: out of memory");
    }
    if (file_len)
        memcpy(out, pixels + HEADER_SIZE, (size_t)file_len);
    free(pixels);
    *len_out = file_len;
    return out;
}

unsigned long long photo_max_bytes(void)
{
    return (unsigned long long)MAX_BYTES;
}

/* --- streaming file API (fd-based; used by the Android JNI layer) ---
 *
 * Same pipeline as the buffer API but input/output flow through file
 * descriptors, so large parts never exist as Java byte arrays. The input
 * fd stays open across calls: each encode/decode consumes exactly one
 * part sequentially (fd is dup'ed internally, not closed).
 */

/* Like photo_encode_buf, but reads len bytes from in_fd at its current
 * position and writes the PNG to out_fd. Stores the payload SHA-256 hex
 * in sha_hex (65 bytes). Returns 0 or -1. */
int photo_encode_fd(int in_fd, uint64_t len, int out_fd, char sha_hex[65])
{
    if (len > MAX_BYTES)
        FAIL("encode: %.1f MB exceeds the %.0f MB payload limit; split first",
             (double)len / 1e6, (double)MAX_BYTES / 1e6);

    uint64_t payload_len = HEADER_SIZE + len;
    uint64_t n_pixels = (payload_len + 2) / 3;
    uint64_t width = (uint64_t)ceil(sqrt((double)n_pixels));
    if (width < 1)
        width = 1;
    uint64_t height = (n_pixels + width - 1) / width;
    if (width * height < n_pixels) {
        width++;
        height = (n_pixels + width - 1) / width;
    }
    uint64_t total = width * height * 3;

    uint8_t *pixels = (uint8_t *)calloc(1, (size_t)total);
    if (!pixels)
        FAIL("encode: out of memory");
    put_be64(pixels, len);

    if (len) {
        FILE *in = fdopen(dup(in_fd), "rb");
        if (!in) {
            free(pixels);
            FAIL("encode: cannot open input fd");
        }
        size_t r = fread(pixels + HEADER_SIZE, 1, (size_t)len, in);
        int err = ferror(in);
        fclose(in);
        if (r != (size_t)len || err) {
            free(pixels);
            FAIL("encode: short read from input (%llu of %llu bytes)",
                 (unsigned long long)r, (unsigned long long)len);
        }
    }

    {
        uint8_t digest[32];
        sha256(pixels + HEADER_SIZE, len, digest);
        to_hex(digest, sha_hex);
    }

    uint64_t png_len = 0;
    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    uint8_t *png = png_to_memory(pixels, w, h, &png_len);
    free(pixels);
    if (!png)
        return -1;

    FILE *out = fdopen(dup(out_fd), "wb");
    if (!out) {
        free(png);
        FAIL("encode: cannot open output fd");
    }
    size_t w_total = fwrite(png, 1, (size_t)png_len, out);
    int werr = ferror(out) || fflush(out) != 0;
    fclose(out);
    free(png);
    if (w_total != (size_t)png_len || werr)
        FAIL("encode: short write to output");
    return 0;
}

/* Reads a whole PNG from in_fd, extracts the payload and writes it to
 * out_fd. Stores the payload SHA-256 hex in sha_hex and the payload size
 * in *len_out. Returns 0 or -1. */
int photo_decode_fd(int in_fd, int out_fd, char sha_hex[65], uint64_t *len_out)
{
    FILE *in = fdopen(dup(in_fd), "rb");
    if (!in)
        FAIL("decode: cannot open input fd");

    uint64_t cap = 1u << 20;
    uint64_t png_len = 0;
    uint8_t *png = (uint8_t *)malloc((size_t)cap);
    if (!png) {
        fclose(in);
        FAIL("decode: out of memory");
    }
    for (;;) {
        if (png_len == cap) {
            cap *= 2;
            if (cap > MAX_DECOMP) {
                free(png); fclose(in);
                FAIL("decode: PNG too large");
            }
            uint8_t *np = (uint8_t *)realloc(png, (size_t)cap);
            if (!np) {
                free(png); fclose(in);
                FAIL("decode: out of memory");
            }
            png = np;
        }
        size_t r = fread(png + png_len, 1, (size_t)(cap - png_len), in);
        png_len += r;
        if (r == 0) {
            if (ferror(in)) {
                free(png); fclose(in);
                FAIL("decode: error reading input");
            }
            break;
        }
    }
    fclose(in);

    uint32_t w, h;
    uint8_t *pixels = png_decode_mem(png, png_len, "input", &w, &h);
    free(png);
    if (!pixels)
        return -1;

    uint64_t raw_len = (uint64_t)w * h * 3;
    if (raw_len < HEADER_SIZE) {
        free(pixels);
        FAIL("decode: image too small to contain the length header");
    }
    uint64_t file_len = get_be64(pixels);
    if (file_len > raw_len - HEADER_SIZE) {
        free(pixels);
        FAIL("decode: header claims %llu bytes but image only holds %llu",
             (unsigned long long)file_len,
             (unsigned long long)(raw_len - HEADER_SIZE));
    }

    FILE *out = fdopen(dup(out_fd), "wb");
    if (!out) {
        free(pixels);
        FAIL("decode: cannot open output fd");
    }
    size_t w_total = file_len ? fwrite(pixels + HEADER_SIZE, 1, (size_t)file_len, out) : 1;
    int werr = ferror(out) || (file_len && fflush(out) != 0);
    fclose(out);

    uint8_t digest[32];
    sha256(pixels + HEADER_SIZE, file_len, digest);
    to_hex(digest, sha_hex);
    free(pixels);

    if (!w_total || werr)
        FAIL("decode: short write to output");
    *len_out = file_len;
    return 0;
}

/* --- file I/O (CLI only) --- */

#ifndef PHOTO_STATIC
static uint8_t *read_file(const char *path, uint64_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        DIE("cannot open '%s' for reading: %s", path, strerror(errno));
    size_t cap = 1u << 20;
    uint64_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        DIE("out of memory");
    for (;;) {
        if (len == (uint64_t)cap) {
            cap *= 2;
            if ((uint64_t)cap > MAX_DECOMP)
                DIE("'%s' is too large to read", path);
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb)
                DIE("out of memory");
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - (size_t)len, f);
        len += r;
        if (r == 0) {
            if (ferror(f))
                DIE("error reading '%s'", path);
            break;
        }
    }
    fclose(f);
    *size_out = len;
    return buf;
}

static void write_file(const char *path, const uint8_t *data, uint64_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        DIE("cannot open '%s' for writing: %s", path, strerror(errno));
    uint64_t off = 0;
    while (off < size) {
        size_t w = fwrite(data + off, 1, (size_t)(size - off), f);
        if (w == 0)
            DIE("error writing '%s'", path);
        off += w;
    }
    if (fclose(f) != 0)
        DIE("error closing '%s': %s", path, strerror(errno));
}
#endif /* PHOTO_STATIC */

/* --- PNG encoder --- */

static int write_chunk_mem(uint8_t *buf, uint64_t cap, uint64_t *off,
                           const char *type, const uint8_t *data, uint32_t len)
{
    if (*off + 12 + (uint64_t)len > cap)
        return -1;
    uint8_t hdr[8], tb[4];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    memcpy(buf + *off, hdr, 8);
    *off += 8;
    if (len) {
        memcpy(buf + *off, data, len);
        *off += len;
    }
    uint32_t c = crc32_begin();
    c = crc32_add(c, (const uint8_t *)type, 4);
    if (len)
        c = crc32_add(c, data, len);
    put_be32(tb, crc32_end(c));
    memcpy(buf + *off, tb, 4);
    *off += 4;
    return 0;
}

/* Pillow's ZipEncode.c scores each filter by the total signed distance of
 * the filtered bytes from zero. */
static unsigned dist8(uint8_t v)
{
    return v < 128 ? v : 256u - v;
}

/* Pick the row filter like Pillow's ZipEncode.c: try None, Up, Sub and
 * Paeth in that order, keeping a filter only when its score strictly
 * improves (Average is never tried without Pillow's "optimize" flag).
 * Writes the filtered row (filter byte + rowbytes) into dst; prev is the
 * previous row's raw pixels (all-zero for the first row). */
static void pillow_filter_row(uint8_t *dst, const uint8_t *cur, const uint8_t *prev,
                              uint8_t *up, uint8_t *sub, uint8_t *paeth,
                              uint64_t rowbytes, int bpp)
{
    /* None candidate: the raw bytes, copied into the destination */
    memcpy(dst + 1, cur, (size_t)rowbytes);
    uint8_t *best = dst + 1;
    int best_type = 0;
    unsigned best_sum = 0;
    for (uint64_t i = 0; i < rowbytes; i++)
        best_sum += dist8(cur[i]);

    if (best_sum > 0) {
        unsigned s = 0;
        for (uint64_t i = 0; i < rowbytes; i++) {
            uint8_t v = (uint8_t)(cur[i] - prev[i]);
            up[i] = v;
            s += dist8(v);
        }
        if (s < best_sum) {
            best = up;
            best_type = 2;
            best_sum = s;
        }
    }
    if (best_sum > 0) {
        unsigned s = 0;
        for (int i = 0; i < bpp; i++) {
            sub[i] = cur[i];
            s += dist8(cur[i]);
        }
        for (uint64_t i = bpp; i < rowbytes; i++) {
            uint8_t v = (uint8_t)(cur[i] - cur[i - bpp]);
            sub[i] = v;
            s += dist8(v);
        }
        if (s < best_sum) {
            best = sub;
            best_type = 1;
            best_sum = s;
        }
    }
    if (best_sum > 0) {
        unsigned s = 0;
        for (int i = 0; i < bpp; i++) {
            uint8_t v = (uint8_t)(cur[i] - prev[i]);
            paeth[i] = v;
            s += dist8(v);
        }
        for (uint64_t i = bpp; i < rowbytes; i++) {
            int a = cur[i - bpp], b = prev[i], c = prev[i - bpp];
            int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
            uint8_t v = (uint8_t)(cur[i] -
                          ((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c));
            paeth[i] = v;
            s += dist8(v);
        }
        if (s < best_sum) {
            best = paeth;
            best_type = 4;
        }
    }

    dst[0] = (uint8_t)best_type;
    if (best != dst + 1)
        memcpy(dst + 1, best, (size_t)rowbytes);
}

/* Wrap the filtered scanlines in a zlib stream (RFC 1950) with the framing
 * zlib's deflate_stored() uses at level 0: header 0x78 0x01, a stored block
 * whenever input reaches the 32768-byte window (rounded up to a row
 * boundary), a final stored block, then the Adler-32 trailer. */
static uint8_t *zlib_wrap_rows(const uint8_t *raw, uint64_t total,
                               uint64_t stride, uint64_t h, uint64_t *out_len)
{
    /* count blocks: threshold-triggered blocks + one final (maybe empty) */
    uint64_t acc = 0, nblocks = 1;
    for (uint64_t r = 0; r < h; r++) {
        acc += stride;
        if (acc >= 32768) {
            nblocks++;
            acc = 0;
        }
    }

    uint64_t cap = 2 + nblocks * 5 + total + 4;
    uint8_t *out = (uint8_t *)malloc((size_t)cap);
    if (!out)
        DIE("out of memory");

    uint64_t o = 0;
    out[o++] = 0x78;    /* CMF: deflate, 32K window */
    out[o++] = 0x01;    /* FLG: checksum, level 0 */

    acc = 0;
    uint64_t start = 0;
    for (uint64_t r = 0; r < h; r++) {
        acc += stride;
        if (acc >= 32768) {
            uint64_t len = acc;
            while (len > 65535) { /* stored LEN is 16-bit; split if ever larger */
                out[o++] = 0;
                out[o++] = 0xFF; out[o++] = 0xFF;
                out[o++] = 0x00; out[o++] = 0x00;
                memcpy(out + o, raw + start, 65535);
                o += 65535;
                start += 65535;
                len -= 65535;
            }
            out[o++] = 0;                            /* BFINAL=0, BTYPE=00 */
            out[o++] = (uint8_t)(len & 0xFF);        /* LEN, little-endian */
            out[o++] = (uint8_t)(len >> 8);
            out[o++] = (uint8_t)(~len & 0xFF);       /* NLEN = ~LEN */
            out[o++] = (uint8_t)((~len >> 8) & 0xFF);
            memcpy(out + o, raw + start, (size_t)len);
            o += len;
            start += len;
            acc = 0;
        }
    }
    {
        uint64_t len = total - start;
        out[o++] = 1;                                /* BFINAL=1, BTYPE=00 */
        out[o++] = (uint8_t)(len & 0xFF);
        out[o++] = (uint8_t)(len >> 8);
        out[o++] = (uint8_t)(~len & 0xFF);
        out[o++] = (uint8_t)((~len >> 8) & 0xFF);
        if (len) {
            memcpy(out + o, raw + start, (size_t)len);
            o += len;
        }
    }

    put_be32(out + o, adler32(raw, total));
    o += 4;
    *out_len = o;
    return out;
}static uint8_t *png_to_memory(const uint8_t *pixels, uint32_t w, uint32_t h,
                              uint64_t *out_len)
{
    uint64_t rowbytes = (uint64_t)w * 3;
    uint64_t stride = rowbytes + 1;              /* +1 filter byte per row */
    uint64_t raw_len = stride * h;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);

    uint8_t *fup = (uint8_t *)malloc((size_t)rowbytes);
    uint8_t *fsub = (uint8_t *)malloc((size_t)rowbytes);
    uint8_t *fpaeth = (uint8_t *)malloc((size_t)rowbytes);
    uint8_t *black = (uint8_t *)calloc((size_t)rowbytes, 1); /* ZipEncode seeds the previous row as black */
    if (!raw || !fup || !fsub || !fpaeth || !black)
        DIE("out of memory");
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *prev = y ? pixels + (uint64_t)(y - 1) * rowbytes : black;
        pillow_filter_row(raw + y * stride,
                          pixels + (uint64_t)y * rowbytes, prev,
                          fup, fsub, fpaeth, rowbytes, 3);
    }

    uint64_t zlen;
    uint8_t *zdata = zlib_wrap_rows(raw, raw_len, stride, h, &zlen);
    free(raw);
    free(fup);
    free(fsub);
    free(fpaeth);
    free(black);
    if (!zdata)
        return NULL;

    /* exact upper bound: sig + IHDR + one IDAT per 65536 z-bytes + IEND */
    uint64_t cap = 8 + (13 + 12) + (zlen / 65536 + 2) * 12 + zlen + 12;
    uint8_t *buf = (uint8_t *)malloc((size_t)cap);
    if (!buf) {
        free(zdata);
        DIE("out of memory");
    }

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(buf, sig, 8);
    uint64_t off = 8;

    uint8_t ihdr[13];
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;      /* bit depth */
    ihdr[9] = 2;      /* color type: truecolor RGB */
    ihdr[10] = 0;     /* compression: deflate */
    ihdr[11] = 0;     /* filter method */
    ihdr[12] = 0;     /* no interlace */
    int rc = write_chunk_mem(buf, cap, &off, "IHDR", ihdr, 13);

    /* IDAT chunks capped at 65536 bytes, like Pillow and libpng emit. */
    uint64_t zoff = 0;
    while (rc == 0 && zoff < zlen) {
        uint64_t n = zlen - zoff;
        if (n > 65536)
            n = 65536;
        rc = write_chunk_mem(buf, cap, &off, "IDAT", zdata + zoff, (uint32_t)n);
        zoff += n;
    }
    free(zdata);
    if (rc == 0)
        rc = write_chunk_mem(buf, cap, &off, "IEND", NULL, 0);
    if (rc != 0)
        DIE("png encode failed");

    *out_len = off;
    return buf;
}

/* --- DEFLATE decompressor (RFC 1950/1951) --- */

typedef struct {
    uint16_t counts[16];   /* number of codes of each length */
    uint16_t symbols[288]; /* symbols sorted by code */
} Huff;

typedef struct {
    /* input (current chunk) */
    const uint8_t *in;
    uint64_t       in_size;
    uint64_t       pos;
    /* bit accumulator */
    uint64_t       bitbuf;
    unsigned       nbits;
    /* output */
    uint8_t       *out;
    uint64_t       out_size;
    uint64_t       cap;
    uint64_t       expect;   /* 0 = unknown, else exact expected output size */
} ZlibStream;

/* Get `want` bits (LSB-first) from the stream; returns 0 if out of input. */
static int zb_get(ZlibStream *z, unsigned want, uint32_t *out)
{
    while (z->nbits < want) {
        if (z->pos >= z->in_size)
            return 0;
        z->bitbuf |= (uint64_t)z->in[z->pos++] << z->nbits;
        z->nbits += 8;
    }
    *out = (uint32_t)(z->bitbuf & (((uint64_t)1 << want) - 1));
    z->bitbuf >>= want;
    z->nbits -= want;
    return 1;
}

/* Decode one Huffman symbol (count-based, as in zlib's puff.c). */
static int32_t decode_sym(ZlibStream *z, const Huff *h)
{
    int32_t code = 0, first = 0, index = 0;
    for (int32_t len = 1; len <= 15; len++) {
        uint32_t bit;
        if (!zb_get(z, 1, &bit))
            return -1;
        code |= (int32_t)bit;
        int32_t count = h->counts[len];
        if (code - first < count)
            return (int32_t)h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* Build canonical Huffman decode tables; -1 on over-subscribed codes. */
static int canon_codes(const uint8_t *lens, int n, Huff *h)
{
    memset(h->counts, 0, sizeof h->counts);
    memset(h->symbols, 0, sizeof h->symbols);
    for (int i = 0; i < n; i++)
        h->counts[lens[i]]++;
    if (h->counts[0] == n)
        return 0;
    int32_t left = 1;
    for (int32_t len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0)
            return -1;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int32_t len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->counts[len]);
    for (int i = 0; i < n; i++)
        if (lens[i] != 0)
            h->symbols[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void fixed_tables(Huff *lit, Huff *dst)
{
    uint8_t lens[288];
    memset(lens, 0, sizeof lens);
    for (int i = 0;   i < 144; i++) lens[i] = 8;
    for (int i = 144; i < 256; i++) lens[i] = 9;
    for (int i = 256; i < 280; i++) lens[i] = 7;
    for (int i = 280; i < 288; i++) lens[i] = 8;
    canon_codes(lens, 288, lit);
    memset(lens, 0, 30);
    for (int i = 0; i < 30; i++) lens[i] = 5;
    canon_codes(lens, 30, dst);
}

static int zout_grow(ZlibStream *z, uint64_t need) /* -1 on limit/OOM */
{
    if (z->out_size + need <= z->cap)
        return 0;
    uint64_t ncap = z->cap ? z->cap * 2 : (1u << 16);
    while (ncap < z->out_size + need)
        ncap *= 2;
    if (ncap > MAX_DECOMP) {
        fprintf(stderr, "inflate: decompressed data too large\n");
        return -1;
    }
    uint8_t *np = (uint8_t *)realloc(z->out, (size_t)ncap);
    if (!np) {
        fprintf(stderr, "inflate: out of memory\n");
        return -1;
    }
    z->out = np;
    z->cap = ncap;
    return 0;
}

static int zout_byte(ZlibStream *z, uint8_t b)
{
    if (z->out_size >= z->cap && zout_grow(z, 1) != 0)
        return -1;
    z->out[z->out_size++] = b;
    return 0;
}

/* Stored (uncompressed) deflate block. */
static int inflate_stored(ZlibStream *z)
{
    unsigned rem = z->nbits & 7; /* pad to byte boundary */
    z->bitbuf >>= rem;
    z->nbits -= rem;

    uint32_t lo = 0, hi = 0;
    if (!zb_get(z, 16, &lo))
        return -1;
    if (!zb_get(z, 16, &hi))
        return -1;
    uint32_t len = lo;
    uint32_t nlen = hi;
    if ((len ^ 0xFFFFu) != nlen)
        return -1;
    if ((uint64_t)len > z->in_size - z->pos)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        if (zout_byte(z, z->in[z->pos++]) != 0)
            return -1;
    return 0;
}

/* One Huffman-coded block of literals and length/distance pairs. */
static int inflate_block(ZlibStream *z, const Huff *lit, const Huff *dst)
{
    for (;;) {
        int32_t sym = decode_sym(z, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (zout_byte(z, (uint8_t)sym) != 0)
                return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym > 28)
                return -1;
            uint32_t len = len_base[sym];
            unsigned eb = len_extra[sym];
            if (eb) {
                uint32_t x;
                if (!zb_get(z, eb, &x))
                    return -1;
                len += x;
            }
            int32_t dsym = decode_sym(z, dst);
            if (dsym < 0 || dsym > 29)
                return -1;
            uint32_t dist = dist_base[dsym];
            unsigned db = dist_extra[dsym];
            if (db) {
                uint32_t x;
                if (!zb_get(z, db, &x))
                    return -1;
                dist += x;
            }
            if ((uint64_t)dist > z->out_size)
                return -1;
            if (zout_grow(z, len) != 0)
                return -1;
            uint8_t *out = z->out;
            uint64_t o = z->out_size;
            for (uint32_t i = 0; i < len; i++)
                out[o + i] = out[o + i - dist];
            z->out_size = o + len;
        }
    }
}

/* Dynamic Huffman tables (code-length code first); -1 on bad input. */
static int parse_dynamic(ZlibStream *z, Huff *lit, Huff *dst)
{
    uint32_t v;
    if (!zb_get(z, 5, &v)) return -1;
    uint32_t hlit = v + 257;
    if (!zb_get(z, 5, &v)) return -1;
    uint32_t hdist = v + 1;
    if (!zb_get(z, 4, &v)) return -1;
    uint32_t hclen = v + 4;
    if (hlit > 286 || hdist > 30)
        return -1;

    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                    11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t cl_lens[19];
    memset(cl_lens, 0, sizeof cl_lens);
    for (uint32_t i = 0; i < hclen; i++) {
        if (!zb_get(z, 3, &v)) return -1;
        cl_lens[ord[i]] = (uint8_t)v;
    }
    Huff cl;
    if (canon_codes(cl_lens, 19, &cl) != 0)
        return -1;

    uint8_t lens[316];
    memset(lens, 0, sizeof lens);
    uint32_t n = hlit + hdist;
    uint32_t i = 0;
    while (i < n) {
        int32_t sym = decode_sym(z, &cl);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0)
                return -1;
            uint32_t r;
            if (!zb_get(z, 2, &r)) return -1;
            r += 3;
            if (i + r > n) return -1;
            uint8_t pv = lens[i - 1];
            while (r--) lens[i++] = pv;
        } else if (sym == 17) {
            uint32_t r;
            if (!zb_get(z, 3, &r)) return -1;
            r += 3;
            if (i + r > n) return -1;
            while (r--) lens[i++] = 0;
        } else {
            uint32_t r;
            if (!zb_get(z, 7, &r)) return -1;
            r += 11;
            if (i + r > n) return -1;
            while (r--) lens[i++] = 0;
        }
    }
    if (lens[256] == 0)
        return -1; /* no end-of-block code */
    if (canon_codes(lens, (int)hlit, lit) != 0)
        return -1;
    if (canon_codes(lens + hlit, (int)hdist, dst) != 0)
        return -1;
    return 0;
}

/* One-shot zlib decompressor. `expect` may be 0 (unknown) or the exact
 * expected output size. Returns a malloc'd buffer; dies on bad input. */
static uint8_t *mem_inflate(const uint8_t *in, uint64_t in_size,
                            uint64_t expect, uint64_t *out_size)
{
    ZlibStream z;
    memset(&z, 0, sizeof z);
    z.in = in;
    z.in_size = in_size;
    z.expect = expect;
    z.cap = expect ? expect : (1u << 16);
    if (z.cap > MAX_DECOMP)
        DIE("inflate: expected output size too large");
    z.out = (uint8_t *)malloc((size_t)z.cap);
    if (!z.out)
        DIE("inflate: out of memory");

    if (in_size < 2)
        DIE("inflate: truncated zlib stream");
    if ((in[0] & 0x0F) != 8)
        DIE("inflate: unsupported zlib compression method");
    if ((in[0] >> 4) > 7)
        DIE("inflate: unsupported zlib window size");
    if ((((uint32_t)in[0] << 8) | in[1]) % 31 != 0)
        DIE("inflate: bad zlib header");
    if (in[1] & 0x20)
        DIE("inflate: preset dictionary not supported");
    z.pos = 2;

    for (;;) {
        uint32_t v;
        if (!zb_get(&z, 3, &v))
            DIE("inflate: truncated zlib stream");
        int bfinal = v & 1;
        int btype = (int)((v >> 1) & 3);
        int rc;
        if (btype == 0) {
            rc = inflate_stored(&z);
        } else if (btype == 1) {
            Huff lit, dst;
            fixed_tables(&lit, &dst);
            rc = inflate_block(&z, &lit, &dst);
        } else if (btype == 2) {
            Huff lit, dst;
            rc = parse_dynamic(&z, &lit, &dst) != 0
                     ? -1 : inflate_block(&z, &lit, &dst);
        } else {
            rc = -1;
        }
        if (rc != 0)
            DIE("inflate: corrupt or truncated deflate stream");
        if (bfinal)
            break;
    }

    /* Adler-32 check if the trailer is already in the bit buffer */
    unsigned rem = z.nbits & 7;
    if (rem) {
        z.bitbuf >>= rem;
        z.nbits -= rem;
    }
    if (z.nbits >= 32) {
        uint32_t v = (uint32_t)z.bitbuf;
        uint32_t stored = ((v & 0xFFu) << 24) | ((v << 8) & 0x00FF0000u) |
                          ((v >> 8) & 0x0000FF00u) | (v >> 24);
        if (stored != adler32(z.out, z.out_size))
            fprintf(stderr, "warning: zlib checksum mismatch (data may be corrupt)\n");
    }
    if (z.expect && z.out_size != z.expect)
        DIE("inflate: decompressed size mismatch");
    *out_size = z.out_size;
    return z.out;
}

/* --- PNG decoder --- */

typedef struct {
    uint32_t  w, h;
    int       bpp;         /* bytes per pixel: 1, 2, 3 or 4 */
    int       img_ok;
    int       have_plte;
    /* concatenated raw IDAT stream */
    uint8_t  *zsrc;
    uint64_t  zsrc_size, zsrc_cap;
} PngParser;

static int ihdr_proc(PngParser *P, const uint8_t *d, uint32_t n)
{
    if (n != 13 || P->img_ok) /* IHDR must come first, exactly once */
        return -1;
    uint32_t w = rd32(d);
    uint32_t h = rd32(d + 4);
    uint8_t depth = d[8];
    uint8_t ctype = d[9];
    uint8_t interlace = d[12];
    if (w == 0 || h == 0)
        return -1;
    if ((uint64_t)w * h > MAX_PIXELS * 5) {
        fprintf(stderr, "decode_png: image is %ux%u (%.0f MP) — refusing absurd image\n",
                w, h, (double)w * h / 1e6);
        return -1;
    }
    if (depth != 8) {
        fprintf(stderr, "decode_png: bit depth %u not supported (need 8)\n", depth);
        return -1;
    }
    switch (ctype) {
        case 0: P->bpp = 1; break;   /* grayscale */
        case 2: P->bpp = 3; break;   /* RGB */
        case 4: P->bpp = 2; break;   /* gray + alpha */
        case 6: P->bpp = 4; break;   /* RGBA */
        default:
            fprintf(stderr, "decode_png: color type %u not supported "
                            "(need 0/2/4/6; palette images not supported)\n", ctype);
            return -1;
    }
    if (interlace) {
        fprintf(stderr, "decode_png: interlaced PNG not supported\n");
        return -1;
    }
    P->w = w;
    P->h = h;
    P->img_ok = 1;
    return 0;
}

static int plte_proc(PngParser *P, const uint8_t *d, uint32_t n)
{
    (void)d;
    if (n == 0 || n % 3 != 0)
        return -1;
    P->have_plte = 1;
    return 0;
}

static int idat_proc(PngParser *P, const uint8_t *d, uint32_t n)
{
    if ((uint64_t)n > MAX_DECOMP - P->zsrc_size)
        return -1;
    if (P->zsrc_cap - P->zsrc_size < (uint64_t)n) {
        uint64_t ncap = P->zsrc_cap ? P->zsrc_cap * 2 : (1u << 16);
        while (ncap - P->zsrc_size < (uint64_t)n)
            ncap *= 2;
        uint8_t *np = (uint8_t *)realloc(P->zsrc, (size_t)ncap);
        if (!np)
            return -1;
        P->zsrc = np;
        P->zsrc_cap = ncap;
    }
    memcpy(P->zsrc + P->zsrc_size, d, n);
    P->zsrc_size += n;
    return 0;
}

/* Convert decoded pixels to packed RGB, dropping alpha (Pillow's
 * convert("RGB") semantics). */
static uint8_t *convert_to_rgb(const uint8_t *src, uint64_t n, int bpp)
{
    uint8_t *dst = (uint8_t *)malloc((size_t)(n * 3));
    if (!dst)
        DIE("out of memory");
    switch (bpp) {
        case 1: /* gray */
            for (uint64_t i = 0; i < n; i++)
                dst[3 * i] = dst[3 * i + 1] = dst[3 * i + 2] = src[i];
            break;
        case 2: /* gray + alpha */
            for (uint64_t i = 0; i < n; i++)
                dst[3 * i] = dst[3 * i + 1] = dst[3 * i + 2] = src[2 * i];
            break;
        case 4: /* RGBA */
            for (uint64_t i = 0; i < n; i++) {
                dst[3 * i]     = src[4 * i];
                dst[3 * i + 1] = src[4 * i + 1];
                dst[3 * i + 2] = src[4 * i + 2];
            }
            break;
        default: /* 3 */
            memcpy(dst, src, (size_t)(n * 3));
            break;
    }
    return dst;
}

/* Reverse the PNG row filters, in place; -1 on an unknown filter type. */
static int unfilter(uint8_t *raw, uint32_t w, uint32_t h, int bpp)
{
    uint64_t rowbytes = (uint64_t)w * bpp;
    uint64_t stride = rowbytes + 1;
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * stride;
        uint8_t ft = row[0];
        uint8_t *cur = row + 1;
        switch (ft) {
        case 0:
            break;
        case 1: /* Sub */
            for (uint64_t i = bpp; i < rowbytes; i++)
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            break;
        case 2: /* Up */
            if (prev)
                for (uint64_t i = 0; i < rowbytes; i++)
                    cur[i] = (uint8_t)(cur[i] + prev[i]);
            break;
        case 3: /* Average */
            for (uint64_t i = 0; i < rowbytes; i++) {
                int a = (i >= (uint64_t)bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4: /* Paeth */
            for (uint64_t i = 0; i < rowbytes; i++) {
                int a = (i >= (uint64_t)bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= (uint64_t)bpp) ? prev[i - bpp] : 0;
                int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                cur[i] = (uint8_t)(cur[i] + pr);
            }
            break;
        default:
            fprintf(stderr, "decode_png: unknown filter type %u in scanlines\n", ft);
            return -1;
        }
        prev = cur;
    }
    return 0;
}

/* Decode an in-memory PNG into a flat RGB array (w*h*3), equivalent to
 * np.array(Image.open(...).convert("RGB")). Dies on bad input. */
static uint8_t *png_decode_mem(const uint8_t *buf, uint64_t fsize, const char *name,
                               uint32_t *w_out, uint32_t *h_out)
{
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

    if (fsize < 8 || memcmp(buf, sig, 8) != 0)
        DIE("decode_png: '%s' is not a PNG file", name);

    PngParser P;
    memset(&P, 0, sizeof P);

    uint64_t off = 8;
    int ended = 0;
    while (off + 8 <= fsize) {
        uint32_t len = rd32(buf + off);
        off += 4;
        if (off + 4 + (uint64_t)len + 4 > fsize)
            DIE("decode_png: truncated chunk in '%s'", name);
        const uint8_t *type = buf + off;
        off += 4;
        const uint8_t *data = buf + off;
        off += len;

        uint32_t crc = rd32(buf + off);
        off += 4;
        uint32_t c = crc32_begin();
        c = crc32_add(c, type, 4);
        c = crc32_add(c, data, len);
        if (crc32_end(c) != crc)
            DIE("decode_png: CRC mismatch in chunk '%.4s' of '%s'",
                (const char *)type, name);

        int rc = 0;
        if (memcmp(type, "IHDR", 4) == 0)
            rc = ihdr_proc(&P, data, len);
        else if (memcmp(type, "PLTE", 4) == 0)
            rc = plte_proc(&P, data, len);
        else if (memcmp(type, "IDAT", 4) == 0)
            rc = idat_proc(&P, data, len);
        else if (memcmp(type, "IEND", 4) == 0) {
            ended = 1;
            break;
        }
        /* other chunks are ignored */
        if (rc != 0)
            DIE("decode_png: malformed or unsupported PNG '%s'", name);
    }
    if (!ended)
        DIE("decode_png: missing IEND chunk in '%s'", name);
    if (!P.img_ok)
        DIE("decode_png: IHDR chunk missing in '%s'", name);
    if (P.have_plte)
        DIE("decode_png: palette PNGs are not supported '%s'", name);

    /* scanline stream: h rows of (1 filter byte + w*bpp bytes) */
    uint64_t stride = (uint64_t)P.w * P.bpp + 1;
    uint64_t raw_len;
    uint8_t *raw = mem_inflate(P.zsrc, P.zsrc_size, stride * P.h, &raw_len);
    free(P.zsrc);

    if (unfilter(raw, P.w, P.h, P.bpp) != 0)
        DIE("decode_png: malformed or unsupported PNG '%s'", name);

    uint64_t npix = (uint64_t)P.w * P.h * P.bpp;
    uint8_t *packed = (uint8_t *)malloc((size_t)npix);
    if (!packed)
        DIE("out of memory");
    for (uint32_t y = 0; y < P.h; y++)
        memcpy(packed + (uint64_t)y * P.w * P.bpp,
               raw + y * stride + 1, (size_t)((uint64_t)P.w * P.bpp));
    free(raw);

    uint8_t *pixels = convert_to_rgb(packed, (uint64_t)P.w * P.h, P.bpp);
    free(packed);
    *w_out = P.w;
    *h_out = P.h;
    return pixels;
}

/* --- CLI (file-based wrappers over the buffer core) --- */

#ifndef PHOTO_STATIC

static void encode_cmd(const char *input_path, const char *output_png)
{
    uint64_t file_len;
    uint8_t *data = read_file(input_path, &file_len);

    uint8_t digest[32];
    char hex[65];
    sha256(data, file_len, digest);
    to_hex(digest, hex);

    uint32_t w, h;
    uint64_t png_len;
    uint8_t *png = photo_encode_buf(data, file_len, &png_len, &w, &h);
    free(data);
    if (!png)
        fatal("[encode] ERROR: %s", photo_last_error());

    write_file(output_png, png, png_len);
    free(png);

    printf("[encode] %s  (%.2f MB)\n", input_path, (double)file_len / 1048576.0);
    printf("         -> %s  (%ux%u px, %.2f MB on disk)\n",
           output_png, w, h, (double)png_len / 1048576.0);
    printf("         SHA-256: %s\n", hex);
}

static void decode_cmd(const char *input_png, const char *output_path)
{
    uint64_t png_len;
    uint8_t *png = read_file(input_png, &png_len);

    uint64_t file_len;
    uint8_t *data = photo_decode_buf(png, png_len, &file_len);
    free(png);
    if (!data)
        fatal("[decode] ERROR: %s", photo_last_error());

    write_file(output_path, data, file_len);

    uint8_t digest[32];
    char hex[65];
    sha256(data, file_len, digest);
    to_hex(digest, hex);
    free(data);

    printf("[decode] %s\n", input_png);
    printf("         -> %s  (%.2f MB)\n", output_path,
           (double)file_len / 1048576.0);
    printf("         SHA-256: %s\n", hex);
}

static void usage(void)
{
    printf(
        "photo — embed any file into a PNG; recover it byte-for-byte later.\n"
        "\n"
        "Usage:\n"
        "  encode:  ./photo encode <input_file> <output.png>\n"
        "  decode:  ./photo decode <input.png> <output_file>\n"
        "\n"
        "Encode packs file bytes into the RGB pixels of a lossless PNG, preceded\n"
        "by an 8-byte header with the original length. Decode reverses this,\n"
        "discarding padding; the printed SHA-256 confirms a byte-exact restore.\n"
        "\n"
        "Max payload: %llu bytes (~%.0f MB). Google Photos caps images at 200 MP\n"
        "(3 bytes/pixel); for larger files, split first.\n",
        (unsigned long long)MAX_BYTES, (double)MAX_BYTES / 1e6);
}

int main(int argc, char **argv)
{
    if (argc != 4 ||
        (strcmp(argv[1], "encode") != 0 && strcmp(argv[1], "decode") != 0)) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "encode") == 0)
        encode_cmd(argv[2], argv[3]);
    else
        decode_cmd(argv[2], argv[3]);
    return 0;
}

#endif /* PHOTO_STATIC */
