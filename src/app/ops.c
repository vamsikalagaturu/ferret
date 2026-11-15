#include "app.h"
#include "vfs/trash.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

fr_clip clip;

/* the one dialog open at a time */
static enum { DLG_NONE, DLG_RENAME, DLG_NEW_FOLDER, DLG_DELETE, DLG_PROPERTIES, DLG_ERROR, DLG_BULK, DLG_OPEN_WITH } dlg;
static fr_text field;
static char *dlg_uri;          /* rename: the item */
static fr_strs dlg_uris;       /* delete: the items */
static int dlg_permanent;
static char dlg_message[1024], dlg_title[128];
static int dlg_pending;     /* rename, new: the call runs off this device (remote.c) */
static unsigned dlg_serial; /* the dialog an answer from there is for */

static int job_asking(void)
{
    fr_job_status st[16];
    int n = fr_jobs_list(st, 16);
    for (int i = 0; i < n; i++)
        if (st[i].state == JOB_ASKING) return 1;
    return 0;
}

int ops_dialog_open(void) { return dlg != DLG_NONE || job_asking(); }

static void dlg_close(void)
{
    if (dlg == DLG_PROPERTIES) props_close();
    if (dlg == DLG_BULK) bulk_rename_close();
    if (dlg == DLG_OPEN_WITH) openwith_close();
    dlg = DLG_NONE;
    dlg_pending = 0;
    dlg_serial++;
    free(dlg_uri);
    dlg_uri = NULL;
    fr_strs_free(&dlg_uris);
    ui.dirty = 1;
}

void ops_open_entry(const char *uri, const fr_entry *e)
{
    if (e->is_dir) browser_open(uri, 1);
    else if (fr_archive_browsable(e->name)) {
        char *root = fr_archive_root(uri);
        browser_open(root, 1);
        free(root);
    } else if (!fr_uri_local(uri) && fr_vfs_backend(uri)) {
        fr_strs one = {(char *[]){(char *)uri}, 1, 1};
        fetch_open(&one, 0);
    } else {
        const char *path = fr_uri_local(uri);
        if (!path || plat_open(path)) ui_toast("No app can open %s", e->name);
    }
}

void ops_open_item(fr_item *it)
{
    char *uri = fr_uri_join(br.uri, it->e.name);
    ops_open_entry(uri, &it->e);
    free(uri);
}

/* archives are rewritten whole for a change inside, so it runs as a job */
static int in_archive(const char *uri) { return fr_archive_is_uri(uri); }

int ops_rename(const char *from, const char *to, remote_done done, unsigned ctx)
{
    if (remote_slow(from)) {
        remote_rename(from, to, done, ctx);
        return 1;
    }
    if (!in_archive(from)) {
        int rc = fr_vfs_rename(from, to);
        if (!rc) undo_record_rename(from, to);
        return rc;
    }
    if (fr_vfs_exists(to)) return -EEXIST;
    fr_job_submit_rename(from, to);
    return 0;
}

int ops_archive_targets(void)
{
    if (!br.uri || in_archive(br.uri)) return 0;
    fr_strs uris = browser_target_uris();
    int n = uris.n;
    for (int i = 0; i < uris.n; i++) {
        char *name = fr_uri_name(uris.v[i]);
        if (!fr_archive_browsable(name)) n = 0;
        free(name);
    }
    fr_strs_free(&uris);
    return n;
}

void ops_extract(int into_folder)
{
    if (!ops_archive_targets()) {
        ui_toast("Choose archives to extract (zip, tar, 7z, rar, iso)");
        return;
    }
    fr_strs uris = browser_target_uris();
    fr_job_submit_extract(&uris, br.uri, into_folder);
    fr_strs_free(&uris);
    browser_clear_selection();
}

char *ops_compress_name(void)
{
    fr_strs uris = browser_target_uris();
    char *name = NULL;
    if (uris.n == 1) {
        fr_entry e = {0};
        int is_dir = !fr_vfs_stat(uris.v[0], &e) && e.is_dir;
        fr_entry_free(&e);
        char *full = fr_uri_name(uris.v[0]);
        name = fr_format("%.*s.zip", (int)fr_stem_len(full, is_dir), full);
        free(full);
    } else if (uris.n) {
        char *here = fr_uri_name(br.uri);
        name = fr_format("%s.zip", *here ? here : "Archive");
        free(here);
    }
    fr_strs_free(&uris);
    return name;
}

void ops_compress(void)
{
    if (!br.uri || in_archive(br.uri)) {
        ui_toast("Archives are made outside archives");
        return;
    }
    char *name = ops_compress_name();
    if (!name) return;
    fr_strs uris = browser_target_uris();
    char *dest = fr_uri_free_name(br.uri, name, 0), *shown = fr_uri_name(dest);
    fr_job_submit_compress(&uris, dest);
    free(br.focus_name);
    br.focus_name = shown;
    fr_strs_free(&uris);
    free(dest), free(name);
    browser_clear_selection();
}

static void copy_now(const char *s, int cut)
{
    fr_strs_free(&clip.uris);
    clip.uris = browser_target_uris();
    clip.cut = cut;
    if (!clip.uris.n) return;
    browser_flash_targets();
    browser_clear_selection();
    ui_toast("%d %s %s; open a folder and paste", clip.uris.n, clip.uris.n == 1 ? "item" : "items", cut ? "cut" : "copied");
}

void ops_copy(int cut) { app_defer(copy_now, NULL, cut); }

static void paste_now(const char *s, int n)
{
    if (!clip.uris.n || !br.uri) return;
    fr_job_submit(clip.cut ? JOB_MOVE : JOB_COPY, &clip.uris, br.uri);
    if (clip.cut) fr_strs_free(&clip.uris);
}

void ops_paste(void) { app_defer(paste_now, NULL, 0); }

static void to_other_now(const char *s, int move)
{
    fr_browser *other = tabs_other(&tabs);
    if (!other || br_cur != tabs_current(&tabs)) return;
    if (!other->uri || !br.uri || other->results || fr_trash_is_root(other->uri)) {
        ui_toast("The other pane shows no folder to put them in");
        return;
    }
    if (!strcmp(other->uri, br.uri)) {
        ui_toast("The other pane shows this folder too");
        return;
    }
    fr_strs uris = browser_target_uris();
    if (!uris.n) return;
    fr_job_submit(move ? JOB_MOVE : JOB_COPY, &uris, other->uri);
    fr_strs_free(&uris);
    browser_clear_selection();
}

void ops_to_other_pane(int move) { app_defer(to_other_now, NULL, move); }

static void clip_clear_now(const char *s, int n) { fr_strs_free(&clip.uris); }

void ops_clip_clear(void) { app_defer(clip_clear_now, NULL, 0); }

static fr_strs targets(void) { return browser_target_uris(); }

void ops_delete(int permanently)
{
    fr_strs uris = targets();
    if (!uris.n) return;
    dlg = DLG_DELETE;
    dlg_permanent = permanently || !fr_vfs_can_trash(uris.v[0]);
    char *first = fr_uri_name(uris.v[0]);
    if (dlg_permanent) {
        snprintf(dlg_title, sizeof dlg_title, "Delete %s?", uris.n == 1 ? "this item" : "these items");
        if (uris.n == 1) snprintf(dlg_message, sizeof dlg_message, "\"%s\" will be deleted. This cannot be undone.", first);
        else snprintf(dlg_message, sizeof dlg_message, "%d items will be deleted. This cannot be undone.", uris.n);
    } else {
        snprintf(dlg_title, sizeof dlg_title, "Move to the trash?");
        if (uris.n == 1) snprintf(dlg_message, sizeof dlg_message, "\"%s\" goes to the trash.", first);
        else snprintf(dlg_message, sizeof dlg_message, "%d items go to the trash.", uris.n);
    }
    free(first);
    fr_strs_free(&dlg_uris);
    dlg_uris = uris;
}

void ops_delete_uris(fr_strs *uris, const char *title, const char *message)
{
    dlg = DLG_DELETE;
    dlg_permanent = 1;
    snprintf(dlg_title, sizeof dlg_title, "%s", title);
    snprintf(dlg_message, sizeof dlg_message, "%s", message);
    fr_strs_free(&dlg_uris);
    dlg_uris = *uris;
    *uris = (fr_strs){0};
}

void ops_open_with(void)
{
    if (dlg == DLG_NONE && openwith_open()) dlg = DLG_OPEN_WITH;
}

void ops_open_with_paths(fr_strs *paths)
{
    if (dlg == DLG_NONE && openwith_open_paths(paths)) dlg = DLG_OPEN_WITH;
    fr_strs_free(paths);
}

void ops_terminal_here(void)
{
    const char *dir = br.uri ? fr_uri_local(br.uri) : NULL;
    if (!dir) ui_toast("A terminal opens only in a folder on this device");
    else if (plat_terminal(dir)) ui_toast("No terminal found: set $TERMINAL, or install one such as foot or xterm");
}

void ops_rename_start(void)
{
    fr_strs uris = targets();
    if (uris.n != 1) {
        fr_strs_free(&uris);
        return;
    }
    fr_item *it = browser_item_of(uris.v[0]);
    int is_dir = it && it->e.is_dir;
    char *name = fr_uri_name(uris.v[0]);
    text_set(&field, name);
    text_select(&field, 0, (int)fr_stem_len(name, is_dir));
    free(name);
    free(dlg_uri);
    dlg_uri = fr_strdup(uris.v[0]);
    fr_strs_free(&uris);
    dlg = DLG_RENAME;
}

/* the name the dialog offers, unless it was changed meanwhile */
static void free_name_found(const char *uri, unsigned ctx)
{
    if (ctx != dlg_serial || dlg != DLG_NEW_FOLDER || dlg_pending || strcmp(field.buf, "New folder/")) return;
    char *name = fr_uri_name(uri), *shown = fr_format("%s/", name);
    text_set(&field, shown);
    text_select(&field, 0, field.len - 1);
    free(name), free(shown);
}

void ops_new_folder_start(void)
{
    if (!ui.touch) {
        keys_new_start("New folder/");
        return;
    }
    if (remote_slow(br.uri)) {
        text_set(&field, "New folder/");
        text_select(&field, 0, field.len - 1);
        dlg = DLG_NEW_FOLDER;
        remote_free_name(br.uri, "New folder", 1, free_name_found, dlg_serial);
        return;
    }
    char *uri = fr_uri_free_name(br.uri, "New folder", 1), *name = fr_uri_name(uri), *shown = fr_format("%s/", name);
    text_set(&field, shown);
    text_select(&field, 0, field.len - 1);
    free(uri), free(name), free(shown);
    dlg = DLG_NEW_FOLDER;
}

void ops_new_start(void)
{
    if (!ui.touch) {
        keys_new_start("");
        return;
    }
    text_set(&field, "");
    dlg = DLG_NEW_FOLDER;
}

void ops_share(void)
{
    fr_strs uris = targets();
    size_t len = 1;
    for (int i = 0; i < uris.n; i++) len += strlen(uris.v[i]) + 1;
    char *paths = fr_realloc(NULL, len), *o = paths;
    *o = 0;
    for (int i = 0; i < uris.n; i++) {
        const char *p = fr_uri_local(uris.v[i]);
        if (p) o += sprintf(o, "%s%s", o == paths ? "" : "\n", p);
    }
    if (*paths && plat_share(paths)) ui_toast("No app can take these");
    free(paths);
    fr_strs_free(&uris);
}

void ops_bulk_rename(int as_text)
{
    if (br.uri && in_archive(br.uri)) {
        ui_toast("Rename items inside an archive one at a time");
        return;
    }
    if (bulk_rename_open(as_text)) dlg = DLG_BULK;
}

void ops_properties(void)
{
    fr_strs uris = targets();
    if (!uris.n) return;
    props_open(&uris);
    dlg = DLG_PROPERTIES;
}

/* an answer from off this device for the rename or new dialog: it closes once that worked */
static void dlg_done(int rc, unsigned ctx)
{
    if (ctx != dlg_serial || !dlg_pending) return;
    dlg_pending = 0;
    if (!rc) dlg_close();
}

static void rename_commit(void)
{
    if (dlg_pending) return;
    char *old = fr_uri_name(dlg_uri);
    if (!strcmp(old, field.buf)) {
        free(old);
        dlg_close();
        return;
    }
    free(old);
    if (!fr_name_valid(field.buf)) {
        ui_toast("A name cannot be empty, \".\", \"..\" or contain \"/\"");
        return;
    }
    char *parent = fr_uri_parent(dlg_uri), *to = fr_uri_join(parent, field.buf);
    int rc = ops_rename(dlg_uri, to, dlg_done, dlg_serial);
    if (rc == 1) dlg_pending = 1;
    else if (rc == -EEXIST) ui_toast("\"%s\" already exists here", field.buf);
    else if (rc) ui_toast("Could not rename: %s", fr_vfs_error(rc));
    else {
        free(br.focus_name);
        br.focus_name = fr_strdup(field.buf);
        browser_reload();
        dlg_close();
    }
    free(parent), free(to);
}

/* a name ending in "/" makes a folder, any other an empty file */
int ops_create(const char *typed, remote_done done, unsigned ctx)
{
    size_t n = strlen(typed);
    int folder = n && typed[n - 1] == '/';
    char *name = fr_strndup(typed, folder ? n - 1 : n);
    int rc = -EINVAL;
    if (!fr_name_valid(name)) ui_toast("A name cannot be empty, \".\", \"..\" or contain \"/\" (except one at the end, for a folder)");
    else if (remote_slow(br.uri)) {
        char *uri = fr_uri_join(br.uri, name);
        remote_create(uri, folder, done, ctx);
        free(uri);
        rc = 1;
    } else {
        char *uri = fr_uri_join(br.uri, name);
        rc = 0;
        if (in_archive(uri)) {
            if (fr_vfs_exists(uri)) rc = -EEXIST;
            else fr_job_submit_create(uri, folder);
        } else if (folder) rc = fr_vfs_mkdir(uri);
        else {
            fr_stream *s = fr_vfs_open_write(uri, 1, &rc);
            if (s) rc = s->close(s);
        }
        if (!rc && !in_archive(uri)) undo_record_created(uri, folder);
        free(uri);
        if (rc == -EEXIST) ui_toast("\"%s\" already exists here", name);
        else if (rc) ui_toast("Could not make the %s: %s", folder ? "folder" : "file", fr_vfs_error(rc));
        else {
            browser_drop_placeholder();
            free(br.focus_name);
            br.focus_name = fr_strdup(name);
            browser_reload();
        }
    }
    free(name);
    return rc;
}

static void new_folder_commit(void)
{
    if (dlg_pending) return;
    int rc = ops_create(field.buf, dlg_done, dlg_serial);
    if (rc == 1) dlg_pending = 1;
    else if (!rc) dlg_close();
}

/* ---- jobs ---- */

static int conflict_for_all;

void ops_poll_jobs(void)
{
    static int seen_finished;
    int f = fr_jobs_finished();
    if (f == seen_finished) return;
    seen_finished = f;
    fr_job_status st[16];
    int n = fr_jobs_list(st, 16), changed = 0;
    for (int i = 0; i < n; i++) {
        if (st[i].state != JOB_DONE && st[i].state != JOB_CANCELLED) continue;
        if (fetch_job_done(&st[i])) {
            fr_job_dismiss(st[i].id);
            continue;
        }
        changed = 1;
        static const char *done[] = {"Copied", "Moved", "Deleted", "Moved to the trash", "Changed the permissions of",
                                     "Extracted", "Compressed", "Renamed", "Made", "Did", "Opened", "Synced", "Linked"};
        if (undo_job_done(&st[i])) {
        } else if (st[i].state == JOB_CANCELLED) ui_toast("Cancelled");
        else if (st[i].failed) {
            if (dlg == DLG_PROPERTIES) props_close();
            if (dlg == DLG_BULK) bulk_rename_close();
            dlg = DLG_ERROR;
            snprintf(dlg_title, sizeof dlg_title, "%d %s could not be done", st[i].failed, st[i].failed == 1 ? "item" : "items");
            snprintf(dlg_message, sizeof dlg_message, "%s", st[i].error);
        } else if (st[i].kind == JOB_COMPRESS || st[i].kind == JOB_RENAME || st[i].kind == JOB_CREATE)
            ui_toast("%s %s", done[st[i].kind], st[i].dest_name);
        else
            ui_toast("%s %d %s", done[st[i].kind], st[i].items, st[i].items == 1 ? "item" : "items");
        fr_job_dismiss(st[i].id);
    }
    /* after the renames were recorded, which search results follow */
    if (changed) browser_reload_shown();
}

float ops_draw_jobs(Rectangle area)
{
    fr_job_status st[16];
    int n = fr_jobs_list(st, 16), active = -1, queued = 0;
    for (int i = 0; i < n; i++) {
        if (st[i].state == JOB_RUNNING || st[i].state == JOB_ASKING) active = i;
        if (st[i].state == JOB_QUEUED) queued++;
    }
    if (active < 0) return 0;
    fr_job_status *j = &st[active];

    if (j->state == JOB_ASKING) {
        char title[300], msg[400];
        snprintf(title, sizeof title, "\"%s\" already exists", j->conflict);
        snprintf(msg, sizeof msg, j->conflict_dir ? "Replacing merges the two folders: files with the same name are overwritten."
                                                  : "Replacing overwrites the file in %s.", j->dest_name);
        fr_dialog d = {.title = title, .message = msg, .check = "Do this for all conflicts", .checked = &conflict_for_all,
                       .buttons = {"Skip", "Keep both", "Replace"}, .nbuttons = 3, .cancel = 0};
        int b = ui_dialog(&d);
        if (b >= 0) {
            static const fr_conflict answers[] = {CONFLICT_SKIP, CONFLICT_KEEP_BOTH, CONFLICT_REPLACE};
            fr_job_answer(j->id, answers[b], conflict_for_all);
            conflict_for_all = 0;
        }
    }

    float h = DP(ui.touch ? 64 : 52), pad = DP(16);
    Rectangle r = {area.x, area.y + area.height - h, area.width, h};
    ui_rect(r, theme.surface);
    ui_hline(r.x, r.y, r.width, theme.border);
    static const char *verbs[] = {"Copying", "Moving", "Deleting", "Moving to the trash", "Changing the permissions of",
                                  "Extracting", "Compressing", "Renaming", "Making", "", "Opening", "Syncing", "Linking"};
    char line[640];
    const char *what = j->items == 1 && j->current[0] ? j->current : NULL;
    if (j->kind == JOB_FETCH) snprintf(line, sizeof line, "Opening %s", j->dest_name);
    else if (j->kind == JOB_RENAME || j->kind == JOB_CREATE) snprintf(line, sizeof line, "%s %s", verbs[j->kind], j->dest_name);
    else if (j->kind <= JOB_MOVE || j->kind == JOB_EXTRACT || j->kind == JOB_COMPRESS || j->kind == JOB_SYNC || j->kind == JOB_LINK) {
        if (what) snprintf(line, sizeof line, "%s %s to %s", verbs[j->kind], what, j->dest_name);
        else snprintf(line, sizeof line, "%s %d items to %s", verbs[j->kind], j->items, j->dest_name);
    } else if (j->kind == JOB_STEPS) snprintf(line, sizeof line, "%s %s", j->what, what ? what : "");
    else if (what) snprintf(line, sizeof line, "%s %s", verbs[j->kind], what);
    else snprintf(line, sizeof line, "%s %d items", verbs[j->kind], j->items);
    if (queued) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof line - l, " (+%d waiting)", queued);
    }
    float frac = j->bytes_total ? (float)((double)j->bytes_done / (double)j->bytes_total)
                 : j->files_total ? (float)j->files_done / (float)j->files_total
                                  : 0;
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(frac * 100));
    float btn = DP(40), pw = ui_text_width(pct, 13, FONT_REGULAR);
    Rectangle text = {r.x + pad, r.y + DP(8), r.width - 2 * pad - btn - DP(8), h / 2 - DP(4)};
    ui_text_box(line, text, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
    Rectangle bar = {r.x + pad, r.y + h * 0.66f, text.width - pw - DP(12), DP(4)};
    ui_progress(bar, frac, theme.accent);
    ui_text_box(pct, (Rectangle){bar.x + bar.width + DP(12), bar.y - DP(8), pw, DP(20)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    if (ui_icon_button(ui_id("job-cancel", j->id), (Rectangle){r.x + r.width - pad - btn, r.y + (h - btn) / 2, btn, btn}, "x", theme.text_dim))
        fr_job_cancel(j->id);
    return h;
}

/* ---- dialogs ---- */

void ops_draw_dialogs(void)
{
    if (dlg == DLG_NONE) return;
    int b;
    switch (dlg) {
    case DLG_RENAME: {
        fr_dialog d = {.title = "Rename", .message = dlg_pending ? "Renaming..." : NULL, .field = &field,
                       .buttons = {"Cancel", "Rename"}, .nbuttons = 2, .cancel = 0};
        b = ui_dialog(&d);
        if (b == 1) rename_commit();
        else if (b == 0) dlg_close();
        break;
    }
    case DLG_NEW_FOLDER: {
        fr_dialog d = {.title = "New file or folder", .message = dlg_pending ? "Making it..." : "End the name with / to make a folder.", .field = &field,
                       .buttons = {"Cancel", "Make"}, .nbuttons = 2, .cancel = 0};
        b = ui_dialog(&d);
        if (b == 1) new_folder_commit();
        else if (b == 0) dlg_close();
        break;
    }
    case DLG_DELETE: {
        fr_dialog d = {.title = dlg_title, .message = dlg_message, .nbuttons = 2, .cancel = 0, .danger = dlg_permanent};
        d.buttons[0] = "Cancel";
        d.buttons[1] = dlg_permanent ? "Delete" : "Move to trash";
        b = ui_dialog(&d);
        if (b == 1) {
            fr_job_submit(dlg_permanent ? JOB_DELETE : JOB_TRASH, &dlg_uris, NULL);
            browser_clear_selection();
            dlg_close();
        } else if (b == 0) dlg_close();
        break;
    }
    case DLG_PROPERTIES:
        if (!props_draw()) dlg_close();
        break;
    case DLG_BULK:
        if (!bulk_rename_draw()) dlg_close();
        break;
    case DLG_OPEN_WITH:
        if (!openwith_draw()) dlg_close();
        break;
    case DLG_ERROR: {
        fr_dialog d = {.title = dlg_title, .message = dlg_message, .buttons = {"Close"}, .nbuttons = 1, .cancel = 0};
        if (ui_dialog(&d) >= 0) dlg_close();
        break;
    }
    default: break;
    }
}
