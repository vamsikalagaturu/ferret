#ifndef FR_RECENT_H
#define FR_RECENT_H
/* The folders visited last, newest first. */
#include "util.h"

#define FR_RECENT_MAX 50

/* uri to the front of list (moved there when it is in it already), with any password taken out of it; the oldest
 * go past max */
void fr_recent_push(fr_strs *list, const char *uri, int max);
/* uri without the password of any "user:password@" in it, also of an outer URI inside it (archive://) */
char *fr_uri_strip_password(const char *uri);
#endif
