#ifndef FR_VIEWS_H
#define FR_VIEWS_H
/* Shared by the layouts: views.c (details), views_columns.c and views_grid.c. */
#include "app.h"

void view_row_pressed(int v, int right_click); /* click, double click, Ctrl, Shift, right click, touch */
void view_list_draw(Rectangle r);              /* the current folder as rows, scrolled by br.scroll */
void view_background_input(Rectangle r);       /* a press on empty space in r */
void view_draw_empty(Rectangle r, const char *icon, const char *title, const char *detail, int retry);
int view_draw_folder_state(Rectangle r);       /* draws the error or empty state; 0 when there are items */
void view_draw_name(const char *s, Rectangle r, float size_dp, Color c); /* centred, up to two lines */

void columns_draw(Rectangle r);
void trash_draw(Rectangle r); /* trash:/// in any layout */

void grid_draw(Rectangle r);
int grid_columns_for(float width);
void grid_reveal(int v, float width);
#endif
