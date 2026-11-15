#ifndef FR_VFS_MTP_H
#define FR_VFS_MTP_H
/* Phones and cameras over USB through libmtp, on the desktop only (FR_HAVE_MTP): mtp://<device>/<storage>/<path>.
 * The device is named by its serial number, a storage by its description ("storage-<id in hex>" where that is
 * missing or shared). The root lists the storages. */
#include "vfs.h"

typedef struct {
    char *device;  /* never empty */
    char *storage; /* NULL at the device's root */
    char *path;    /* "/" or "/a/b": no empty, "." or ".." segments, no trailing '/' */
} fr_mtp_uri;

int fr_mtp_parse(const char *uri, fr_mtp_uri *out); /* 0, or -EINVAL with out empty */
void fr_mtp_uri_free(fr_mtp_uri *u);

typedef struct {
    char *uri, *label; /* "mtp://<device>/", the device's friendly name */
    uint64_t size;     /* of its storages; 0 while a phone is locked */
} fr_mtp_device;

typedef struct {
    fr_mtp_device *v;
    int n, cap;
} fr_mtp_devices;

/* Looks for devices on a thread of its own, when the USB devices change and every few seconds while one could not
 * be opened; wake runs (on that thread) whenever the list changed. Further calls do nothing. */
void fr_mtp_start(void (*wake)(void));
unsigned fr_mtp_serial(void); /* changes with the list */
void fr_mtp_list_devices(fr_mtp_devices *out);
void fr_mtp_devices_free(fr_mtp_devices *d);

extern const fr_backend fr_backend_mtp;
#endif
