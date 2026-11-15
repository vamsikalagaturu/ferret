#ifndef FR_PLATFORM_H
#define FR_PLATFORM_H
/* What differs between the desktop (platform/linux/platform_linux.c) and Android (platform/android/android_main.c).
 * Everything runs on the window's thread except plat_wake(). */
#include "core/util.h"
#include "disks/plan.h"
#include "raylib.h"

int fr_app_main(int argc, char **argv); /* src/app/main.c */
/* ferret --replay (src/app/replay.c) drives the window from a script: the pointer is the script's, and the
 * desktop's clipboard and apps stay out of it */
int replay_running(void);
Vector2 replay_pointer(void);

void plat_init_window(void);   /* opens the window */
void plat_frame(void);         /* once per frame, before the frame takes its input */
void plat_wait(double seconds); /* sleeps until input, plat_wake() or the timeout */
void plat_wake(void);          /* any thread; does nothing once the window is closed */
void plat_close_window(void);  /* closes the window; later plat_wake() calls are dropped */
float plat_scale(void);        /* pixels per dp */
Vector2 plat_mouse(void);      /* the pointer in framebuffer pixels */
int plat_touch(void);          /* a touchscreen is the pointer */
/* screen pixels covered by system bars, the cutout or the soft keyboard */
void plat_insets(float *left, float *top, float *right, float *bottom);
void plat_keyboard(int show);
int plat_open(const char *path); /* in the app the system picks for it; 0 when handed over */
/* paths separated by '\n' to the system's share sheet (Android); 0 when handed over, -1 where there is none */
int plat_share(const char *paths);
/* Open with: the desktop lists the apps itself (platform/linux/apps_linux.c), Android shows its own chooser */
typedef struct {
    char *id, *name; /* the desktop ID ("org.gnome.TextEditor.desktop") and what it is called */
    int is_default;
} fr_app_choice;

typedef struct {
    fr_app_choice *v;
    int n, cap;
} fr_app_choices;

/* the apps that open files of type mime, the default first; -1 where there is no list (plat_open_chooser) */
int plat_apps_for(const char *mime, fr_app_choices *out);
void fr_app_choices_free(fr_app_choices *c);
int plat_open_app(const char *id, const fr_strs *paths); /* 0 when started */
int plat_set_default_app(const char *mime, const char *id); /* 0, or -errno */
int plat_open_chooser(const char *path); /* the system's own "open with"; 0 when handed over, -1 none */
/* a terminal in dir, or with dir NULL whether there is one; 0 when started (or found), -1 none */
int plat_terminal(const char *dir);

void plat_set_clipboard(const char *text);
char *plat_clipboard(void); /* malloc'd, NULL for none */
void plat_haptic(void);

/* Server passwords in the system's keystore (platform/linux/secret_linux.c: libsecret; Android: the Android
 * Keystore): 0 done, -1 not (or no keystore: plat_secret_available() is 0). Window thread. */
int plat_secret_available(void);
int plat_secret_load(const char *key, char **user, char **password); /* both malloc'd */
int plat_secret_save(const char *key, const char *user, const char *password);
int plat_secret_forget(const char *key);

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
/* DISK_MOUNT_RO and DISK_RELEASE are the disk manager's look-inside mount and its unmount; DISK_POWER_OFF powers a
 * drive off as it is (UDisks2 refuses while it is in use) */
enum { DISK_MOUNT = 1, DISK_UNMOUNT, DISK_EJECT, DISK_MOUNT_RO, DISK_RELEASE, DISK_POWER_OFF };

typedef struct {
    char *id; /* names it in requests */
    char *label, *device;
    fr_strs mounts; /* none: not mounted */
    char *uri;      /* a device opened as this location instead of mounted (a phone over MTP), else NULL */
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

/* The disk manager (src/app/disks_view.c): partitioning, formatting and image writes through UDisks2 on the desktop.
 * Where it is not supported (Android) the view is a storage overview of the places instead. Every call UDisks2 makes
 * for it is judged by dk_refusal on the live tree first. */
int plat_dm_supported(void);
unsigned plat_dm_serial(void);   /* changes with UDisks2's objects */
int plat_dm_tree(dk_tree *out);  /* a copy of them; -1 when UDisks2 is not running */
void plat_dm_apply(dk_steps *steps); /* takes the steps; runs them in order on a thread, stopping at the first error */

typedef struct {
    int running;
    int done;         /* steps finished so far */
    int step_done;    /* a step finished since the last call */
    int finished;     /* the run ended since the last call; error says how */
    double progress;  /* of the step ferret drives itself (an image write), -1 else */
    char text[128];   /* what that step or UDisks2's job does now */
    char error[512];
} fr_dm_run;
void plat_dm_status(fr_dm_run *out); /* takes step_done and finished */

/* the local path holding a place's files (for the usage scan), NULL when there is none */
char *plat_volume_path(const char *uri);
#endif
