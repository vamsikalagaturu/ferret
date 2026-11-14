#ifndef FR_PLATFORM_H
#define FR_PLATFORM_H
/* What differs between the desktop (platform/linux/platform_linux.c) and Android (platform/android/android_main.c).
 * Everything runs on the window's thread except plat_wake(). */
#include "core/util.h"
#include "raylib.h"

int fr_app_main(int argc, char **argv); /* src/app/main.c */

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
#endif
