#ifndef FR_DISKS_IMAGE_H
#define FR_DISKS_IMAGE_H
/* Bootable USB sticks: one hybrid image copied byte for byte onto the whole device and verified by reading it back.
 * The device arrives as a file descriptor UDisks2 opened (Block.OpenDevice), so no privileges are needed here. */
#include <stddef.h>
#include <stdint.h>

/* Whether path can be written raw to a stick: a hybrid image carries an MBR boot signature (0x55AA at byte 510).
 * 0, or -1 with why not in why. */
int dk_image_check(const char *path, char *why, size_t len);

/* Writes size bytes of image to fd, fdatasync, drops the cached pages, reads it back and compares SHA-256s; closes
 * fd. progress (may be NULL) gets the fraction of the current pass and what it does. NULL, or the malloc'd error. */
char *dk_image_write(int fd, const char *image, uint64_t size, void (*progress)(void *ctx, double fraction, const char *text), void *ctx);

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} dk_sha256;
void dk_sha256_init(dk_sha256 *s);
void dk_sha256_add(dk_sha256 *s, const void *data, size_t n);
void dk_sha256_end(dk_sha256 *s, unsigned char out[32]);
#endif
