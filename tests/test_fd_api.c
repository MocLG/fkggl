/* Native test harness for the photo.c fd + buffer APIs (not shipped). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include "photo.h"

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

int main(void)
{
    static const size_t sizes[] = {0, 1, 4, 100, 4095, 4096, 65535, 65536, 70000};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        size_t n = sizes[si];
        uint8_t *data = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(i * 131 + si);
        uint64_t png_len; uint32_t w, h;
        uint8_t *png = photo_encode_buf(data, n, &png_len, &w, &h);
        char name[64]; snprintf(name, sizeof name, "buf round-trip %zu", n);
        if (!png) { CHECK(0, name); free(data); continue; }
        uint64_t out_len;
        uint8_t *out = photo_decode_buf(png, png_len, &out_len);
        CHECK(out && out_len == n && (n == 0 || memcmp(out, data, n) == 0), name);
        free(png); free(out); free(data);
    }

    uint64_t png_len; uint32_t w, h;
    CHECK(photo_encode_buf(NULL, photo_max_bytes() + 1, &png_len, &w, &h) == NULL,
          "oversize rejected");
    uint8_t junk[64]; memset(junk, 'A', sizeof junk);
    uint64_t dl;
    CHECK(photo_decode_buf(junk, sizeof junk, &dl) == NULL, "garbage rejected");

    const size_t BIG = 3u << 20;
    uint8_t *data = malloc(BIG);
    for (size_t i = 0; i < BIG; i++) data[i] = (uint8_t)(i * 7);
    int fd = open("/tmp/photo_fdtest", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, data, BIG); close(fd);

    char sha1[65] = {0}, sha2[65] = {0};
    uint64_t got = 0;
    int in = open("/tmp/photo_fdtest", O_RDONLY);
    int out = open("/tmp/photo_fdtest.png", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int rc = photo_encode_fd(in, BIG, out, sha1);
    close(out); close(in);
    CHECK(rc == 0 && strlen(sha1) == 64, "fd encode");

    in = open("/tmp/photo_fdtest.png", O_RDONLY);
    out = open("/tmp/photo_fdtest.out", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    rc = photo_decode_fd(in, out, sha2, &got);
    close(out); close(in);
    CHECK(rc == 0 && got == BIG && strcmp(sha1, sha2) == 0, "fd decode + sha match");

    fd = open("/tmp/photo_fdtest.out", O_RDONLY);
    uint8_t *back = malloc(BIG);
    size_t total = 0;
    while (total < BIG) {
        ssize_t r = read(fd, back + total, BIG - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    CHECK(total == BIG && memcmp(back, data, total) == 0, "fd decode content");
    free(back);

    in = open("/tmp/photo_fdtest", O_RDONLY);
    out = open("/tmp/photo_fdtest0.png", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    rc = photo_encode_fd(in, 0, out, sha1);
    close(out); close(in);
    in = open("/tmp/photo_fdtest0.png", O_RDONLY);
    out = open("/tmp/photo_fdtest0.out", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    rc = photo_decode_fd(in, out, sha2, &got);
    close(out); close(in);
    CHECK(rc == 0 && got == 0, "fd empty round-trip");

    /* app flow: 3 MB file split into 2 parts (2 MB + 1 MB), each encoded
     * from an lseek'ed fd like the Android activity does, then decoded
     * back-to-back into one chained output fd. */
    const size_t P1 = 2u << 20, TOTAL = 3u << 20;
    in = open("/tmp/photo_fdtest", O_RDONLY);
    out = open("/tmp/photo_fdtest_p0.png", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    lseek(in, 0, SEEK_SET);
    CHECK(photo_encode_fd(in, P1, out, sha1) == 0, "split part 0 encode");
    close(out);
    out = open("/tmp/photo_fdtest_p1.png", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    lseek(in, P1, SEEK_SET);
    CHECK(photo_encode_fd(in, TOTAL - P1, out, sha2) == 0, "split part 1 encode");
    close(out); close(in);

    int p0 = open("/tmp/photo_fdtest_p0.png", O_RDONLY);
    int p1 = open("/tmp/photo_fdtest_p1.png", O_RDONLY);
    out = open("/tmp/photo_fdtest_merged", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    uint64_t s0 = 0, s1 = 0;
    char msha0[65] = {0}, msha1[65] = {0};
    CHECK(photo_decode_fd(p0, out, msha0, &s0) == 0 && s0 == P1, "split part 0 decode");
    CHECK(photo_decode_fd(p1, out, msha1, &s1) == 0 && s1 == TOTAL - P1,
          "split part 1 decode (chained append)");
    close(p0); close(p1); close(out);

    fd = open("/tmp/photo_fdtest_merged", O_RDONLY);
    uint8_t *merged = malloc(TOTAL);
    total = 0;
    while (total < TOTAL) {
        ssize_t r = read(fd, merged + total, TOTAL - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    CHECK(total == TOTAL && memcmp(merged, data, TOTAL) == 0, "split-merge content");
    free(merged);

    unlink("/tmp/photo_fdtest"); unlink("/tmp/photo_fdtest.png");
    unlink("/tmp/photo_fdtest.out"); unlink("/tmp/photo_fdtest0.png");
    unlink("/tmp/photo_fdtest0.out"); unlink("/tmp/photo_fdtest_p0.png");
    unlink("/tmp/photo_fdtest_p1.png"); unlink("/tmp/photo_fdtest_merged");
    free(data);
    printf(fails ? "FD_TESTS_FAILED\n" : "FD_TESTS_OK\n");
    return fails != 0;
}
