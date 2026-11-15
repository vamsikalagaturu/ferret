#include "app.h"
#include "vfs/trash.h"
#include <stdlib.h>
#include <string.h>

/* Files dropped from another app are copied (moved with Shift held) into the folder shown, or into the folder row
 * they were dropped on; in the split view, the pane's they were dropped on. */
void drop_poll(void)
{
    if (!IsFileDropped()) return;
    FilePathList dropped = LoadDroppedFiles();
    fr_strs uris = {0};
    for (unsigned i = 0; i < dropped.count; i++)
        if (dropped.paths[i][0] == '/') FR_PUSH(uris, fr_uri_from_path(dropped.paths[i]));
    UnloadDroppedFiles(dropped);
    Vector2 at = plat_mouse();
    if (app_split() && CheckCollisionPointRec(at, app.pane_rect[!tabs.v[tabs.cur].side])) browser_enter(tabs_other(&tabs));
    fr_item *it = br.uri && !ops_dialog_open() ? view_item_at(at) : NULL;
    char *dest = it && it->e.is_dir && !it->placeholder ? fr_uri_join(br.uri, it->e.name) : br.uri ? fr_strdup(br.uri) : NULL;
    int move = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (uris.n && dest && fr_trash_is_root(dest)) ui_toast("Drop files on a folder; Delete moves them to the trash");
    else if (uris.n && dest) fr_job_submit(move ? JOB_MOVE : JOB_COPY, &uris, dest);
    free(dest);
    fr_strs_free(&uris);
    tabs_sync();
    ui.dirty = 1;
}
