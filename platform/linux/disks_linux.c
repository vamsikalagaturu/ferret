/* The desktop's disks (platform.h) from UDisks2 over the system D-Bus. One thread keeps the list, rebuilt on
 * UDisks2's signals; each request runs on a thread and connection of its own, as polkit may hold it while it asks
 * for a password. */
#define _GNU_SOURCE
#include "platform.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define UDISKS "org.freedesktop.UDisks2"
#define IFACE(name) UDISKS "." name
#define CALL_TIMEOUT_US (10ull * 60 * 1000 * 1000)

typedef struct {
    char *path, *device, *label, *type, *drive, *backing, *cleartext, *bus;
    uint64_t size;
    fr_strs mounts;
    int block, fs, encrypted, loop, swap, hint_system, hint_ignore;
    int is_drive, removable, ejectable, can_power_off;
} obj;

typedef struct {
    obj *v;
    int n, cap;
} objs;

typedef struct {
    int op;
    char *id;
} running_op;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static fr_disks list;
static unsigned serial;
static struct {
    running_op *v;
    int n, cap;
} running;
static struct {
    fr_disk_done *v;
    int n, cap;
} finished;

static char *error_text(int r)
{
    char buf[256];
    return fr_strdup(strerror_r(-r, buf, sizeof buf));
}

/* ---- reading GetManagedObjects ---- */

static int skip(sd_bus_message *m) { return sd_bus_message_skip(m, "v"); }

static int holds(sd_bus_message *m, const char *sig)
{
    const char *contents = NULL;
    int r = sd_bus_message_peek_type(m, NULL, &contents);
    return r < 0 ? r : contents && !strcmp(contents, sig);
}

static int basic(sd_bus_message *m, char type, void *out)
{
    char sig[2] = {type, 0};
    int r = holds(m, sig);
    if (r <= 0) return r < 0 ? r : skip(m);
    if ((r = sd_bus_message_enter_container(m, 'v', sig)) < 0 || (r = sd_bus_message_read_basic(m, type, out)) < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int string(sd_bus_message *m, char type, char **out)
{
    const char *s = NULL;
    int r = basic(m, type, &s);
    if (r >= 0 && s) free(*out), *out = fr_strdup(s);
    return r;
}

static char *bytes_to_string(const void *p, size_t n) { return n ? fr_strndup(p, strnlen(p, n)) : fr_strdup(""); }

static int bytestring(sd_bus_message *m, char **out)
{
    const void *p = NULL;
    size_t n = 0;
    int r = holds(m, "ay");
    if (r <= 0) return r < 0 ? r : skip(m);
    if ((r = sd_bus_message_enter_container(m, 'v', "ay")) < 0 || (r = sd_bus_message_read_array(m, 'y', &p, &n)) < 0) return r;
    free(*out), *out = bytes_to_string(p, n);
    return sd_bus_message_exit_container(m);
}

static int mount_points(sd_bus_message *m, fr_strs *out)
{
    int r = holds(m, "aay");
    if (r <= 0) return r < 0 ? r : skip(m);
    if ((r = sd_bus_message_enter_container(m, 'v', "aay")) < 0 || (r = sd_bus_message_enter_container(m, 'a', "ay")) < 0) return r;
    while ((r = sd_bus_message_at_end(m, 0)) == 0) {
        const void *p = NULL;
        size_t n = 0;
        if ((r = sd_bus_message_read_array(m, 'y', &p, &n)) < 0) return r;
        FR_PUSH(*out, bytes_to_string(p, n));
    }
    if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int property(sd_bus_message *m, const char *iface, const char *name, obj *o)
{
    if (!strcmp(iface, IFACE("Block"))) {
        if (!strcmp(name, "PreferredDevice")) return bytestring(m, &o->device);
        if (!strcmp(name, "Size")) return basic(m, 't', &o->size);
        if (!strcmp(name, "IdLabel")) return string(m, 's', &o->label);
        if (!strcmp(name, "IdType")) return string(m, 's', &o->type);
        if (!strcmp(name, "HintSystem")) return basic(m, 'b', &o->hint_system);
        if (!strcmp(name, "HintIgnore")) return basic(m, 'b', &o->hint_ignore);
        if (!strcmp(name, "Drive")) return string(m, 'o', &o->drive);
        if (!strcmp(name, "CryptoBackingDevice")) return string(m, 'o', &o->backing);
    } else if (!strcmp(iface, IFACE("Filesystem"))) {
        if (!strcmp(name, "MountPoints")) return mount_points(m, &o->mounts);
    } else if (!strcmp(iface, IFACE("Encrypted"))) {
        if (!strcmp(name, "CleartextDevice")) return string(m, 'o', &o->cleartext);
    } else if (!strcmp(iface, IFACE("Drive"))) {
        if (!strcmp(name, "Removable")) return basic(m, 'b', &o->removable);
        if (!strcmp(name, "Ejectable")) return basic(m, 'b', &o->ejectable);
        if (!strcmp(name, "CanPowerOff")) return basic(m, 'b', &o->can_power_off);
        if (!strcmp(name, "ConnectionBus")) return string(m, 's', &o->bus);
    }
    return skip(m);
}

static int interface(sd_bus_message *m, obj *o)
{
    const char *iface;
    int r = sd_bus_message_read(m, "s", &iface);
    if (r < 0) return r;
    o->block |= !strcmp(iface, IFACE("Block"));
    o->fs |= !strcmp(iface, IFACE("Filesystem"));
    o->encrypted |= !strcmp(iface, IFACE("Encrypted"));
    o->loop |= !strcmp(iface, IFACE("Loop"));
    o->swap |= !strcmp(iface, IFACE("Swapspace"));
    o->is_drive |= !strcmp(iface, IFACE("Drive"));
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *name;
        if ((r = sd_bus_message_read(m, "s", &name)) < 0 || (r = property(m, iface, name, o)) < 0 ||
            (r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int object(sd_bus_message *m, obj *o)
{
    const char *path;
    int r = sd_bus_message_read(m, "o", &path);
    if (r < 0) return r;
    o->path = fr_strdup(path);
    if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0)
        if ((r = interface(m, o)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

static void obj_free(obj *o)
{
    free(o->path), free(o->device), free(o->label), free(o->type), free(o->drive), free(o->backing);
    free(o->cleartext), free(o->bus);
    fr_strs_free(&o->mounts);
}

static void objs_free(objs *o)
{
    for (int i = 0; i < o->n; i++) obj_free(&o->v[i]);
    free(o->v);
    *o = (objs){0};
}

static int collect(sd_bus *bus, objs *out)
{
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, UDISKS, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", NULL, &reply, "");
    if (r >= 0) r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        FR_PUSH(*out, (obj){0});
        if ((r = object(reply, &out->v[out->n - 1])) >= 0) r = sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    if (r < 0) objs_free(out);
    return r;
}

/* ---- from objects to disks ---- */

static const obj *find(const objs *o, const char *path)
{
    for (int i = 0; path && i < o->n; i++)
        if (!strcmp(o->v[i].path, path)) return &o->v[i];
    return NULL;
}

static int none(const char *path) { return !path || !strcmp(path, "/"); }

/* an unlocked volume's drive is its encrypted backing's */
static const obj *drive_of(const objs *o, const obj *b)
{
    const obj *backing = none(b->backing) ? NULL : find(o, b->backing);
    const obj *d = find(o, backing ? backing->drive : b->drive);
    return d && d->is_drive ? d : NULL;
}

static int system_mount(const fr_strs *mounts)
{
    for (int i = 0; i < mounts->n; i++)
        if (!strcmp(mounts->v[i], "/") || !strncmp(mounts->v[i], "/boot", 5)) return 1;
    return 0;
}

static int by_place(const void *a, const void *b)
{
    const fr_disk *x = a, *y = b;
    if (x->removable != y->removable) return x->removable - y->removable;
    return fr_natcmp(x->device, y->device);
}

static void build(const objs *o, fr_disks *out)
{
    for (int i = 0; i < o->n; i++) {
        const obj *b = &o->v[i];
        if (!b->block || b->loop || b->swap || (b->type && !strcmp(b->type, "swap"))) continue;
        if (!b->device || !strncmp(b->device, "/dev/loop", 9)) continue;
        if (b->hint_system && b->hint_ignore) continue;
        int locked = b->encrypted && none(b->cleartext);
        if ((!b->fs && !locked) || system_mount(&b->mounts)) continue;
        const obj *d = drive_of(o, b);
        char size[32];
        fr_format_size(b->size, size, sizeof size);
        fr_disk disk = {
            .id = fr_strdup(b->path),
            .label = b->label && *b->label ? fr_strdup(b->label) : fr_format("%s %s", b->type && *b->type ? b->type : "Disk", size),
            .device = fr_strdup(b->device),
            .mounts = fr_strs_copy(&b->mounts),
            .size = b->size,
            .removable = d && (d->removable || d->ejectable || (d->bus && !strcmp(d->bus, "usb"))),
            .locked = locked,
        };
        FR_PUSH(*out, disk);
    }
    if (out->n) qsort(out->v, (size_t)out->n, sizeof *out->v, by_place);
}

/* ---- the list's thread ---- */

static int changed;

static int on_signal(sd_bus_message *m, void *data, sd_bus_error *e)
{
    changed = 1;
    return 0;
}

static void refresh(sd_bus *bus)
{
    objs o = {0};
    if (collect(bus, &o) < 0) return;
    fr_disks fresh = {0};
    build(&o, &fresh);
    objs_free(&o);
    pthread_mutex_lock(&lock);
    fr_disks old = list;
    list = fresh;
    serial++;
    pthread_mutex_unlock(&lock);
    fr_disks_free(&old);
    plat_wake();
}

static void *watch(void *arg)
{
    sd_bus *bus = NULL;
    if (sd_bus_open_system(&bus) < 0) return NULL;
    if (sd_bus_add_match(bus, NULL, "type='signal',sender='" UDISKS "',interface='org.freedesktop.DBus.ObjectManager'",
                         on_signal, NULL) < 0 ||
        sd_bus_add_match(bus, NULL,
                         "type='signal',sender='" UDISKS "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                         on_signal, NULL) < 0) {
        sd_bus_flush_close_unref(bus);
        return NULL;
    }
    changed = 1;
    for (;;) {
        int r = sd_bus_process(bus, NULL);
        if (r < 0) break;
        if (r > 0) continue;
        if (changed) {
            changed = 0;
            refresh(bus);
            continue;
        }
        if (sd_bus_wait(bus, UINT64_MAX) < 0) break;
    }
    sd_bus_flush_close_unref(bus);
    return NULL;
}

static void start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, watch, NULL)) pthread_detach(t);
}

unsigned plat_disks_serial(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start);
    pthread_mutex_lock(&lock);
    unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}

static int running_index(const char *id)
{
    for (int i = 0; i < running.n; i++)
        if (!strcmp(running.v[i].id, id)) return i;
    return -1;
}

void plat_disks(fr_disks *out)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < list.n; i++) {
        fr_disk d = list.v[i];
        d.id = fr_strdup(d.id), d.label = fr_strdup(d.label), d.device = fr_strdup(d.device);
        d.mounts = fr_strs_copy(&d.mounts);
        int k = running_index(d.id);
        d.busy = k < 0 ? 0 : running.v[k].op;
        FR_PUSH(*out, d);
    }
    pthread_mutex_unlock(&lock);
}

/* ---- requests ---- */

static char *call(sd_bus *bus, const char *path, const char *iface, const char *method, char **mounted_at)
{
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_message_new_method_call(bus, &m, UDISKS, path, iface, method);
    if (r >= 0) r = sd_bus_message_set_allow_interactive_authorization(m, 1);
    if (r >= 0) r = sd_bus_message_append(m, "a{sv}", 0);
    if (r >= 0) r = sd_bus_call(bus, m, CALL_TIMEOUT_US, &e, &reply);
    const char *s;
    if (r >= 0 && mounted_at && sd_bus_message_read(reply, "s", &s) > 0) *mounted_at = fr_strdup(s);
    char *error = r >= 0 ? NULL : e.message ? fr_strdup(e.message) : error_text(r);
    for (size_t n = error ? strlen(error) : 0; n && (error[n - 1] == '\n' || error[n - 1] == ' '); n--) error[n - 1] = 0;
    sd_bus_error_free(&e);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    return error;
}

static char *eject(sd_bus *bus, const char *id, fr_strs *unmounted)
{
    objs o = {0};
    int r = collect(bus, &o);
    if (r < 0) return error_text(r);
    const obj *b = find(&o, id), *drive = b ? drive_of(&o, b) : NULL;
    char *error = drive ? NULL : fr_strdup("The drive is gone");
    for (int i = 0; !error && i < o.n; i++)
        if (o.v[i].fs && o.v[i].mounts.n && drive_of(&o, &o.v[i]) == drive &&
            !(error = call(bus, o.v[i].path, IFACE("Filesystem"), "Unmount", NULL)))
            for (int k = 0; k < o.v[i].mounts.n; k++) FR_PUSH(*unmounted, fr_strdup(o.v[i].mounts.v[k]));
    if (!error) error = call(bus, drive->path, IFACE("Drive"), drive->can_power_off ? "PowerOff" : "Eject", NULL);
    objs_free(&o);
    return error;
}

static void *run(void *arg)
{
    fr_disk_done *d = arg;
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    if (r < 0) d->error = error_text(r);
    else if (d->op == DISK_MOUNT) d->error = call(bus, d->id, IFACE("Filesystem"), "Mount", &d->path);
    else if (d->op == DISK_UNMOUNT) d->error = call(bus, d->id, IFACE("Filesystem"), "Unmount", NULL);
    else d->error = eject(bus, d->id, &d->unmounted);
    sd_bus_flush_close_unref(bus);
    pthread_mutex_lock(&lock);
    int k = running_index(d->id);
    if (k >= 0) {
        free(running.v[k].id);
        running.v[k] = running.v[--running.n];
    }
    FR_PUSH(finished, *d);
    serial++;
    pthread_mutex_unlock(&lock);
    free(d);
    plat_wake();
    return NULL;
}

void plat_disk_request(int op, const char *id)
{
    pthread_mutex_lock(&lock);
    if (running_index(id) >= 0) {
        pthread_mutex_unlock(&lock);
        return;
    }
    fr_disk_done *d = fr_realloc(NULL, sizeof *d);
    *d = (fr_disk_done){.op = op, .id = fr_strdup(id)};
    for (int i = 0; i < list.n && !d->label; i++)
        if (!strcmp(list.v[i].id, id)) {
            d->label = fr_strdup(list.v[i].label);
            if (op == DISK_UNMOUNT) d->unmounted = fr_strs_copy(&list.v[i].mounts);
        }
    if (!d->label) d->label = fr_strdup(id);
    FR_PUSH(running, ((running_op){op, fr_strdup(id)}));
    serial++;
    pthread_t t;
    if (!pthread_create(&t, NULL, run, d)) pthread_detach(t);
    else {
        d->error = fr_strdup("Cannot start a thread");
        free(running.v[--running.n].id);
        FR_PUSH(finished, *d);
        free(d);
    }
    pthread_mutex_unlock(&lock);
}

int plat_disk_done(fr_disk_done *out)
{
    pthread_mutex_lock(&lock);
    int any = finished.n > 0;
    if (any) {
        *out = finished.v[0];
        memmove(finished.v, finished.v + 1, sizeof *finished.v * (size_t)--finished.n);
    }
    pthread_mutex_unlock(&lock);
    return any;
}
