#ifndef FR_APP_H
#define FR_APP_H
#include "jobs/jobs.h"
#include "platform.h"
#include "ui.h"
#include "vfs/vfs.h"
#include "vfs/vfs_archive.h"

/* ---- file kinds (filetypes.c) ---- */
typedef enum { KIND_FOLDER, KIND_IMAGE, KIND_VIDEO, KIND_AUDIO, KIND_ARCHIVE, KIND_DOCUMENT, KIND_SHEET, KIND_SLIDES, KIND_CODE, KIND_TEXT, KIND_OTHER } fr_kind;
fr_kind kind_of(const char *name, int is_dir);
const char *kind_icon(fr_kind k);
Color kind_color(fr_kind k);
const char *kind_label(fr_kind k);
const char *mime_of(const char *name, int is_dir); /* by the extension */

/* ---- the folder being shown (browser.c) ---- */
typedef enum { SORT_NAME, SORT_SIZE, SORT_DATE, SORT_KIND, SORT_EXT } fr_sort;

typedef struct {
    fr_entry e;
    fr_kind kind;
    int count; /* folders: items inside, -1 not counted yet, -2 unreadable */
    int selected;
    int placeholder; /* the row of a file or folder being named, not on disk yet */
    double flash_at; /* yanked (copied, cut, path copied) then: the row flashes */
} fr_item;

typedef struct {
    char *uri;
    struct {
        fr_item *v;
        int n, cap;
    } items;
    struct {
        int *v;
        int n, cap;
    } view; /* indices into items, as shown */
    fr_strs back, forward;
    fr_sort sort;
    int desc, show_hidden;
    int filtering;
    fr_text filter;
    fr_scroll scroll;
    int cursor, anchor; /* keyboard row and the start of a Shift range, view indices */
    int keyboard;       /* the cursor moved by key since the last click: show it */
    int nselected;
    char *error;        /* the listing failed */
    int loading;        /* a listing off this machine is on its way (async.h); the items are the last ones until then */
    int results;        /* the items are search results below uri, each named by its path from there */
    int64_t dir_mtime;
    double checked_at;
    char *focus_name;   /* the row the next browser_open() or browser_reload() lands on; it takes it */
} fr_browser;

extern fr_browser br;
void browser_open(const char *uri, int push_history);
void browser_reload(void);
void browser_rebuild_view(void);
void browser_back(void);
void browser_forward(void);
void browser_up(void);
void browser_poll(void); /* reloads when the folder changed on disk */
void browser_select_all(void);
void browser_clear_selection(void);
void browser_set_selected(int item, int on);
fr_strs browser_selected_uris(void);
fr_strs browser_target_uris(void); /* the selection if the focused row is in it, else the focused row */
fr_item *browser_item(int view_index);
void browser_count_some(int first, int last); /* folder item counts for the rows shown */
void browser_add_placeholder(void);           /* an empty row at the top, under the cursor */
void browser_drop_placeholder(void);
/* search results (between frames only): begin empties the list into results of uri's tree, add takes es's
 * entries, leave lists uri again; a reload re-reads the results' entries and drops those gone */
void browser_results_begin(void);
void browser_results_add(fr_entries *es);
void browser_results_leave(void);
void browser_results_renamed(const char *from, const char *to); /* the results under from follow it */

/* ---- the window's state (main.c) ---- */
typedef enum { LAYOUT_DETAILS, LAYOUT_COLUMNS, LAYOUT_GRID } fr_layout;

typedef struct {
    int quit;
    int storage_ok;
    int drawer_open;
    fr_places places;
    double places_at;
    fr_disks disks;
    unsigned disks_serial;
    fr_layout layout;
    unsigned menu_more, menu_row, menu_bg, menu_selection;
    int help_open; /* the keyboard shortcuts sheet */
    double path_flash_at; /* the folder's path was copied then: the breadcrumb flashes */
} fr_app;
extern fr_app app;
void app_zoom(int dir);      /* the interface size: +1 bigger, -1 smaller, 0 the system's */
void app_text_zoom(int dir); /* the text alone, the same way */
/* fn(s, n) now, or once the frame being drawn ends, in the order asked (s is copied): what changes the folder's
 * items, the places, the disks, the favorites or the clipboard goes through here, so a frame never sees the lists
 * it draws change under it, and an fr_item * stays valid until the frame ends */
void app_defer(void (*fn)(const char *s, int n), const char *s, int n);

float app_row_height(void);
int app_wide(void); /* the places stay beside the files */

/* ---- places: the sidebar and the drawer (sidebar.c) ---- */
void places_refresh(int now);
int place_of(const char *uri); /* the place holding uri, -1 none */
void places_draw(Rectangle r);
void disks_poll(void); /* takes the disks' list and finished requests */

/* ---- the disks view (disks_view.c, disks_usage.c): the disk manager on the desktop, a storage overview where the
 * platform has none (Android); both look inside volumes ---- */
int dview_is_open(void);
void dview_open(void);
void dview_close(void);
void dview_draw(Rectangle r); /* in place of the header and the files */
void dview_keys(void);        /* keys_handle's, while it is open */
void dview_poll(void);        /* between frames: the disks, the queue's run, the scan */
int dview_modal(void);        /* a dialog of its own is open */
int dview_disk_done(const fr_disk_done *d); /* 1 when the finished request was the view's own */
void dview_shutdown(void);    /* before the window closes: stops the scan, gives back its read-only mount */

/* ---- the header (header.c) ---- */
float header_height(void);
void header_draw(Rectangle r);
void path_edit_start(void); /* the breadcrumb becomes a field to type a location in */

/* ---- the files in the current layout (views.c) ---- */
void view_draw(Rectangle r);
void view_set_layout(fr_layout layout);
void view_reveal(int v); /* scroll so view row v shows */
int view_grid_columns(void); /* items per row in the grid layout, 1 in the others */
void view_flash(const fr_item *it, Rectangle r); /* the yank flash over a row or tile */
fr_item *view_item_at(Vector2 p); /* the row or tile the last frame drew under p, NULL none */
void browser_flash_targets(void); /* flashes the selected rows, else the cursor row */

/* ---- image thumbnails (thumbnails.c) ---- */
void thumbs_start(void);
void thumbs_stop(void); /* before the window closes */
void thumbs_poll(void); /* between frames: asks for what the frame lacked, uploads what is ready */
int thumbs_busy(void);
/* its thumbnail fitted in box, aspect kept: 1 drawn, 0 not ready yet, -1 it has none */
int thumb_draw(const fr_item *it, Rectangle box);

/* ---- settings.toml, favorites, theme overrides (settings.c) ---- */
typedef struct {
    fr_strs favorites; /* URIs, in the order shown */
    fr_strs servers;   /* the Network section's URIs (never with a password), in the order shown */
    char *theme[THEME_ROLES]; /* [theme] values as written, NULL for roles not set */
    char *last_folder, *start_folder; /* URIs, NULL when not set */
} fr_settings;
extern fr_settings settings;
#define UI_SCALE_MIN 0.5
#define UI_SCALE_MAX 2.0
#define TEXT_SCALE_MIN 0.7
#define TEXT_SCALE_MAX 2.0
void settings_load(void);
void settings_poll(void); /* saves what changed (sort, layout, hidden, favorites); reloads after a hand edit */
char *settings_start_uri(void); /* start_folder, else last_folder, when it is a folder that is there; else NULL */
int favorite_index(const char *uri); /* -1 when not a favorite */
void favorite_toggle(const char *uri);
int server_index(const char *uri); /* -1 when not in the Network section */
void server_set(const char *uri, int on); /* adds it to the Network section, or takes it out */

/* ---- network locations, for any scheme with a backend (connect.c) ---- */
/* an address as typed made one to save and open: normalized, any password dropped; NULL when it is not one */
char *server_uri(const char *typed);
/* the Network section of the sidebar from y down: the saved servers and "Connect to server"; the y after it */
float network_draw(Rectangle r, float y, float ih);
void connect_start(void); /* the "Connect to server" dialog */
/* window thread, before a remote location is used: what the keystore has for its server, into memory */
void net_prepare(const char *uri);
/* a remote call on uri failed with rc: asks for a sign-in or the server's trust when that is what it wants (1),
 * then retry() runs once the user answered yes */
int net_ask(const char *uri, int rc, void (*retry)(void));
int connect_dialog_open(void);
void connect_draw_dialogs(void);

/* ---- keys (keys.c) ---- */
typedef enum { MODE_NORMAL, MODE_VISUAL, MODE_INSERT } fr_mode;
void keys_handle(void);
const char *keys_pending(void); /* the chord prefix waiting for its next key, "" none */
int keys_mode(void);            /* an fr_mode */
int keys_insert_field(int v, Rectangle r);
void keys_new_start(const char *typed); /* a new row at the top to name a file, or a folder with a trailing "/" */ /* draws view row v's name being typed in r; 0 when v is not renamed in place */
void filter_start(int first_char);
void filter_stop(void);
int step_back(int quit_at_end);
void move_cursor(int to, int extend);

/* ---- actions (ops.c) ---- */
typedef struct {
    fr_strs uris;
    int cut;
} fr_clip;
extern fr_clip clip;

void ops_open_item(fr_item *it);
void ops_open_entry(const char *uri, const fr_entry *e); /* a folder or an archive is browsed, a file handed on */
int ops_rename(const char *from, const char *to);       /* inside an archive as a job (0 once it is asked for) */
int ops_archive_targets(void);  /* how many targets there are when all are archives to extract, else 0 */
void ops_extract(int into_folder); /* the targets here, or each into a folder of its own named after it */
char *ops_compress_name(void);  /* "<name>.zip" for the targets, NULL without any */
void ops_compress(void);        /* the targets into a new zip here */
void ops_bulk_rename(int as_text); /* the targets' names in an editor, or find and replace across them */
void ops_copy(int cut);
void ops_paste(void);
void ops_clip_clear(void);
void ops_delete(int permanently);
void ops_rename_start(void);
void ops_new_folder_start(void);
void ops_new_start(void); /* a file, or a folder when the name ends in "/" */
int ops_create(const char *typed); /* makes it in the current folder; 0, or an error already shown */
void ops_properties(void);
void ops_share(void); /* the targets to the system's share sheet (Android) */
void ops_draw_dialogs(void);
int ops_dialog_open(void); /* also a job's question about a name taken */
float ops_draw_jobs(Rectangle area); /* the job strip at the bottom of area; returns its height */
void ops_poll_jobs(void);

/* ---- searching below the folder (search.c) ---- */
void search_prompt(int content); /* a field in the header for a name (content 0) or the text inside files */
int search_header(Rectangle r);  /* draws the field or the results' bar; 0 when neither shows */
void search_poll(void);          /* between frames: the results found meanwhile into the list */
int search_running(void);
void search_reveal(void);        /* the cursor row's result, shown in its folder */
void search_stop(void);          /* before the window closes */

/* ---- renaming the targets together (bulk_rename.c), one of ops.c's dialogs ---- */
int bulk_rename_open(int as_text); /* as_text: the names in an editor; else find and replace. 0 when nothing to rename */
int bulk_rename_draw(void);        /* 0 once it closed itself */
void bulk_rename_close(void);

/* ---- undo and redo of what was done to files (undo.c); kept until the window closes ---- */
void undo_record(const char *label, fr_steps *steps); /* takes the steps */
void undo_record_rename(const char *from, const char *to);
void undo_record_created(const char *uri, int folder);
/* a finished job: what it did, recorded (an undo or redo job toasts its own outcome); 1 when it toasted */
int undo_job_done(const fr_job_status *st);
void undo_run(int redo);
int undo_can(int redo);

/* ---- ferret --replay SCRIPT [FOLDER] (replay.c): a script's input between frames, checked as it goes ---- */
int replay_load(const char *script, const char *folder); /* 0, or -1 after saying why; relative paths are FOLDER's */
int replay_frame(void);  /* once a frame, after its input was handled: 0 when the script is done */
void replay_capture(void); /* before the drawn frame is shown: takes a pending shot */
int replay_status(void); /* the exit status: 0 when every expectation held */

/* ---- the properties panel (properties.c), one of ops.c's dialogs ---- */
void props_open(fr_strs *uris); /* takes the URIs */
int props_draw(void);           /* 0 once it closed itself */
void props_close(void);

/* ---- open with (openwith.c), one of ops.c's dialogs ---- */
int openwith_open(void); /* the targets' apps; 0 when no dialog is needed (handed to the system, or said why) */
int openwith_draw(void); /* 0 once it closed itself */
void openwith_close(void);
void ops_open_with(void);
void ops_terminal_here(void);
/* asks before deleting uris (taken) for good, with this title and message */
void ops_delete_uris(fr_strs *uris, const char *title, const char *message);

/* ---- trash:/// (views_trash.c) ---- */
void trash_draw_menus(void);
void trash_restore(void); /* the targets, back where they came from */
void trash_empty(void);   /* asks first */

/* ---- files dropped on the window (drop.c), between frames ---- */
void drop_poll(void);
#endif
