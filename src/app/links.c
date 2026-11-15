#include "app.h"
#include <stdlib.h>
#include <string.h>

/* What yl took to link to: kept apart from the clipboard, and kept after a paste (links to the same items can go
 * to several folders) until the next yl. */
static fr_strs yanked;

static int local_all(const fr_strs *uris)
{
    for (int i = 0; i < uris->n; i++)
        if (!fr_uri_local(uris->v[i])) return 0;
    return uris->n > 0;
}

static void yank_now(const char *s, int n)
{
    fr_strs uris = browser_target_uris();
    if (!uris.n) return;
    if (!local_all(&uris)) {
        ui_toast("Links are made only to files on this device");
        fr_strs_free(&uris);
        return;
    }
    fr_strs_free(&yanked);
    yanked = uris;
    browser_flash_targets();
    browser_clear_selection();
    ui_toast("%d %s to link to; P makes the links in a folder", yanked.n, yanked.n == 1 ? "item" : "items");
}

void links_yank(void) { app_defer(yank_now, NULL, 0); }

int links_ready(void) { return yanked.n || clip.uris.n; }

/* n: 1 relative, 0 absolute, -1 as the settings say */
static void paste_now(const char *s, int n)
{
    const fr_strs *from = yanked.n ? &yanked : &clip.uris;
    if (!from->n) {
        ui_toast("Nothing to link to: yl takes the items first");
        return;
    }
    if (!br.uri || !fr_uri_local(br.uri) || br.results) {
        ui_toast("Links are made only in folders on this device");
        return;
    }
    if (!local_all(from)) {
        ui_toast("Links are made only to files on this device");
        return;
    }
    fr_job_submit_link(from, br.uri, n < 0 ? settings.relative_links : n);
}

void links_paste(int relative) { app_defer(paste_now, NULL, relative); }
