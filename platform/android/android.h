#ifndef FR_ANDROID_H
#define FR_ANDROID_H
/* What android_main.c shares with the other Android sources. */
#include "platform.h"

/* FerretActivity.<method>(String x nargs) returning String, nargs at most 3; malloc'd, NULL for Java null or an
 * exception. Any thread. */
char *java(const char *method, int nargs, ...);
int android_main_thread(void); /* the android_main thread, the one that draws */

/* saf.c */
void saf_places(fr_places *out); /* the removable volumes (SD cards, USB drives) */
char *saf_local_path(const char *uri); /* the /storage path of a saf:// location when it is readable there, else NULL */
#endif
