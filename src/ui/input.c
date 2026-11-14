/* The window sleeps between events, so a click can arrive whole (press and release) before the next frame and
 * raylib's per-frame button states would miss it. Every event is queued instead; a frame takes at most one mouse
 * button event, and the rest wait for the frames after it. */
#include "platform.h"
#include "ui.h"
#include <math.h>
#include <string.h>

fr_ui ui;

typedef struct {
    enum { EV_MOUSE, EV_KEY, EV_CHAR, EV_WHEEL } type;
    int a, b;
    float x, y;
} event;

static event queue[256];
static int nqueue;
static Vector2 drawn_mouse;
static int left_down;
static double last_press_t;
static Vector2 last_press_at;
static struct {
    unsigned id;
    Rectangle r;
} clickables[1024]; /* what ui_clickable() got this frame, for ferret --replay to aim at */
static int nclickables;

static void push(event e)
{
    if (nqueue < (int)(sizeof queue / sizeof *queue)) queue[nqueue++] = e;
    ui.dirty = 1;
}

void ui_push_mouse(int button, int down) { push((event){EV_MOUSE, button, down, 0, 0}); }
void ui_push_key(int key, int mods) { push((event){EV_KEY, key, mods, 0, 0}); }
void ui_push_char(int codepoint) { push((event){EV_CHAR, codepoint, 0, 0, 0}); }
void ui_push_wheel(float dx, float dy) { push((event){EV_WHEEL, 0, 0, dx, dy}); }

#ifndef __ANDROID__
typedef void (*glfw_mouse_fn)(void *window, int button, int action, int mods);
typedef void (*glfw_key_fn)(void *window, int key, int scancode, int action, int mods);
typedef void (*glfw_char_fn)(void *window, unsigned int codepoint);
typedef void (*glfw_scroll_fn)(void *window, double x, double y);
glfw_mouse_fn glfwSetMouseButtonCallback(void *window, glfw_mouse_fn fn);
glfw_key_fn glfwSetKeyCallback(void *window, glfw_key_fn fn);
glfw_char_fn glfwSetCharCallback(void *window, glfw_char_fn fn);
glfw_scroll_fn glfwSetScrollCallback(void *window, glfw_scroll_fn fn);
void *glfwGetCurrentContext(void);

static glfw_mouse_fn raylib_mouse;
static glfw_key_fn raylib_key;
static glfw_char_fn raylib_char;
static glfw_scroll_fn raylib_scroll;

static void on_mouse(void *w, int button, int action, int mods)
{
    if (raylib_mouse) raylib_mouse(w, button, action, mods);
    ui_push_mouse(button, action == 1);
}

static void on_key(void *w, int key, int scancode, int action, int mods)
{
    if (raylib_key) raylib_key(w, key, scancode, action, mods);
    if (action != 0) ui_push_key(key, mods & 15);
}

static void on_char(void *w, unsigned int cp)
{
    if (raylib_char) raylib_char(w, cp);
    ui_push_char((int)cp);
}

static void on_scroll(void *w, double x, double y)
{
    if (raylib_scroll) raylib_scroll(w, x, y);
    ui_push_wheel((float)x, (float)y);
}
#endif

void ui_input_init(void)
{
#ifndef __ANDROID__
    void *w = glfwGetCurrentContext();
    raylib_mouse = glfwSetMouseButtonCallback(w, on_mouse);
    raylib_key = glfwSetKeyCallback(w, on_key);
    raylib_char = glfwSetCharCallback(w, on_char);
    raylib_scroll = glfwSetScrollCallback(w, on_scroll);
#endif
}

int ui_events_pending(void) { return nqueue > 0; }

int ui_input_since_frame(void)
{
    Vector2 m = plat_mouse();
    return nqueue > 0 || m.x != drawn_mouse.x || m.y != drawn_mouse.y;
}

void ui_redraw_in(double seconds)
{
    double t = ui.now + seconds;
    if (!ui.redraw_at || t < ui.redraw_at) ui.redraw_at = t;
}

void ui_begin_frame(void)
{
    double now = fr_now();
    ui.dt = ui.now ? fmin(fmax(now - ui.now, 0), 0.05) : 0;
    ui.now = now;
    ui.scale = plat_scale() * (ui.user_scale > 0 ? ui.user_scale : 1);
    ui.touch = plat_touch();
    ui.w = (float)GetRenderWidth();
    ui.h = (float)GetRenderHeight();
    plat_insets(&ui.il, &ui.it, &ui.ir, &ui.ib);
    ui.safe = (Rectangle){ui.il, ui.it, fmaxf(0, ui.w - ui.il - ui.ir), fmaxf(0, ui.h - ui.it - ui.ib)};
    ui.pressed = ui.released = ui.rpressed = ui.mpressed = 0;
    ui.wheel = (Vector2){0, 0};
    ui.nkeys = ui.nchars = 0;
    ui.top = ui.top_next;
    ui.top_next = 0;
    ui.layer = 0;
    ui.dirty = 0;
    ui.redraw_at = 0;
    nclickables = 0;
    ui.mouse = drawn_mouse = plat_mouse();

    int k = 0, took_mouse = 0;
    for (; k < nqueue; k++) {
        event *e = &queue[k];
        if (e->type == EV_MOUSE) {
            if (took_mouse) break;
            took_mouse = 1;
            if (e->a == MOUSE_BUTTON_LEFT) {
                if (e->b) {
                    ui.pressed = 1, left_down = 1;
                    int again = ui.now - last_press_t < 0.4 && fabsf(ui.mouse.x - last_press_at.x) < DP(8) &&
                                fabsf(ui.mouse.y - last_press_at.y) < DP(8);
                    ui.clicks = again ? 2 : 1;
                    last_press_t = again ? 0 : ui.now;
                    last_press_at = ui.mouse;
                } else ui.released = 1, left_down = 0;
            } else if (e->a == MOUSE_BUTTON_RIGHT && e->b) ui.rpressed = 1;
            else if (e->a == MOUSE_BUTTON_MIDDLE && e->b) ui.mpressed = 1;
        } else if (e->type == EV_KEY) {
            if (ui.nkeys < 64) ui.keys[ui.nkeys].key = e->a, ui.keys[ui.nkeys].mods = e->b, ui.keys[ui.nkeys++].used = 0;
        } else if (e->type == EV_CHAR) {
            if (ui.nchars < 64) ui.chars[ui.nchars++] = e->a;
        } else ui.wheel.x += e->x, ui.wheel.y += e->y;
    }
    memmove(queue, queue + k, sizeof *queue * (size_t)(nqueue - k));
    nqueue -= k;
    if (nqueue) ui.dirty = 1;
    ui.down = left_down;
    if (!ui.down && !ui.released) ui.active = 0;
}

void ui_end_frame(void)
{
    if (ui.released) ui.active = 0;
}

int ui_key(int key, int mods)
{
    for (int i = 0; i < ui.nkeys; i++)
        if (!ui.keys[i].used && ui.keys[i].key == key && ui.keys[i].mods == mods) return ui.keys[i].used = 1;
    return 0;
}

int ui_key_any_mods(int key)
{
    for (int i = 0; i < ui.nkeys; i++)
        if (!ui.keys[i].used && ui.keys[i].key == key) return ui.keys[i].used = 1;
    return 0;
}

void ui_layer(int layer)
{
    ui.layer = layer;
    if (layer > ui.top_next) ui.top_next = layer;
}

int ui_live(void) { return ui.layer >= ui.top; }

int ui_hover(Rectangle r) { return ui_live() && CheckCollisionPointRec(ui.mouse, r); }

unsigned ui_id(const char *name, int index)
{
    unsigned h = 2166136261u;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) h = (h ^ *c) * 16777619u;
    h = (h ^ (unsigned)index) * 16777619u;
    return h ? h : 1;
}

int ui_clickable_rect(unsigned id, Rectangle *out)
{
    for (int i = nclickables - 1; i >= 0; i--)
        if (clickables[i].id == id && clickables[i].r.width > 0 && clickables[i].r.height > 0) return *out = clickables[i].r, 1;
    return 0;
}

int ui_clickable(unsigned id, Rectangle r, int *state)
{
    if (nclickables < (int)(sizeof clickables / sizeof *clickables)) clickables[nclickables].id = id, clickables[nclickables++].r = r;
    int over = ui_hover(r), clicked = 0;
    if (over && ui.pressed) ui.active = id;
    if (ui.active == id && ui.released) clicked = over;
    if (state) *state = ui.active == id && ui.down ? 2 : over && !ui.touch ? 1 : 0;
    return clicked;
}
