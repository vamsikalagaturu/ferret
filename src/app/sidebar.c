#include "app.h"
#include <string.h>

void places_refresh(int now)
{
    if (!now && ui.now - app.places_at < 3) return;
    app.places_at = ui.now;
    fr_places_free(&app.places);
    plat_places(&app.places);
}

int place_of(const char *uri)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < app.places.n; i++) {
        size_t n = strlen(app.places.v[i].uri);
        if (fr_uri_within(app.places.v[i].uri, uri) && n > best_len) best = i, best_len = n;
    }
    return best;
}

void places_draw(Rectangle r)
{
    ui_rect(r, theme.surface);
    float y = r.y + DP(12), ih = DP(ui.touch ? 52 : 38);
    ui_text_box("ferret", (Rectangle){r.x + DP(20), y, r.width - DP(40), DP(28)}, 20, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
    ui_text_box(FR_VERSION, (Rectangle){r.x + DP(20), y + DP(28), r.width - DP(40), DP(16)}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 1);
    y += DP(56);
    fr_places *p = &app.places;
    int current = br.uri ? place_of(br.uri) : -1;
    for (int i = 0; i < p->n; i++) {
        if (i && p->v[i].removable && !p->v[i - 1].removable) {
            ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
            y += DP(12);
        }
        Rectangle row = {r.x + DP(8), y, r.width - DP(16), ih};
        int state;
        if (ui_clickable(ui_id("place", i), row, &state)) {
            browser_open(p->v[i].uri, 1);
            app.drawer_open = 0;
        }
        if (i == current) ui_round(row, 8, theme.selection);
        else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        Color c = i == current ? theme.accent : theme.text_dim;
        ui_icon(p->v[i].icon, (Rectangle){row.x + DP(8), y, DP(32), ih}, 20, c);
        ui_text_box(p->v[i].label, (Rectangle){row.x + DP(52), y, row.width - DP(60), ih}, 15,
                    i == current ? FONT_SEMIBOLD : FONT_REGULAR, i == current ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        y += ih;
    }
}
