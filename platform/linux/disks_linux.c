/* The desktop's disks (platform.h) from UDisks2 over the system D-Bus. One thread keeps UDisks2's objects, re-read
 * on its signals: the sidebar's list is built from them and the disk manager gets a copy. Each request, and the disk
 * manager's queue, runs on a thread and connection of its own, as polkit may hold a call while it asks for a
 * password. Every call that changes a disk is first judged by dk_refusal on a fresh read of the objects. Phones and
 * cameras over MTP (src/vfs/vfs_mtp.c) follow the disks, opened by URI. */
#define _GNU_SOURCE
#include "disks/image.h"
#include "platform.h"
#include "vfs/vfs_mtp.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define UDISKS "org.freedesktop.UDisks2"
#define IFACE(name) UDISKS "." name
#define MANAGER "/org/freedesktop/UDisks2/Manager"
#define CALL_TIMEOUT_US (10ull * 60 * 1000 * 1000)
/* formats and resizes of large partitions, and polkit prompts, run far past D-Bus's 25 s default */
#define APPLY_TIMEOUT_US (60ull * 60 * 1000 * 1000)

typedef struct {
    int op;
    char *id;
} running_op;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static fr_disks list;
static dk_tree tree;
static int tree_ok;
static unsigned serial, tree_serial;
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
    fr_strs_free(out);
    while ((r = sd_bus_message_at_end(m, 0)) == 0) {
        const void *p = NULL;
        size_t n = 0;
        if ((r = sd_bus_message_read_array(m, 'y', &p, &n)) < 0) return r;
        FR_PUSH(*out, bytes_to_string(p, n));
    }
    if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int strings(sd_bus_message *m, fr_strs *out)
{
    int r = holds(m, "as");
    if (r <= 0) return r < 0 ? r : skip(m);
    if ((r = sd_bus_message_enter_container(m, 'v', "as")) < 0) return r;
    char **v = NULL;
    if ((r = sd_bus_message_read_strv(m, &v)) < 0) return r;
    for (char **s = v; s && *s; s++) FR_PUSH(*out, *s);
    free(v);
    return sd_bus_message_exit_container(m);
}

static int property(sd_bus_message *m, const char *iface, const char *name, dk_obj *o, dk_tree *t)
{
    if (!strcmp(iface, IFACE("Block"))) {
        if (!strcmp(name, "PreferredDevice")) return bytestring(m, &o->device);
        if (!strcmp(name, "Size")) return basic(m, 't', &o->size);
        if (!strcmp(name, "IdLabel")) return string(m, 's', &o->id_label);
        if (!strcmp(name, "IdType")) return string(m, 's', &o->id_type);
        if (!strcmp(name, "IdUUID")) return string(m, 's', &o->id_uuid);
        if (!strcmp(name, "IdUsage")) return string(m, 's', &o->id_usage);
        if (!strcmp(name, "HintSystem")) return basic(m, 'b', &o->hint_system);
        if (!strcmp(name, "HintIgnore")) return basic(m, 'b', &o->hint_ignore);
        if (!strcmp(name, "Drive")) return string(m, 'o', &o->drive_path);
        if (!strcmp(name, "CryptoBackingDevice")) return string(m, 'o', &o->backing);
    } else if (!strcmp(iface, IFACE("Partition"))) {
        if (!strcmp(name, "Table")) return string(m, 'o', &o->table_path);
        if (!strcmp(name, "Type")) return string(m, 's', &o->part_type);
        if (!strcmp(name, "UUID")) return string(m, 's', &o->part_uuid);
        if (!strcmp(name, "Name")) return string(m, 's', &o->part_name);
        if (!strcmp(name, "Offset")) return basic(m, 't', &o->part_offset);
        if (!strcmp(name, "Size")) return basic(m, 't', &o->part_size);
        if (!strcmp(name, "Number")) return basic(m, 'u', &o->number);
        if (!strcmp(name, "IsContainer")) return basic(m, 'b', &o->container);
    } else if (!strcmp(iface, IFACE("PartitionTable"))) {
        if (!strcmp(name, "Type")) return string(m, 's', &o->table_type);
    } else if (!strcmp(iface, IFACE("Filesystem"))) {
        if (!strcmp(name, "MountPoints")) return mount_points(m, &o->mounts);
    } else if (!strcmp(iface, IFACE("Swapspace"))) {
        if (!strcmp(name, "Active")) return basic(m, 'b', &o->swap_active);
    } else if (!strcmp(iface, IFACE("Encrypted"))) {
        if (!strcmp(name, "CleartextDevice")) return string(m, 'o', &o->cleartext);
    } else if (!strcmp(iface, IFACE("Drive"))) {
        if (!strcmp(name, "Vendor")) return string(m, 's', &o->vendor);
        if (!strcmp(name, "Model")) return string(m, 's', &o->model);
        if (!strcmp(name, "Serial")) return string(m, 's', &o->serial);
        if (!strcmp(name, "SortKey")) return string(m, 's', &o->sort_key);
        if (!strcmp(name, "Removable")) return basic(m, 'b', &o->removable);
        if (!strcmp(name, "Ejectable")) return basic(m, 'b', &o->ejectable);
        if (!strcmp(name, "CanPowerOff")) return basic(m, 'b', &o->can_power_off);
        if (!strcmp(name, "ConnectionBus")) return string(m, 's', &o->bus);
    } else if (!strcmp(iface, IFACE("Job"))) {
        if (!strcmp(name, "Operation")) return string(m, 's', &o->job_operation);
        if (!strcmp(name, "Progress")) return basic(m, 'd', &o->job_progress);
    } else if (!strcmp(iface, IFACE("Manager")) && t) {
        if (!strcmp(name, "SupportedFilesystems")) return strings(m, &t->formats);
        if (!strcmp(name, "Version")) return string(m, 's', &t->version);
    }
    return skip(m);
}

static int interface(sd_bus_message *m, dk_obj *o, dk_tree *t)
{
    const char *iface;
    int r = sd_bus_message_read(m, "s", &iface);
    if (r < 0) return r;
    o->block |= !strcmp(iface, IFACE("Block"));
    o->partition |= !strcmp(iface, IFACE("Partition"));
    o->table |= !strcmp(iface, IFACE("PartitionTable"));
    o->fs |= !strcmp(iface, IFACE("Filesystem"));
    o->encrypted |= !strcmp(iface, IFACE("Encrypted"));
    o->loop |= !strcmp(iface, IFACE("Loop"));
    o->swap |= !strcmp(iface, IFACE("Swapspace"));
    o->drive |= !strcmp(iface, IFACE("Drive"));
    if (!strcmp(iface, IFACE("Job"))) o->job = 1, o->job_progress = -1;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *name;
        if ((r = sd_bus_message_read(m, "s", &name)) < 0 || (r = property(m, iface, name, o, t)) < 0 ||
            (r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int object(sd_bus_message *m, dk_obj *o, dk_tree *t)
{
    const char *path;
    int r = sd_bus_message_read(m, "o", &path);
    if (r < 0) return r;
    o->path = fr_strdup(path);
    if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0)
        if ((r = interface(m, o, strcmp(path, MANAGER) ? NULL : t)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

/* the objects into t->objs, and the Manager's properties into t */
static int collect(sd_bus *bus, dk_tree *t)
{
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, UDISKS, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", NULL, &reply, "");
    if (r >= 0) r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        FR_PUSH(t->objs, (dk_obj){0});
        if ((r = object(reply, &t->objs.v[t->objs.n - 1], t)) >= 0) r = sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    if (r < 0) dk_tree_free(t);
    return r;
}

static int collect_objs(sd_bus *bus, dk_objs *out)
{
    dk_tree t = {0};
    int r = collect(bus, &t);
    *out = t.objs;
    t.objs = (dk_objs){0};
    dk_tree_free(&t);
    return r;
}

/* Which filesystems UDisks2 can make and resize here: that depends on the mkfs and resize tools installed, so it is
 * asked once per start of the list's thread. Both are queries. */
static void capabilities(sd_bus *bus, dk_tree *t)
{
    fr_strs supported = t->formats;
    t->formats = (fr_strs){0};
    for (int i = 0; i < supported.n; i++) {
        const char *fs = supported.v[i];
        sd_bus_message *reply = NULL;
        int ok = 0;
        uint64_t modes = 0;
        const char *tool;
        if (sd_bus_call_method(bus, UDISKS, MANAGER, IFACE("Manager"), "CanFormat", NULL, &reply, "s", fs) >= 0 &&
            sd_bus_message_read(reply, "(bs)", &ok, &tool) >= 0 && ok)
            FR_PUSH(t->formats, fr_strdup(fs));
        reply = sd_bus_message_unref(reply);
        ok = 0;
        if (sd_bus_call_method(bus, UDISKS, MANAGER, IFACE("Manager"), "CanResize", NULL, &reply, "s", fs) >= 0 &&
            sd_bus_message_read(reply, "(bts)", &ok, &modes, &tool) >= 0 && ok)
            FR_PUSH(t->resize, ((dk_resize_mode){fr_strdup(fs), modes}));
        sd_bus_message_unref(reply);
    }
    fr_strs_free(&supported);
}

/* ---- the sidebar's list ---- */

static int none(const char *path) { return !path || !strcmp(path, "/"); }

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

/* the disk manager's segment for a block (an unlocked one's is its encrypted partition's), with its drive */
static const dk_seg *seg_of(const dk_disks *dd, const dk_obj *b, const dk_disk **drive)
{
    const char *path = b->backing && !none(b->backing) ? b->backing : b->path;
    for (int i = 0; i < dd->n; i++)
        for (int k = 0; k < dd->v[i].segs.n; k++)
            if (dd->v[i].segs.v[k].path && !strcmp(dd->v[i].segs.v[k].path, path)) {
                *drive = &dd->v[i];
                return &dd->v[i].segs.v[k];
            }
    return NULL;
}

/* An unlabeled filesystem is named by the OS found on it; "Windows" from partition types alone only on internal
 * drives, as every NTFS or exFAT stick carries Microsoft's type too. Neighbours' OS (inferred) names nothing. */
static char *name_of(const dk_obj *b, const dk_seg *s, int removable)
{
    if (b->id_label && *b->id_label) return fr_strdup(b->id_label);
    if (s && !s->inferred && (s->group_kind == GROUP_OS || s->group_kind == GROUP_RUNNING || (s->group_kind == GROUP_WINDOWS && !removable)))
        return fr_strdup(s->group);
    return fr_strdup(b->id_type && *b->id_type ? b->id_type : "Disk");
}

static void build(const dk_tree *t, fr_disks *out)
{
    const dk_objs *o = &t->objs;
    dk_env env = {0};
    dk_env_read(&env);
    dk_disks dd = {0};
    dk_build(t, &env, &dd);
    for (int i = 0; i < o->n; i++) {
        const dk_obj *b = &o->v[i];
        if (!b->block || b->loop || b->swap || (b->id_type && !strcmp(b->id_type, "swap"))) continue;
        if (!b->device || !strncmp(b->device, "/dev/loop", 9)) continue;
        if (b->hint_system && b->hint_ignore) continue;
        int locked = b->encrypted && none(b->cleartext);
        if ((!b->fs && !locked) || system_mount(&b->mounts)) continue;
        const dk_obj *d = dk_drive_of(o, b);
        const dk_disk *drive = NULL;
        const dk_seg *s = seg_of(&dd, b, &drive);
        int removable = d && (d->removable || d->ejectable || (d->bus && !strcmp(d->bus, "usb")));
        fr_disk disk = {
            .id = fr_strdup(b->path),
            .label = name_of(b, s, removable),
            .device = fr_strdup(b->device),
            .drive = drive && drive->name && *drive->name ? fr_strdup(drive->name) : NULL,
            .mounts = fr_strs_copy(&b->mounts),
            .size = b->size,
            .removable = removable,
            .locked = locked,
        };
        FR_PUSH(*out, disk);
    }
    dk_disks_free(&dd);
    dk_env_free(&env);
    if (out->n) qsort(out->v, (size_t)out->n, sizeof *out->v, by_place);
}

/* ---- the list's thread ---- */

static int changed;

static int on_signal(sd_bus_message *m, void *data, sd_bus_error *e)
{
    changed = 1;
    return 0;
}

static void refresh(sd_bus *bus, int *caps_done)
{
    dk_tree t = {0};
    if (collect(bus, &t) < 0) {
        pthread_mutex_lock(&lock);
        tree_ok = 0;
        tree_serial++;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_lock(&lock);
    int have_caps = *caps_done;
    pthread_mutex_unlock(&lock);
    if (!have_caps) {
        capabilities(bus, &t);
        *caps_done = 1;
    } else {
        pthread_mutex_lock(&lock);
        fr_strs_free(&t.formats);
        t.formats = fr_strs_copy(&tree.formats);
        for (int i = 0; i < tree.resize.n; i++) FR_PUSH(t.resize, ((dk_resize_mode){fr_strdup(tree.resize.v[i].fs), tree.resize.v[i].modes}));
        pthread_mutex_unlock(&lock);
    }
    fr_disks fresh = {0};
    build(&t, &fresh);
    pthread_mutex_lock(&lock);
    fr_disks old = list;
    dk_tree old_tree = tree;
    list = fresh;
    tree = t;
    tree_ok = 1;
    serial++;
    tree_serial++;
    pthread_mutex_unlock(&lock);
    fr_disks_free(&old);
    dk_tree_free(&old_tree);
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
    int caps_done = 0;
    for (;;) {
        int r = sd_bus_process(bus, NULL);
        if (r < 0) break;
        if (r > 0) continue;
        if (changed) {
            changed = 0;
            refresh(bus, &caps_done);
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
    fr_mtp_start(plat_wake);
}

static void start_once(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start);
}

unsigned plat_disks_serial(void)
{
    start_once();
    pthread_mutex_lock(&lock);
    unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s + fr_mtp_serial();
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
        d.id = fr_strdup(d.id), d.label = fr_strdup(d.label), d.device = fr_strdup(d.device), d.drive = d.drive ? fr_strdup(d.drive) : NULL;
        d.mounts = fr_strs_copy(&d.mounts);
        int k = running_index(d.id);
        d.busy = k < 0 ? 0 : running.v[k].op;
        FR_PUSH(*out, d);
    }
    pthread_mutex_unlock(&lock);
    fr_mtp_devices mtp = {0};
    fr_mtp_list_devices(&mtp);
    for (int i = 0; i < mtp.n; i++) {
        fr_disk d = {.id = fr_strdup(mtp.v[i].uri), .label = fr_strdup(mtp.v[i].label), .device = fr_strdup(""),
                     .uri = fr_strdup(mtp.v[i].uri), .size = mtp.v[i].size, .removable = 1};
        FR_PUSH(*out, d);
    }
    fr_mtp_devices_free(&mtp);
}

/* ---- calls ---- */

static void trim(char *s)
{
    for (size_t n = s ? strlen(s) : 0; n && (s[n - 1] == '\n' || s[n - 1] == ' '); n--) s[n - 1] = 0;
}

/* the backstop, on a fresh read of the objects; NULL when the call may go ahead */
static char *refused(sd_bus *bus, const char *object, const char *method)
{
    dk_objs o = {0};
    int r = collect_objs(bus, &o);
    if (r < 0) return error_text(r);
    char *root = dk_root_drive(&o);
    const char *why = dk_refusal(&o, root, object, method);
    char *out = why ? fr_strdup(why) : NULL;
    free(root);
    dk_objs_free(&o);
    return out;
}

static int append_options(sd_bus_message *m, const char *label, int update_type, int ro)
{
    int r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r >= 0 && update_type) r = sd_bus_message_append(m, "{sv}", "update-partition-type", "b", 1);
    if (r >= 0 && label && *label) r = sd_bus_message_append(m, "{sv}", "label", "s", label);
    if (r >= 0 && ro) r = sd_bus_message_append(m, "{sv}", "options", "s", "ro");
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

/* sends m; NULL when done, else why not. The reply goes to *reply when asked for. */
static char *send(sd_bus *bus, sd_bus_message *m, int r, uint64_t timeout, sd_bus_message **reply)
{
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message *rep = NULL;
    if (r >= 0) r = sd_bus_call(bus, m, timeout, &e, &rep);
    char *error = r >= 0 ? NULL : e.message ? fr_strdup(e.message) : error_text(r);
    trim(error);
    sd_bus_error_free(&e);
    if (reply && !error) *reply = rep;
    else sd_bus_message_unref(rep);
    sd_bus_message_unref(m);
    return error;
}

static int new_call(sd_bus *bus, sd_bus_message **m, const char *path, const char *iface, const char *method)
{
    int r = sd_bus_message_new_method_call(bus, m, UDISKS, path, iface, method);
    if (r >= 0) r = sd_bus_message_set_allow_interactive_authorization(*m, 1);
    return r;
}

static char *call(sd_bus *bus, const char *path, const char *iface, const char *method, int ro, char **mounted_at)
{
    char *why = refused(bus, path, method);
    if (why) return why;
    sd_bus_message *m = NULL, *reply = NULL;
    int r = new_call(bus, &m, path, iface, method);
    if (r >= 0) r = append_options(m, NULL, 0, ro);
    char *error = send(bus, m, r, CALL_TIMEOUT_US, mounted_at ? &reply : NULL);
    const char *s;
    if (!error && mounted_at && sd_bus_message_read(reply, "s", &s) > 0) *mounted_at = fr_strdup(s);
    sd_bus_message_unref(reply);
    return error;
}

static char *eject(sd_bus *bus, const char *id, fr_strs *unmounted, int power_off_only)
{
    dk_objs o = {0};
    int r = collect_objs(bus, &o);
    if (r < 0) return error_text(r);
    const dk_obj *b = dk_find(&o, id), *drive = b ? (b->drive ? b : dk_drive_of(&o, b)) : NULL;
    char *error = drive ? NULL : fr_strdup("The drive is gone");
    for (int i = 0; !error && !power_off_only && i < o.n; i++)
        if (o.v[i].fs && o.v[i].mounts.n && dk_drive_of(&o, &o.v[i]) == drive &&
            !(error = call(bus, o.v[i].path, IFACE("Filesystem"), "Unmount", 0, NULL)))
            for (int k = 0; k < o.v[i].mounts.n; k++) FR_PUSH(*unmounted, fr_strdup(o.v[i].mounts.v[k]));
    if (!error) error = call(bus, drive->path, IFACE("Drive"), power_off_only || drive->can_power_off ? "PowerOff" : "Eject", 0, NULL);
    dk_objs_free(&o);
    return error;
}

/* ---- requests ---- */

static void *run(void *arg)
{
    fr_disk_done *d = arg;
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    if (r < 0) d->error = error_text(r);
    else if (d->op == DISK_MOUNT || d->op == DISK_MOUNT_RO)
        d->error = call(bus, d->id, IFACE("Filesystem"), "Mount", d->op == DISK_MOUNT_RO, &d->path);
    else if (d->op == DISK_UNMOUNT || d->op == DISK_RELEASE) d->error = call(bus, d->id, IFACE("Filesystem"), "Unmount", 0, NULL);
    else d->error = eject(bus, d->id, &d->unmounted, d->op == DISK_POWER_OFF);
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
            if (op == DISK_UNMOUNT || op == DISK_RELEASE) d->unmounted = fr_strs_copy(&list.v[i].mounts);
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

/* ---- the disk manager ---- */

int plat_dm_supported(void) { return 1; }

unsigned plat_dm_serial(void)
{
    start_once();
    pthread_mutex_lock(&lock);
    unsigned s = tree_serial;
    pthread_mutex_unlock(&lock);
    return s;
}

int plat_dm_tree(dk_tree *out)
{
    start_once();
    pthread_mutex_lock(&lock);
    int ok = tree_ok;
    if (ok) *out = dk_tree_copy(&tree);
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

char *plat_volume_path(const char *uri)
{
    const char *p = fr_uri_local(uri);
    return p ? fr_strdup(p) : NULL;
}

static fr_dm_run status = {.progress = -1};

static void set_progress(void *ctx, double fraction, const char *text)
{
    pthread_mutex_lock(&lock);
    status.progress = fraction;
    snprintf(status.text, sizeof status.text, "%s", text);
    pthread_mutex_unlock(&lock);
    plat_wake();
}

/* UDisks2 opens the whole device after polkit agrees (OpenDevice "rw"); the stick is checked for mounts again, as
 * something may have automounted it since it was queued */
static char *write_image(sd_bus *bus, const dk_call *c)
{
    dk_objs o = {0};
    int r = collect_objs(bus, &o);
    if (r < 0) return error_text(r);
    const dk_obj *disk = dk_find(&o, c->object), *drive = disk ? dk_drive_of(&o, disk) : NULL;
    char *error = disk && drive ? NULL : fr_strdup("The disk is gone.");
    for (int i = 0; !error && i < o.n; i++)
        if (o.v[i].mounts.n && dk_drive_of(&o, &o.v[i]) == drive)
            error = fr_format("A partition on %s got mounted since it was queued: unmount it first.", disk->device);
    dk_objs_free(&o);
    if (error || (error = refused(bus, c->object, "OpenDevice"))) return error;
    set_progress(NULL, 0, "Waiting for permission");
    sd_bus_message *m = NULL, *reply = NULL;
    r = new_call(bus, &m, c->object, IFACE("Block"), "OpenDevice");
    if (r >= 0) r = sd_bus_message_append(m, "s", "rw");
    if (r >= 0) r = append_options(m, NULL, 0, 0);
    if ((error = send(bus, m, r, APPLY_TIMEOUT_US, &reply))) return error;
    int fd = -1, handed = -1;
    if (sd_bus_message_read(reply, "h", &handed) > 0) fd = dup(handed);
    sd_bus_message_unref(reply);
    if (fd < 0) return fr_strdup("UDisks2 returned no usable device handle.");
    if ((error = dk_image_write(fd, c->file, c->size, set_progress, NULL))) return error;
    set_progress(NULL, -1, "");
    m = NULL;
    r = new_call(bus, &m, c->object, IFACE("Block"), "Rescan");
    if (r >= 0) r = append_options(m, NULL, 0, 0);
    return send(bus, m, r, CALL_TIMEOUT_US, NULL);
}

static char *run_call(sd_bus *bus, const dk_call *c)
{
    if (c->method == CALL_IMAGE) return write_image(bus, c);
    char *why = refused(bus, c->object, dk_call_method(c->method));
    if (why) return why;
    static const char *const ifaces[] = {[CALL_DELETE] = IFACE("Partition"),       [CALL_CREATE] = IFACE("PartitionTable"),
                                         [CALL_CREATE_FORMAT] = IFACE("PartitionTable"), [CALL_FORMAT] = IFACE("Block"),
                                         [CALL_SET_LABEL] = IFACE("Filesystem"),   [CALL_PART_RESIZE] = IFACE("Partition"),
                                         [CALL_FS_RESIZE] = IFACE("Filesystem")};
    sd_bus_message *m = NULL;
    int r = new_call(bus, &m, c->object, ifaces[c->method], dk_call_method(c->method));
    switch (c->method) {
    case CALL_DELETE: break;
    case CALL_CREATE:
        if (r >= 0) r = sd_bus_message_append(m, "ttss", c->offset, c->size, c->ptype, "");
        break;
    case CALL_CREATE_FORMAT:
        if (r >= 0) r = sd_bus_message_append(m, "ttss", c->offset, c->size, c->ptype, "");
        if (r >= 0) r = append_options(m, NULL, 0, 0);
        if (r >= 0) r = sd_bus_message_append(m, "s", c->fs);
        if (r >= 0) r = append_options(m, c->label, 1, 0);
        return send(bus, m, r, APPLY_TIMEOUT_US, NULL);
    case CALL_FORMAT:
        if (r >= 0) r = sd_bus_message_append(m, "s", c->fs);
        if (r >= 0) r = append_options(m, c->label, 1, 0);
        return send(bus, m, r, APPLY_TIMEOUT_US, NULL);
    case CALL_SET_LABEL:
        if (r >= 0) r = sd_bus_message_append(m, "s", c->label);
        break;
    default:
        if (r >= 0) r = sd_bus_message_append(m, "t", c->size);
        break;
    }
    if (r >= 0) r = append_options(m, NULL, 0, 0);
    return send(bus, m, r, APPLY_TIMEOUT_US, NULL);
}

static void *apply(void *arg)
{
    dk_steps *steps = arg;
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    char *error = r < 0 ? error_text(r) : NULL;
    for (int i = 0; !error && i < steps->n; i++) {
        for (int k = 0; !error && k < steps->v[i].n; k++) error = run_call(bus, &steps->v[i].v[k]);
        if (error) break;
        pthread_mutex_lock(&lock);
        status.done++;
        status.step_done = 1;
        pthread_mutex_unlock(&lock);
        plat_wake();
    }
    sd_bus_flush_close_unref(bus);
    dk_steps_free(steps);
    free(steps);
    pthread_mutex_lock(&lock);
    status.running = 0;
    status.finished = 1;
    status.progress = -1;
    status.text[0] = 0;
    snprintf(status.error, sizeof status.error, "%s", error ? error : "");
    pthread_mutex_unlock(&lock);
    free(error);
    plat_wake();
    return NULL;
}

void plat_dm_apply(dk_steps *steps)
{
    dk_steps *mine = fr_realloc(NULL, sizeof *mine);
    *mine = *steps;
    *steps = (dk_steps){0};
    pthread_mutex_lock(&lock);
    int busy = status.running;
    if (!busy) status = (fr_dm_run){.running = 1, .progress = -1};
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (busy || pthread_create(&t, NULL, apply, mine)) {
        dk_steps_free(mine);
        free(mine);
        pthread_mutex_lock(&lock);
        if (!busy) status = (fr_dm_run){.finished = 1, .progress = -1};
        snprintf(status.error, sizeof status.error, "%s", busy ? "Changes are being applied already." : "Cannot start a thread");
        status.finished = 1;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_detach(t);
}

void plat_dm_status(fr_dm_run *out)
{
    pthread_mutex_lock(&lock);
    *out = status;
    if (status.progress < 0 && status.running)
        for (int i = 0; i < tree.objs.n; i++)
            if (tree.objs.v[i].job && tree.objs.v[i].job_operation) {
                snprintf(out->text, sizeof out->text, "%s", tree.objs.v[i].job_operation);
                out->progress = tree.objs.v[i].job_progress;
            }
    status.step_done = status.finished = 0;
    pthread_mutex_unlock(&lock);
}
