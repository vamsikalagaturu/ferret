/* The Android side of platform.h, on raylib's PLATFORM_ANDROID: the process entry raylib's android_main() calls,
 * the environment the shared sources expect (HOME, XDG_*), touch, mouse and keys fed to input.c's queue, the soft
 * keyboard, and what FerretActivity.java does in Java. Everything runs on the android_main thread except
 * plat_wake() and native_text(), which the soft keyboard calls on the UI thread. */
#include "platform.h"
#include "ui.h"
#include "vfs/vfs.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ferret", __VA_ARGS__)

struct android_app *GetAndroidApp(void); /* rcore_android.c; not declared in raylib.h */

/* ---- JNI ---- */

/* raylib attaches and detaches this thread around its own JNI calls, so the env is looked up every time */
static JNIEnv *jni(void)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) (*vm)->AttachCurrentThread(vm, &env, NULL);
    return env;
}

static int jfailed(JNIEnv *e)
{
    if (!(*e)->ExceptionCheck(e)) return 0;
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
    return 1;
}

static jobject call_obj(JNIEnv *e, jobject o, const char *name, const char *sig, ...)
{
    if (!o) return NULL;
    jclass c = (*e)->GetObjectClass(e, o);
    jmethodID m = (*e)->GetMethodID(e, c, name, sig);
    (*e)->DeleteLocalRef(e, c);
    if (jfailed(e) || !m) return NULL;
    va_list ap;
    va_start(ap, sig);
    jobject r = (*e)->CallObjectMethodV(e, o, m, ap);
    va_end(ap);
    return jfailed(e) ? NULL : r;
}

static char *jstr(JNIEnv *e, jobject s)
{
    if (!s) return NULL;
    const char *u = (*e)->GetStringUTFChars(e, (jstring)s, NULL);
    char *r = u ? fr_strdup(u) : NULL;
    if (u) (*e)->ReleaseStringUTFChars(e, (jstring)s, u);
    return r;
}

/* FerretActivity.<method>(String x nargs) returning String, nargs at most 2; malloc'd, NULL for Java null or an
 * exception */
static char *java(const char *method, int nargs, ...)
{
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 16);
    char sig[96] = "(";
    jvalue args[2] = {0};
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 2; i++) {
        const char *s = va_arg(ap, const char *);
        args[i].l = s ? (*e)->NewStringUTF(e, s) : NULL;
        strcat(sig, "Ljava/lang/String;");
    }
    va_end(ap);
    strcat(sig, ")Ljava/lang/String;");
    jobject act = GetAndroidApp()->activity->clazz;
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = (*e)->GetMethodID(e, c, method, sig);
    char *r = NULL;
    if (!jfailed(e) && m) {
        jobject s = (*e)->CallObjectMethodA(e, act, m, args);
        if (!jfailed(e)) r = jstr(e, s);
    }
    (*e)->PopLocalFrame(e, NULL);
    return r;
}

/* ---- environment ---- */

static void setup_environment(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *e = jni();
    (*e)->PushLocalFrame(e, 16);
    char *cache = jstr(e, call_obj(e, call_obj(e, act->clazz, "getCacheDir", "()Ljava/io/File;"), "getAbsolutePath",
                                   "()Ljava/lang/String;"));
    (*e)->PopLocalFrame(e, NULL);
    const char *files = act->internalDataPath;
    if (!cache) cache = fr_strdup(files);

    char *config = fr_format("%s/.config", files), *data = fr_format("%s/.local/share", files);
    char *run = fr_format("%s/run", cache);
    fr_mkdirs(config);
    fr_mkdirs(data);
    fr_mkdirs(run);
    chmod(run, 0700);
    setenv("HOME", files, 1);
    setenv("XDG_CONFIG_HOME", config, 1);
    setenv("XDG_DATA_HOME", data, 1);
    setenv("XDG_RUNTIME_DIR", run, 1);
    setenv("TMPDIR", cache, 1);
    if (chdir(files)) LOG("chdir %s failed", files);
    free(config), free(data), free(run), free(cache);
}

/* ---- touch ---- */

#define LONG_PRESS_S 0.4 /* ViewConfiguration.getLongPressTimeout() */
#define SLOP_DP 8.0f     /* ViewConfiguration's touch slop */
#define NOTCH_DP 60.0f   /* finger travel per wheel notch */
#define FLING_MIN_DP 100.0f  /* per second, at release */
#define FLING_MAX_DP 3500.0f /* per second */
#define FLING_DECAY 2.6f     /* per second */
#define FLING_STOP_DP 40.0f  /* per second */

static struct {
    int pending;   /* down, not yet a tap, scroll or long press */
    int scrolling;
    int held;      /* the long press fired: the rest of this touch does nothing */
    int rebase;    /* finger count changed: the centroid jumps, so it restarts there */
    int lifted;    /* up; the pointer leaves once its clicks are taken */
    double t;
    float x, y, cx, cy;
} touch;

static struct {
    float vx, vy;
} fling;

/* recent finger samples (event time s, centroid px), batched ones included, for the release velocity */
typedef struct {
    double t;
    float x, y;
} sample;

static sample samples[32];
static int nsamples;

static float dp(void) { return plat_scale(); }

static void centroid(const AInputEvent *e, float *x, float *y)
{
    int n = (int)AMotionEvent_getPointerCount(e);
    *x = *y = 0;
    for (int i = 0; i < n; i++) *x += AMotionEvent_getX(e, (size_t)i), *y += AMotionEvent_getY(e, (size_t)i);
    if (n) *x /= n, *y /= n;
}

static void add_samples(const AInputEvent *e)
{
    int n = (int)AMotionEvent_getPointerCount(e), hist = (int)AMotionEvent_getHistorySize(e);
    for (int h = 0; h <= hist; h++) {
        float x = 0, y = 0;
        for (int i = 0; i < n; i++) {
            x += h < hist ? AMotionEvent_getHistoricalX(e, (size_t)i, (size_t)h) : AMotionEvent_getX(e, (size_t)i);
            y += h < hist ? AMotionEvent_getHistoricalY(e, (size_t)i, (size_t)h) : AMotionEvent_getY(e, (size_t)i);
        }
        double t = (h < hist ? AMotionEvent_getHistoricalEventTime(e, (size_t)h) : AMotionEvent_getEventTime(e)) / 1e9;
        if (nsamples == 32) memmove(samples, samples + 1, sizeof samples[0] * 31), nsamples--;
        samples[nsamples++] = (sample){t, x / n, y / n};
    }
}

/* px/s over the last 100 ms of samples; none if the finger rested before lifting */
static int release_velocity(double up_t, float *vx, float *vy)
{
    if (nsamples < 2 || up_t - samples[nsamples - 1].t > 0.05) return 0;
    int first = nsamples - 1;
    while (first > 0 && samples[nsamples - 1].t - samples[first - 1].t <= 0.1) first--;
    double dt = samples[nsamples - 1].t - samples[first].t;
    if (dt < 0.005) return 0;
    *vx = (float)((samples[nsamples - 1].x - samples[first].x) / dt);
    *vy = (float)((samples[nsamples - 1].y - samples[first].y) / dt);
    return 1;
}

static void scroll_by(float dx, float dy)
{
    if (dx || dy) ui_push_wheel(dx / (NOTCH_DP * dp()), dy / (NOTCH_DP * dp()));
}

/* A tap clicks when the finger lifts, a moving finger (or two) scrolls and flings when let go, a finger held still
 * right-clicks when the long press time is reached (plat_frame). */
static void on_touch(const AInputEvent *e, int action)
{
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        touch.pending = 1, touch.scrolling = touch.held = touch.lifted = touch.rebase = 0;
        touch.t = fr_now(), touch.x = touch.cx = x, touch.y = touch.cy = y;
        nsamples = 0;
        add_samples(e);
        /* a finger on a moving list stops it, and that touch is not a tap */
        if (fling.vx || fling.vy) touch.pending = 0, touch.scrolling = 1;
        fling.vx = fling.vy = 0;
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (touch.held) break;
        touch.pending = 0;
        touch.scrolling = touch.rebase = 1;
        nsamples = 0;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        if (touch.held) break;
        if (touch.rebase) nsamples = 0;
        add_samples(e);
        if (touch.pending && hypotf(x - touch.x, y - touch.y) > SLOP_DP * dp()) touch.pending = 0, touch.scrolling = 1;
        if (touch.scrolling) {
            float cx, cy;
            centroid(e, &cx, &cy);
            if (!touch.rebase) scroll_by(cx - touch.cx, cy - touch.cy);
            touch.cx = cx, touch.cy = cy, touch.rebase = 0;
        }
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (action == AMOTION_EVENT_ACTION_UP && !touch.held) {
            add_samples(e);
            if (touch.pending) ui_push_mouse(MOUSE_BUTTON_LEFT, 1), ui_push_mouse(MOUSE_BUTTON_LEFT, 0);
            float vx, vy;
            if (touch.scrolling && release_velocity(AMotionEvent_getEventTime(e) / 1e9, &vx, &vy) &&
                hypotf(vx, vy) > FLING_MIN_DP * dp()) {
                float speed = hypotf(vx, vy), most = FLING_MAX_DP * dp(), k = speed > most ? most / speed : 1;
                fling.vx = vx * k, fling.vy = vy * k;
            }
        }
        touch.pending = touch.scrolling = touch.held = 0;
        touch.lifted = 1;
        break;
    }
}

static int mouse_left, mouse_buttons;

static void on_mouse(const AInputEvent *e, int action)
{
    int buttons = AMotionEvent_getButtonState(e);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        if (buttons & AMOTION_EVENT_BUTTON_PRIMARY) ui_push_mouse(MOUSE_BUTTON_LEFT, 1), mouse_left = 1;
        break;
    case AMOTION_EVENT_ACTION_UP:
        if (mouse_left) ui_push_mouse(MOUSE_BUTTON_LEFT, 0), mouse_left = 0;
        break;
    /* AMotionEvent_getActionButton() needs API 33; the changed bit of the button state says the same */
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
        int changed = buttons ^ mouse_buttons;
        if (changed & AMOTION_EVENT_BUTTON_SECONDARY)
            ui_push_mouse(MOUSE_BUTTON_RIGHT, (buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
        if (changed & AMOTION_EVENT_BUTTON_TERTIARY)
            ui_push_mouse(MOUSE_BUTTON_MIDDLE, (buttons & AMOTION_EVENT_BUTTON_TERTIARY) != 0);
        break;
    }
    case AMOTION_EVENT_ACTION_SCROLL:
        ui_push_wheel(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0),
                      AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0));
        break;
    }
    mouse_buttons = buttons;
}

/* ---- keys and the soft keyboard ---- */

/* The character a key produces; the NDK has no getUnicodeChar(), so ask android.view.KeyEvent. */
static int key_char(int code, int meta)
{
    static jclass cls;
    static jmethodID ctor, get;
    JNIEnv *e = jni();
    if (!cls) {
        jclass c = (*e)->FindClass(e, "android/view/KeyEvent");
        if (jfailed(e) || !c) return 0;
        cls = (*e)->NewGlobalRef(e, c);
        (*e)->DeleteLocalRef(e, c);
        ctor = (*e)->GetMethodID(e, cls, "<init>", "(JJIIII)V");
        get = (*e)->GetMethodID(e, cls, "getUnicodeChar", "(I)I");
    }
    jobject k = (*e)->NewObject(e, cls, ctor, (jlong)0, (jlong)0, (jint)AKEY_EVENT_ACTION_DOWN, (jint)code, (jint)0,
                                (jint)meta);
    if (jfailed(e) || !k) return 0;
    int ch = (*e)->CallIntMethod(e, k, get, (jint)meta);
    (*e)->DeleteLocalRef(e, k);
    return jfailed(e) ? 0 : ch;
}

static int key_mods(int meta)
{
    return (meta & AMETA_SHIFT_ON ? MOD_SHIFT : 0) | (meta & AMETA_CTRL_ON ? MOD_CTRL : 0) |
           (meta & AMETA_ALT_ON ? MOD_ALT : 0) | (meta & AMETA_META_ON ? MOD_SUPER : 0);
}

/* FerretActivity.TextSink's text: code points and these */
#define IME_BACKSPACE -1
#define IME_ENTER -2
#define IME_DELETE -3
static pthread_mutex_t ime_lock = PTHREAD_MUTEX_INITIALIZER;
static int ime_q[1024], n_ime;

static void JNICALL native_text(JNIEnv *e, jclass cls, jintArray codes)
{
    jsize n = (*e)->GetArrayLength(e, codes);
    jint *v = (*e)->GetIntArrayElements(e, codes, NULL);
    if (!v) return;
    pthread_mutex_lock(&ime_lock);
    for (jsize i = 0; i < n && n_ime < (int)(sizeof ime_q / sizeof *ime_q); i++) ime_q[n_ime++] = v[i];
    pthread_mutex_unlock(&ime_lock);
    (*e)->ReleaseIntArrayElements(e, codes, v, JNI_ABORT);
    ALooper_wake(GetAndroidApp()->looper);
}

static void take_ime(void)
{
    pthread_mutex_lock(&ime_lock);
    for (int i = 0; i < n_ime; i++) {
        int c = ime_q[i];
        if (c == IME_ENTER) ui_push_key(KEY_ENTER, 0);
        else ui_push_char(c == IME_BACKSPACE ? 8 : c == IME_DELETE ? 127 : c);
    }
    n_ime = 0;
    pthread_mutex_unlock(&ime_lock);
}

/* ---- input and activity events ---- */

static int32_t (*raylib_input)(struct android_app *, AInputEvent *);
static void (*raylib_cmd)(struct android_app *, int32_t);
static double insets_until; /* poll the insets often until then: the soft keyboard is sliding */

static int32_t on_input(struct android_app *a, AInputEvent *e)
{
    int type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e), meta = AKeyEvent_getMetaState(e);
        if (code == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(e) == 0) ui_push_key(KEY_BACK, 0);
            return 1;
        }
        int32_t handled = raylib_input(a, e);
        int key;
        while ((key = GetKeyPressed())) ui_push_key(key, key_mods(meta));
        int repeat =
            action == AKEY_EVENT_ACTION_MULTIPLE ? AKeyEvent_getRepeatCount(e) : action == AKEY_EVENT_ACTION_DOWN;
        if (repeat > 0 && !(meta & (AMETA_CTRL_ON | AMETA_ALT_ON | AMETA_META_ON))) {
            int ch = key_char(code, meta);
            for (int i = 0; ch >= 32 && ch != 127 && i < repeat && i < 64; i++) ui_push_char(ch);
        }
        return handled;
    }
    int32_t handled = raylib_input(a, e);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, source = AInputEvent_getSource(e);
        if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) on_mouse(e, action);
        else if (source & AINPUT_SOURCE_TOUCHSCREEN) on_touch(e, action);
        ui.dirty = 1;
    }
    return handled;
}

static void on_cmd(struct android_app *a, int32_t cmd)
{
    raylib_cmd(a, cmd);
    /* back from another app (the permission settings, a viewer): the new surface needs a frame */
    if (cmd == APP_CMD_INIT_WINDOW || cmd == APP_CMD_GAINED_FOCUS || cmd == APP_CMD_CONFIG_CHANGED) {
        ui.dirty = 1;
        insets_until = fr_now() + 1;
    }
}

/* ---- platform.h ---- */

void plat_init_window(void)
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(0, 0, "ferret"); /* 0x0: the whole display */
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    struct android_app *a = GetAndroidApp();
    raylib_input = a->onInputEvent;
    a->onInputEvent = on_input;
    raylib_cmd = a->onAppCmd;
    a->onAppCmd = on_cmd;
    /* NativeActivity loads this library itself, so the class does not find its native method by name */
    JNIEnv *e = jni();
    jclass c = (*e)->GetObjectClass(e, a->activity->clazz);
    static const JNINativeMethod methods[] = {{"nativeText", "([I)V", (void *)native_text}};
    if ((*e)->RegisterNatives(e, c, methods, 1)) jfailed(e);
    (*e)->DeleteLocalRef(e, c);
}

/* the pointer stays while a list flings, so the wheel goes to what is under it; then it leaves, so nothing
 * looks hovered */
static void pointer_away(void)
{
    if (touch.lifted && !ui_events_pending() && !fling.vx && !fling.vy) {
        touch.lifted = 0;
        SetMousePosition(-10000, -10000);
    }
}

static float insets[4];
static int keyboard_shown;

void plat_frame(void)
{
    double now = fr_now();
    static double last_frame;
    double dt = fmin(now - last_frame, 0.05);
    last_frame = now;
    if (fling.vx || fling.vy) {
        scroll_by((float)(fling.vx * dt), (float)(fling.vy * dt));
        float k = expf(-FLING_DECAY * (float)dt);
        fling.vx *= k, fling.vy *= k;
        if (hypotf(fling.vx, fling.vy) < FLING_STOP_DP * dp()) fling.vx = fling.vy = 0;
    }
    if (touch.pending && now - touch.t >= LONG_PRESS_S) {
        touch.pending = 0, touch.held = 1;
        ui_push_mouse(MOUSE_BUTTON_RIGHT, 1), ui_push_mouse(MOUSE_BUTTON_RIGHT, 0);
        plat_haptic();
    }
    pointer_away();
    take_ime();

    int want = ui.focus != 0;
    if (want != keyboard_shown) plat_keyboard(want);

    static double insets_at = -1;
    if (insets_at < 0 || now - insets_at > 0.1) {
        insets_at = now;
        char *in = java("systemInsets", 0);
        float v[4];
        if (in && sscanf(in, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4) memcpy(insets, v, sizeof v);
        free(in);
    }
}

void plat_wait(double seconds)
{
    double now = fr_now();
    if (fling.vx || fling.vy) return;
    if (touch.pending) seconds = fmin(seconds, fmax(0, touch.t + LONG_PRESS_S - now));
    if (now < insets_until) seconds = fmin(seconds, 0.1);
    pointer_away();
    struct android_app *a = GetAndroidApp();
    struct android_poll_source *source = NULL;
    int events = 0;
    if (ALooper_pollOnce((int)ceil(seconds * 1000), NULL, &events, (void **)&source) >= 0 && source)
        source->process(a, source);
}

void plat_wake(void) { ALooper_wake(GetAndroidApp()->looper); }

float plat_scale(void) { return GetWindowScaleDPI().x; }

Vector2 plat_mouse(void) { return GetMousePosition(); }

int plat_touch(void) { return 1; }

void plat_insets(float *l, float *t, float *r, float *b) { *l = insets[0], *t = insets[1], *r = insets[2], *b = insets[3]; }

void plat_keyboard(int show)
{
    free(java(show ? "showKeyboard" : "hideKeyboard", 0));
    keyboard_shown = show;
    insets_until = fr_now() + 1;
}

int plat_open(const char *path)
{
    char *r = java("open", 1, path);
    int failed = !r;
    free(r);
    return failed ? -1 : 0;
}

void plat_set_clipboard(const char *text) { free(java("setClipboard", 1, text ? text : "")); }

char *plat_clipboard(void)
{
    char *s = java("clipboard", 0);
    if (s && !*s) free(s), s = NULL;
    return s;
}

void plat_haptic(void) { free(java("haptic", 0)); }

int plat_storage_granted(void)
{
    char *r = java("storageGranted", 0);
    int ok = r != NULL;
    free(r);
    return ok;
}

void plat_request_storage(void) { free(java("requestStorage", 0)); }

#define INTERNAL "/storage/emulated/0"

static void add(fr_places *p, const char *label, const char *path, const char *icon, int removable)
{
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return;
    FR_PUSH(*p, ((fr_place){fr_strdup(label), fr_uri_from_path(path), fr_strdup(icon), removable}));
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void plat_places(fr_places *out)
{
    add(out, "Internal storage", INTERNAL, "smartphone", 0);
    static const struct {
        const char *name, *icon;
    } dirs[] = {{"Download", "download"}, {"DCIM", "image"}, {"Pictures", "image"},
                {"Documents", "file-text"}, {"Music", "music"}, {"Movies", "video"}};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char *p = fr_format(INTERNAL "/%s", dirs[i].name);
        add(out, dirs[i].name, p, dirs[i].icon, 0);
        free(p);
    }
    /* SD cards and USB drives: /storage/<volume id> */
    fr_strs vols = {0};
    DIR *d = opendir("/storage");
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (de->d_name[0] != '.' && strcmp(de->d_name, "emulated") && strcmp(de->d_name, "self"))
            FR_PUSH(vols, fr_strdup(de->d_name));
    if (d) closedir(d);
    if (vols.n) qsort(vols.v, (size_t)vols.n, sizeof *vols.v, by_name);
    for (int i = 0; i < vols.n; i++) {
        char *p = fr_format("/storage/%s", vols.v[i]);
        add(out, vols.v[i], p, "hard-drive", 1);
        free(p);
    }
    fr_strs_free(&vols);
}

char *plat_home_uri(void) { return fr_uri_from_path(INTERNAL); }

/* ---- entry ---- */

int main(int argc, char **argv)
{
    struct android_app *a = GetAndroidApp();
    setup_environment();
    char arg0[] = "ferret";
    fr_app_main(1, (char *[]){arg0, NULL});
    /* The process must not outlive the window: the shared sources keep state in statics that a relaunch in the
     * same process would inherit (app.quit among them). */
    if (!a->destroyRequested) ANativeActivity_finish(a->activity);
    for (int i = 0; i < 50 && !a->destroyRequested; i++) {
        struct android_poll_source *source = NULL;
        int events = 0;
        if (ALooper_pollOnce(100, NULL, &events, (void **)&source) >= 0 && source) source->process(a, source);
    }
    exit(0);
}
