/*
 * photo — embed any file into a lossless PNG and recover it byte-for-byte.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Luka Gejak
 *
 * Public API of the library build (photo.c compiled with -DPHOTO_STATIC).
 */
#ifndef PHOTO_H
#define PHOTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max payload per PNG: 599,999,992 bytes (200 MP * 3 B/pixel - 8 B header). */
unsigned long long photo_max_bytes(void);

/* Encode len bytes into a malloc'd PNG image. Returns NULL on error (see
 * photo_last_error()); on success stores the buffer size in png_len_out
 * and the image geometry in w_out / h_out. */
uint8_t *photo_encode_buf(const uint8_t *data, uint64_t len,
                          uint64_t *png_len_out, uint32_t *w_out, uint32_t *h_out);

/* Extract the embedded file from a PNG buffer. Returns a malloc'd buffer
 * and its size in len_out, or NULL on error (see photo_last_error()). */
uint8_t *photo_decode_buf(const uint8_t *png, uint64_t png_len, uint64_t *len_out);

/* Like photo_encode_buf, but reads len bytes from in_fd at its current
 * position and writes the PNG to out_fd. Stores the payload SHA-256 hex
 * in sha_hex (65 bytes). Returns 0 or -1. */
int photo_encode_fd(int in_fd, uint64_t len, int out_fd, char sha_hex[65]);

/* Reads a whole PNG from in_fd, extracts the payload and writes it to
 * out_fd. Stores the payload SHA-256 hex in sha_hex and the payload size
 * in len_out. Returns 0 or -1. */
int photo_decode_fd(int in_fd, int out_fd, char sha_hex[65], uint64_t *len_out);

/* Message for the most recent error (valid until the next photo_* call). */
const char *photo_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_H */
