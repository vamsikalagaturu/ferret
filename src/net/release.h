#ifndef FR_RELEASE_H
#define FR_RELEASE_H
/* ferret's releases on GitHub: the latest one's version, page and APK, for the updater (src/app/update.c). */

#define FR_RELEASE_REPO "vamsikalagaturu/ferret"

typedef struct {
    char *tag;  /* "v0.2.0" */
    char *page; /* the release's page */
    char *apk;  /* its .apk asset's download URL, NULL when it has none */
    char *linux_bin; /* its FR_RELEASE_LINUX_ASSET asset's, NULL when it has none */
} fr_release;

/* the Linux program as a release carries it, for this machine's architecture */
#if defined(__x86_64__)
#define FR_RELEASE_LINUX_ASSET "ferret-linux-x86_64"
#elif defined(__aarch64__)
#define FR_RELEASE_LINUX_ASSET "ferret-linux-aarch64"
#else
#define FR_RELEASE_LINUX_ASSET "ferret-linux"
#endif

/* GitHub's /releases/latest answer for FR_RELEASE_REPO, fetched (blocking: a worker thread's call); 0 or -errno */
int fr_release_latest(fr_release *out);
/* the fields fr_release_latest reads out of the API's JSON; -EINVAL without a tag */
int fr_release_parse(const char *json, fr_release *out);
void fr_release_free(fr_release *r);

/* "v1.2.3" or "1.2.3-dev+abc" against another: <0, 0, >0 by major, minor and patch, a pre-release before its
 * release */
int fr_version_cmp(const char *a, const char *b);
#endif
