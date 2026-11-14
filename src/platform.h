#ifndef FR_PLATFORM_H
#define FR_PLATFORM_H
/* What differs between the desktop (platform/linux/platform_linux.c) and Android (platform/android/android_main.c).
 * Everything runs on the window's thread except plat_wake(). */
#include "core/util.h"
#include "raylib.h"

int fr_app_main(int argc, char **argv); /* src/app/main.c */
/* ferret --replay (src/app/replay.c) drives the window from a script: the pointer is the script's, and the
 * desktop's clipboard and apps stay out of it */
int replay_running(void);
Vector2 replay_pointer(void);

void plat_init_window(void);   /* opens the window */
void plat_frame(void);         /* once per frame, before the frame takes its input */
void plat_wait(double seconds); /* sleeps until input, plat_wake() or the timeout */
void plat_wake(void);          /* any thread */
float plat_scale(void);        /* pixels per dp */
Vector2 plat_mouse(void);      /* the pointer in framebuffer pixels */
int plat_touch(void);          /* a touchscreen is the pointer */
/* screen pixels covered by system bars, the cutout or the soft keyboard */
void plat_insets(float *left, float *top, float *right, float *bottom);
void plat_keyboard(int show);
int plat_open(const char *path); /* in the app the system picks for it; 0 when handed over */
void plat_set_clipboard(const char *text);
char *plat_clipboard(void); /* malloc'd, NULL for none */
void plat_haptic(void);

/* Storage permission: 1 granted, 0 not yet; plat_request_storage() opens the system's settings for it. */
int plat_storage_granted(void);
void plat_request_storage(void);

typedef struct {
    char *label, *uri, *icon;
    int removable;
} fr_place;

typedef struct {
    fr_place *v;
    int n, cap;
} fr_places;

void plat_places(fr_places *out); /* the storages and standard folders, in order */
void fr_places_free(fr_places *p);
char *plat_home_uri(void);        /* where the window starts */

/* Filesystems to mount, mounted or not (platform/linux/disks_linux.c; none on Android). Requests run in the
 * background, and plat_wake() tells when one finished or the list changed. */
enum { DISK_MOUNT = 1, DISK_UNMOUNT, DISK_EJECT };

typedef struct {
    char *id; /* names it in requests */
    char *label, *device;
    fr_strs mounts; /* none: not mounted */
    uint64_t size;
    int removable, locked;
    int busy; /* the DISK_* request running on it, 0 none */
} fr_disk;

typedef struct {
    fr_disk *v;
    int n, cap;
} fr_disks;

typedef struct {
    int op;
    char *id, *label;
    char *path;        /* DISK_MOUNT: where */
    fr_strs unmounted; /* DISK_UNMOUNT, DISK_EJECT: the mount points that went */
    char *error;       /* NULL: done */
} fr_disk_done;

unsigned plat_disks_serial(void); /* changes with the list or what runs */
void plat_disks(fr_disks *out);
void fr_disks_free(fr_disks *d);
void plat_disk_request(int op, const char *id); /* DISK_EJECT: unmounts the drive's filesystems, then powers it off */
int plat_disk_done(fr_disk_done *out);           /* 1 with a finished request (its strings are the caller's), 0 none */
void fr_disk_done_free(fr_disk_done *d);
#endif
