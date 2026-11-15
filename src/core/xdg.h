#ifndef FR_XDG_H
#define FR_XDG_H
/* The text formats behind "Open with": desktop entries and their Exec lines (Desktop Entry Specification 1.5),
 * mimeapps.list (MIME Applications Associations 1.0.1) and the shared-mime-info aliases and subclasses files.
 * Only parsing and rewriting; platform/linux/apps_linux.c finds the files and starts the apps. */
#include "util.h"

typedef struct {
    char *name, *exec, *try_exec, *icon;
    fr_strs mime_types;
    int application; /* Type=Application */
    int terminal, hidden, no_display;
} fr_desktop_entry;

/* the [Desktop Entry] group of a .desktop file; 0, or -1 when it has none */
int fr_desktop_parse(const char *text, fr_desktop_entry *out);
void fr_desktop_entry_free(fr_desktop_entry *e);

/* an Exec value (already unescaped as a string) into its arguments, quotes removed; 0, or -1 when a quote is open */
int fr_exec_split(const char *exec, fr_strs *out);

typedef struct {
    fr_strs *v;
    int n, cap;
} fr_argvs;
void fr_argvs_free(fr_argvs *a);
/* The command lines that open files with an Exec value: one for all of them with %F or %U, else one per file
 * (%f, %u, or the file appended when the line names none), one without files when there are none. %c is name,
 * %i "--icon icon", %k location, %% a percent; deprecated codes go. Files are paths. 0, or -1 when malformed. */
int fr_exec_expand(const char *exec, const fr_strs *files, const char *name, const char *icon, const char *location,
                   fr_argvs *out);

/* the desktop IDs a group of mimeapps.list gives for mime, appended in order */
void fr_mimeapps_get(const char *text, const char *group, const char *mime, fr_strs *out);
/* text (NULL for none) with "mime=id" as the [Default Applications] entry, everything else kept; malloc'd */
char *fr_mimeapps_set_default(const char *text, const char *mime, const char *id);

/* the canonical name of mime by an aliases file ("alias canonical" lines); malloc'd */
char *fr_mime_unalias(const char *aliases, const char *mime);
/* mime, then what it is a kind of by a subclasses file ("child parent" lines), nearest first; text/plain for
 * any text type */
void fr_mime_ancestors(const char *subclasses, const char *mime, fr_strs *out);
#endif
