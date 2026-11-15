#ifndef FR_UI_H
#define FR_UI_H
/* An immediate-mode UI on raylib: each frame draws everything and handles the input that frame took. Sizes are
 * in dp (DP() turns them into pixels); positions are pixels. */
#include "core/util.h"
#include "raylib.h"

#define DP(x) ((x) * ui.scale)

typedef struct {
    Color bg, surface, surface_hi, border, text, text_dim, text_faint, accent, on_accent, danger, warning, success;
    Color hover, pressed, selection, scrim;
    Color folder, image, video, audio, archive, document, sheet, slides, code, other;
} fr_theme;
extern fr_theme theme;
/* the colour roles of the desktop's colors.toml: accent, background, dark_background, ... */
enum { THEME_ROLES = 13 };
extern const char *const theme_roles[THEME_ROLES];
/* Nord, then the desktop's colours when it publishes them, then overrides ("#rrggbb" per role, NULL keeps;
 * overrides itself may be NULL) */
void theme_load(char *const *overrides);

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8 };

typedef struct {
    float scale;      /* pixels per dp */
    float user_scale; /* the interface size chosen in the app, over the system's scale (1 = as the system says) */
    float text_scale; /* text alone, over the interface size (1 = as designed) */
    int touch;
    float w, h;              /* window, pixels */
    float il, it, ir, ib;    /* insets, pixels */
    Rectangle safe;          /* the window minus the insets */
    Vector2 mouse;
    int pressed, released, down, rpressed, mpressed;
    int clicks; /* of the last press: 2 for a double click's second, kept until the next press */
    Vector2 wheel;
    struct {
        int key, mods, used;
    } keys[64];
    int nkeys;
    int chars[64], nchars;
    double now, dt;
    int dirty;      /* draw another frame right away */
    double redraw_at; /* or at this time (0 = no need) */
    unsigned active;  /* the widget the pointer went down on */
    unsigned focus;   /* the text field taking typed keys */
    unsigned id_salt; /* mixed into every ui_id: the same widgets drawn twice (two panes) keep apart; 0 none */
    int layer, top;   /* the layer being drawn; the topmost open one, from the last frame */
    int top_next;
} fr_ui;
extern fr_ui ui;

/* ---- frames and input (input.c) ---- */
void ui_input_init(void);
void ui_push_mouse(int button, int down); /* raylib MOUSE_BUTTON_* */
void ui_push_key(int key, int mods);      /* raylib KEY_*, presses and repeats */
void ui_push_char(int codepoint);         /* 8 = backspace, 127 = delete (soft keyboards) */
void ui_push_wheel(float dx, float dy);   /* in notches; + is towards the top/left */
int ui_events_pending(void);
int ui_input_since_frame(void);
void ui_begin_frame(void);
void ui_end_frame(void);
void ui_redraw_in(double seconds);

/* a key pressed this frame with exactly these modifiers, not taken by a widget; taking it */
int ui_key(int key, int mods);
int ui_key_any_mods(int key);
/* input reaches only the topmost layer: menus and dialogs draw in layer 1 */
void ui_layer(int layer);
int ui_live(void); /* this layer gets input */
int ui_hover(Rectangle r);
unsigned ui_id(const char *name, int index);
/* press inside, release inside: 1. *state gets 1 hovered, 2 held */
int ui_clickable(unsigned id, Rectangle r, int *state);
/* where ui_clickable(id) was this frame, the last one with that id; 0 when it was not */
int ui_clickable_rect(unsigned id, Rectangle *out);

/* ---- drawing (draw.c) ---- */
enum { FONT_REGULAR, FONT_SEMIBOLD };
void ui_fonts_reload(void);
Font ui_font(int weight, float size_dp);
float ui_text_width(const char *s, float size_dp, int weight);
float ui_text_width_n(const char *s, int n, float size_dp, int weight);
void ui_text(const char *s, float x, float y, float size_dp, int weight, Color c);
/* one line in a box, vertically centred; too wide: cut at the end ("..."), or in the middle keeping the end */
void ui_text_box(const char *s, Rectangle r, float size_dp, int weight, Color c, int align, int middle);
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
/* wraps s to width; returns the lines' count, and draws them when draw */
int ui_text_wrapped(const char *s, Rectangle r, float size_dp, int weight, Color c, int draw);
float ui_line_height(float size_dp);
void ui_rect(Rectangle r, Color c);
void ui_round(Rectangle r, float radius_dp, Color c);
void ui_round_line(Rectangle r, float radius_dp, Color c);
void ui_hline(float x, float y, float w, Color c);
Color ui_mix(Color a, Color b, float t);
Color ui_alpha(Color c, float a);
void ui_clip_begin(Rectangle r);
void ui_clip_end(void);

/* ---- icons (icons.c): Lucide SVGs at any size ---- */
void ui_icon(const char *name, Rectangle box, float size_dp, Color c); /* centred in box */
void ui_icons_clear(void);

/* ---- widgets (widgets.c) ---- */
int ui_icon_button(unsigned id, Rectangle r, const char *icon, Color c);
int ui_button(unsigned id, Rectangle r, const char *label, int primary);
float ui_button_width(const char *label);

typedef struct {
    char buf[1024];
    int len, caret, anchor; /* selection: between anchor and caret */
    float scroll;
    int secret; /* shown as dots, and never copied out (a password) */
} fr_text;
void text_set(fr_text *t, const char *s);
void text_select(fr_text *t, int from, int to);
enum { TEXT_CHANGED = 1, TEXT_SUBMIT = 2, TEXT_CANCEL = 4 };
int ui_text_field(unsigned id, fr_text *t, Rectangle r, const char *placeholder);

/* several lines of text, growing as typed; Enter breaks the line */
typedef struct {
    char *buf;
    int len, cap, caret, anchor;
    float scroll_x, scroll_y;
} fr_textarea;
void textarea_set(fr_textarea *t, const char *s);
void textarea_free(fr_textarea *t);
/* TEXT_CHANGED, TEXT_SUBMIT for Ctrl+Enter, TEXT_CANCEL for Esc */
int ui_text_area(unsigned id, fr_textarea *t, Rectangle r);

typedef struct {
    float offset, target, content, view;
    int bar_drag;
    float bar_grab;
    double moved_at;
} fr_scroll;
/* wheel, smoothing, clamping and the scroll bar for a view r over content pixels */
void ui_scroll(unsigned id, fr_scroll *s, Rectangle r, float content);
void ui_scroll_to(fr_scroll *s, float top, float bottom); /* bring [top, bottom] into view */

/* menus: ui_menu_open() at an anchor, then each frame ui_menu_begin() ... items ... ui_menu_end() */
void ui_menu_open(unsigned id, Rectangle anchor);
int ui_menu_begin(unsigned id);
int ui_menu_item(const char *label, const char *icon, int checked); /* checked: -1 none, 0/1 a tick */
/* "label   -  value  +" on one row; the menu stays open. -1 / 1 for - / +, 2 for a tap on the value, else 0 */
int ui_menu_stepper(const char *label, const char *icon, const char *value);
void ui_menu_separator(void);
/* a small title over the items after it, with an optional button at its right: 1 when that was pressed */
int ui_menu_heading(const char *label, const char *button_icon, const char *button_label);
/* one row of n choices, icon over label, `selected` lit; the index picked, else -1; the menu stays open */
int ui_menu_segments(const char *const *labels, const char *const *icons, int n, int selected);
void ui_menu_end(void);
int ui_menu_is_open(void);
void ui_menu_close(void);

typedef struct {
    const char *title, *message;
    fr_text *field;             /* a text field, focused */
    fr_text *field2;            /* a second one under it; Tab and Enter move on to it (focused when field has text) */
    const char *hint, *hint2;   /* the fields' placeholders */
    const char *check;          /* a checkbox label */
    int *checked;
    const char *buttons[4];     /* left to right; the last is the main one */
    int nbuttons, cancel, danger; /* cancel: the button Escape/Back means; danger: the main one is red */
} fr_dialog;
/* draws it in layer 1; the index of the button chosen this frame, or -1 */
int ui_dialog(const fr_dialog *d);

void ui_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_toast_draw(float bottom);
const char *ui_toast_shown(void); /* the toast showing now, "" none */

void ui_progress(Rectangle r, float fraction, Color c);
#endif
