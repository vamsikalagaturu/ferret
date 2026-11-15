/* libmtp is not thread-safe per device, so every call on one runs under its `io` mutex; a device stays alive while
 * a call holds a reference to it, even once it is unplugged. MTP names objects by number, not by path: paths are
 * resolved one folder listing at a time, and the listings are cached briefly. Files are downloaded to a temporary
 * file when opened and uploaded from one when closed. */
#define _GNU_SOURCE
#include "vfs_mtp.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libmtp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT_PARENT LIBMTP_FILES_AND_FOLDERS_ROOT
#define CACHE_SECONDS 30.0
#define SCAN_SECONDS 2
#define RETRY_SECONDS 10.0

/* ---- URIs ---- */

void fr_mtp_uri_free(fr_mtp_uri *u)
{
    free(u->device), free(u->storage), free(u->path);
    *u = (fr_mtp_uri){0};
}

static int dots(const char *s, size_t n) { return (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'); }

int fr_mtp_parse(const char *uri, fr_mtp_uri *out)
{
    *out = (fr_mtp_uri){0};
    if (strncmp(uri, "mtp://", 6)) return -EINVAL;
    const char *p = uri + 6;
    size_t n = strcspn(p, "/");
    if (!n) return -EINVAL;
    out->device = fr_strndup(p, n);
    out->path = fr_realloc(NULL, strlen(p) + 2);
    size_t len = 0;
    for (p += n; *p;) {
        p += strspn(p, "/");
        size_t k = strcspn(p, "/");
        if (!k) break;
        if (dots(p, k)) {
            fr_mtp_uri_free(out);
            return -EINVAL;
        }
        if (!out->storage) out->storage = fr_strndup(p, k);
        else {
            out->path[len++] = '/';
            memcpy(out->path + len, p, k);
            len += k;
        }
        p += k;
    }
    if (!len) out->path[len++] = '/';
    out->path[len] = 0;
    return 0;
}

/* ---- devices ---- */

typedef struct {
    char *key; /* "<storage>/<path>" */
    uint32_t storage, id;
    int is_dir;
    uint64_t size;
    int64_t mtime;
    double at;
} node;

typedef struct {
    char *id, *label;
    uint32_t bus;
    uint8_t devnum;
    uint64_t size;
    LIBMTP_mtpdevice_t *mtp;
    pthread_mutex_t io;
    int refs; /* the list's, and one per call using it */
    struct {
        node *v;
        int n, cap;
    } cache;
} device;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* devices, refs, serial */
static struct {
    device **v;
    int n, cap;
} devices;
static unsigned serial;
static void (*wake_fn)(void);

static void cache_clear(device *d)
{
    for (int i = 0; i < d->cache.n; i++) free(d->cache.v[i].key);
    d->cache.n = 0;
}

static void cache_drop(device *d, int i)
{
    free(d->cache.v[i].key);
    d->cache.v[i] = d->cache.v[--d->cache.n];
}

static node *cache_find(device *d, const char *key)
{
    double now = fr_now();
    node *hit = NULL;
    for (int i = d->cache.n - 1; i >= 0; i--)
        if (now - d->cache.v[i].at > CACHE_SECONDS) cache_drop(d, i);
    for (int i = 0; i < d->cache.n && !hit; i++)
        if (!strcmp(d->cache.v[i].key, key)) hit = &d->cache.v[i];
    return hit;
}

/* what was cached directly inside the folder `key` */
static void cache_drop_children(device *d, const char *key)
{
    size_t n = strlen(key);
    for (int i = d->cache.n - 1; i >= 0; i--) {
        const char *k = d->cache.v[i].key;
        if (!strncmp(k, key, n) && k[n] == '/' && !strchr(k + n + 1, '/')) cache_drop(d, i);
    }
}

static void device_free(device *d)
{
    LIBMTP_Release_Device(d->mtp);
    cache_clear(d);
    free(d->cache.v);
    pthread_mutex_destroy(&d->io);
    free(d->id), free(d->label);
    free(d);
}

/* the device, its io held; NULL when it is not connected */
static device *take(const char *id)
{
    device *d = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < devices.n && !d; i++)
        if (!strcmp(devices.v[i]->id, id)) d = devices.v[i], d->refs++;
    pthread_mutex_unlock(&lock);
    if (d) pthread_mutex_lock(&d->io);
    return d;
}

static void unref(device *d)
{
    pthread_mutex_lock(&lock);
    int last = --d->refs == 0;
    pthread_mutex_unlock(&lock);
    if (last) device_free(d);
}

static void give(device *d)
{
    pthread_mutex_unlock(&d->io);
    unref(d);
}

static int mtp_error(device *d)
{
    int rc = -EIO;
    for (LIBMTP_error_t *e = LIBMTP_Get_Errorstack(d->mtp); e && rc == -EIO; e = e->next)
        switch (e->errornumber) {
        case LIBMTP_ERROR_STORAGE_FULL: rc = -ENOSPC; break;
        case LIBMTP_ERROR_NO_DEVICE_ATTACHED: rc = -ENODEV; break;
        case LIBMTP_ERROR_CANCELLED: rc = -ECANCELED; break;
        case LIBMTP_ERROR_MEMORY_ALLOCATION: rc = -ENOMEM; break;
        default: break;
        }
    LIBMTP_Clear_Errorstack(d->mtp);
    return rc;
}

/* ---- storages ---- */

static void storage_name(const device *d, const LIBMTP_devicestorage_t *s, char *buf, size_t n)
{
    const char *desc = s->StorageDescription;
    int unique = desc && *desc && !dots(desc, strlen(desc));
    for (const LIBMTP_devicestorage_t *o = d->mtp->storage; unique && o; o = o->next)
        unique = o == s || !o->StorageDescription || strcmp(o->StorageDescription, desc);
    if (!unique) {
        snprintf(buf, n, "storage-%08x", (unsigned)s->id);
        return;
    }
    snprintf(buf, n, "%s", desc);
    for (char *c = buf; *c; c++)
        if (*c == '/') *c = '-';
}

/* 0, or -EACCES when the device shows none (a locked phone) */
static int storages_refresh(device *d)
{
    int r = LIBMTP_Get_Storage(d->mtp, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (r < 0) return mtp_error(d);
    return d->mtp->storage ? 0 : -EACCES;
}

static LIBMTP_devicestorage_t *storage_find(device *d, const char *name)
{
    char buf[256];
    for (int pass = 0; pass < 2; pass++) {
        if ((pass || !d->mtp->storage) && storages_refresh(d)) return NULL;
        for (LIBMTP_devicestorage_t *s = d->mtp->storage; s; s = s->next) {
            storage_name(d, s, buf, sizeof buf);
            if (!strcmp(buf, name)) return s;
        }
    }
    return NULL;
}

/* ---- objects ---- */

static void fill(fr_entry *e, const char *name, int is_dir, uint64_t size, int64_t mtime)
{
    *e = (fr_entry){.name = fr_strdup(name), .is_dir = is_dir, .hidden = name[0] == '.', .size = is_dir ? 0 : size,
                    .mtime = mtime};
}

/* lists folder `parent` (named `key` in the cache) into the cache, and into out unless NULL */
static int list_children(device *d, const char *key, uint32_t storage, uint32_t parent, fr_entries *out)
{
    LIBMTP_Clear_Errorstack(d->mtp);
    LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(d->mtp, storage, parent);
    if (!files && LIBMTP_Get_Errorstack(d->mtp)) return mtp_error(d);
    cache_drop_children(d, key);
    double now = fr_now();
    while (files) {
        LIBMTP_file_t *f = files;
        files = f->next;
        if (f->filename && fr_name_valid(f->filename)) {
            node c = {fr_format("%s/%s", key, f->filename), f->storage_id, f->item_id, f->filetype == LIBMTP_FILETYPE_FOLDER,
                      f->filesize, (int64_t)f->modificationdate, now};
            FR_PUSH(d->cache, c);
            if (out) {
                fr_entry e;
                fill(&e, f->filename, c.is_dir, c.size, c.mtime);
                FR_PUSH(*out, e);
            }
        }
        LIBMTP_destroy_file_t(f);
    }
    return 0;
}

/* what u names below a storage; a storage's root is a folder with id ROOT_PARENT */
static int resolve(device *d, const fr_mtp_uri *u, node *out)
{
    LIBMTP_devicestorage_t *s = storage_find(d, u->storage);
    if (!s) return -ENOENT;
    *out = (node){.storage = s->id, .id = ROOT_PARENT, .is_dir = 1};
    size_t base = strlen(u->storage);
    char *key = fr_realloc(NULL, base + strlen(u->path) + 1);
    memcpy(key, u->storage, base + 1);
    int rc = 0;
    for (const char *p = u->path; !rc && *p && strcmp(p, "/");) {
        if (!out->is_dir) {
            rc = -ENOTDIR;
            break;
        }
        size_t k = strcspn(p + 1, "/") + 1, at = strlen(key);
        node *c = NULL;
        memcpy(key + at, p, k);
        key[at + k] = 0;
        if (!(c = cache_find(d, key))) {
            key[at] = 0;
            rc = list_children(d, key, out->storage, out->id, NULL);
            memcpy(key + at, p, k);
            key[at + k] = 0;
            c = rc ? NULL : cache_find(d, key);
        }
        if (!rc && !c) rc = -ENOENT;
        if (!rc) *out = *c, out->key = NULL;
        p += k;
    }
    free(key);
    return rc;
}

/* the folder that would hold u, and u's name */
static int resolve_parent(device *d, const fr_mtp_uri *u, node *out, char **name)
{
    if (!u->storage || !strcmp(u->path, "/")) return -EPERM;
    const char *slash = strrchr(u->path, '/');
    fr_mtp_uri parent = {u->device, u->storage, slash == u->path ? fr_strdup("/") : fr_strndup(u->path, (size_t)(slash - u->path))};
    int rc = resolve(d, &parent, out);
    free(parent.path);
    if (!rc && !out->is_dir) rc = -ENOTDIR;
    *name = rc ? NULL : fr_strdup(slash + 1);
    return rc;
}

static device *begin(const char *uri, fr_mtp_uri *u, int *rc)
{
    if ((*rc = fr_mtp_parse(uri, u))) return NULL;
    device *d = take(u->device);
    if (!d) {
        fr_mtp_uri_free(u);
        *rc = -ENODEV;
    }
    return d;
}

static void end(device *d, fr_mtp_uri *u)
{
    give(d);
    fr_mtp_uri_free(u);
}

/* a stale cached object (changed on the device meanwhile) fails as -EIO or -ENOENT: once more from the device */
static int retry_fresh(device *d, int rc)
{
    if (rc != -EIO && rc != -ENOENT) return 0;
    cache_clear(d);
    return 1;
}

static int mtp_list(const char *uri, fr_entries *out)
{
    fr_mtp_uri u;
    int rc;
    device *d = begin(uri, &u, &rc);
    if (!d) return rc;
    if (!u.storage) {
        if (!(rc = storages_refresh(d)))
            for (LIBMTP_devicestorage_t *s = d->mtp->storage; s; s = s->next) {
                char name[256];
                storage_name(d, s, name, sizeof name);
                fr_entry e;
                fill(&e, name, 1, 0, 0);
                FR_PUSH(*out, e);
            }
        end(d, &u);
        return rc;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        node n;
        rc = resolve(d, &u, &n);
        if (!rc && !n.is_dir) rc = -ENOTDIR;
        if (!rc) {
            char *key = !strcmp(u.path, "/") ? fr_strdup(u.storage) : fr_format("%s%s", u.storage, u.path);
            rc = list_children(d, key, n.storage, n.id, out);
            free(key);
        }
        if (!rc || attempt || !retry_fresh(d, rc)) break;
        fr_entries_free(out);
    }
    end(d, &u);
    return rc;
}

static int mtp_stat(const char *uri, fr_entry *out)
{
    fr_mtp_uri u;
    int rc;
    device *d = begin(uri, &u, &rc);
    if (!d) return rc;
    if (!u.storage) fill(out, d->label, 1, 0, 0);
    else {
        node n;
        rc = resolve(d, &u, &n);
        if (rc && retry_fresh(d, rc)) rc = resolve(d, &u, &n);
        if (!rc) {
            const char *slash = strrchr(u.path, '/');
            fill(out, slash[1] ? slash + 1 : u.storage, n.is_dir, n.size, n.mtime);
        }
    }
    end(d, &u);
    return rc;
}

/* ---- streams: a temporary file, downloaded into when opened or uploaded from when closed ---- */

typedef struct {
    fr_stream s;
    int fd;
    char *upload; /* the URI it goes to, NULL for a download */
    int exclusive;
} temp_stream;

static int temp_file(void)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/var/tmp";
    int fd = open(dir, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) return fd;
    char *path = fr_format("%s/ferret-mtp-XXXXXX", dir);
    fd = mkostemp(path, O_CLOEXEC);
    if (fd >= 0) unlink(path);
    free(path);
    return fd < 0 ? -errno : fd;
}

static long temp_read(fr_stream *s, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(((temp_stream *)s)->fd, buf, n);
        if (r >= 0) return (long)r;
        if (errno != EINTR) return -errno;
    }
}

static long temp_write(fr_stream *s, const void *buf, size_t n)
{
    temp_stream *t = (temp_stream *)s;
    if (!t->upload) return -EBADF;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(t->fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -errno;
        done += (size_t)r;
    }
    return (long)done;
}

static int upload(temp_stream *t)
{
    struct stat st;
    if (fstat(t->fd, &st) || lseek(t->fd, 0, SEEK_SET)) return -errno;
    fr_mtp_uri u;
    int rc;
    device *d = begin(t->upload, &u, &rc);
    if (!d) return rc;
    cache_clear(d);
    node parent, there;
    char *name = NULL;
    rc = resolve_parent(d, &u, &parent, &name);
    if (!rc && !resolve(d, &u, &there)) {
        if (t->exclusive) rc = -EEXIST;
        else if (there.is_dir) rc = -EISDIR;
        else if (LIBMTP_Delete_Object(d->mtp, there.id)) rc = mtp_error(d);
    }
    if (!rc) {
        LIBMTP_file_t *f = LIBMTP_new_file_t();
        f->filename = fr_strdup(name);
        f->filesize = (uint64_t)st.st_size;
        f->filetype = LIBMTP_FILETYPE_UNKNOWN;
        f->parent_id = parent.id == ROOT_PARENT ? 0 : parent.id;
        f->storage_id = parent.storage;
        if (LIBMTP_Send_File_From_File_Descriptor(d->mtp, t->fd, f, NULL, NULL)) rc = mtp_error(d);
        LIBMTP_destroy_file_t(f);
    }
    cache_clear(d);
    free(name);
    end(d, &u);
    return rc;
}

static int temp_close(fr_stream *s)
{
    temp_stream *t = (temp_stream *)s;
    int rc = t->upload ? upload(t) : 0;
    if (close(t->fd) && !rc) rc = -errno;
    free(t->upload);
    free(t);
    return rc;
}

static fr_stream *temp_stream_new(int fd, const char *upload_to, int exclusive)
{
    temp_stream *t = fr_realloc(NULL, sizeof *t);
    *t = (temp_stream){{temp_read, temp_write, temp_close}, fd, upload_to ? fr_strdup(upload_to) : NULL, exclusive};
    return &t->s;
}

static fr_stream *mtp_open_read(const char *uri, int *err)
{
    fr_mtp_uri u;
    device *d = begin(uri, &u, err);
    if (!d) return NULL;
    node n;
    int fd = -1;
    *err = u.storage ? resolve(d, &u, &n) : -EISDIR;
    if (*err && u.storage && retry_fresh(d, *err)) *err = resolve(d, &u, &n);
    if (!*err && n.is_dir) *err = -EISDIR;
    if (!*err && (fd = temp_file()) < 0) *err = fd;
    if (!*err && (LIBMTP_Get_File_To_File_Descriptor(d->mtp, n.id, fd, NULL, NULL) || lseek(fd, 0, SEEK_SET)))
        *err = mtp_error(d);
    end(d, &u);
    if (*err) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    return temp_stream_new(fd, NULL, 0);
}

/* checks what it can now; the file appears when the stream closes */
static fr_stream *mtp_open_write(const char *uri, int exclusive, int *err)
{
    fr_mtp_uri u;
    device *d = begin(uri, &u, err);
    if (!d) return NULL;
    node parent, there;
    char *name = NULL;
    *err = resolve_parent(d, &u, &parent, &name);
    if (!*err && !resolve(d, &u, &there)) *err = exclusive ? -EEXIST : there.is_dir ? -EISDIR : 0;
    free(name);
    end(d, &u);
    int fd = *err ? -1 : temp_file();
    if (fd < 0) {
        if (!*err) *err = fd;
        return NULL;
    }
    return temp_stream_new(fd, uri, exclusive);
}

static int mtp_mkdir(const char *uri)
{
    fr_mtp_uri u;
    int rc;
    device *d = begin(uri, &u, &rc);
    if (!d) return rc;
    node parent, there;
    char *name = NULL;
    rc = resolve_parent(d, &u, &parent, &name);
    if (!rc && !resolve(d, &u, &there)) rc = -EEXIST;
    if (!rc && !LIBMTP_Create_Folder(d->mtp, name, parent.id, parent.storage)) rc = mtp_error(d);
    cache_clear(d);
    free(name);
    end(d, &u);
    return rc;
}

static int mtp_rename(const char *from, const char *to)
{
    fr_mtp_uri a, b;
    int rc = fr_mtp_parse(to, &b);
    if (rc) return rc;
    device *d = begin(from, &a, &rc);
    if (!d) {
        fr_mtp_uri_free(&b);
        return rc;
    }
    node src, parent, there;
    char *name = NULL;
    if (strcmp(a.device, b.device) || !a.storage || !b.storage || strcmp(a.storage, b.storage)) rc = -EXDEV;
    if (!rc) rc = resolve(d, &a, &src);
    if (!rc && !strcmp(a.path, "/")) rc = -EPERM;
    if (!rc) rc = resolve_parent(d, &b, &parent, &name);
    if (!rc && !resolve(d, &b, &there) && there.id != src.id) rc = -EEXIST;
    if (!rc) {
        char *old_dir = fr_strndup(a.path, (size_t)(strrchr(a.path, '/') - a.path));
        char *new_dir = fr_strndup(b.path, (size_t)(strrchr(b.path, '/') - b.path));
        int moves = strcmp(old_dir, new_dir) != 0;
        free(old_dir), free(new_dir);
        /* devices without MoveObject: the caller copies, then removes */
        if (moves && LIBMTP_Move_Object(d->mtp, src.id, parent.storage, parent.id == ROOT_PARENT ? 0 : parent.id))
            mtp_error(d), rc = -EXDEV;
        if (!rc && strcmp(strrchr(a.path, '/') + 1, name) && LIBMTP_Set_Object_Filename(d->mtp, src.id, name))
            rc = mtp_error(d);
    }
    cache_clear(d);
    free(name);
    fr_mtp_uri_free(&b);
    end(d, &a);
    return rc;
}

static int mtp_remove(const char *uri)
{
    fr_mtp_uri u;
    int rc;
    device *d = begin(uri, &u, &rc);
    if (!d) return rc;
    node n;
    rc = u.storage && strcmp(u.path, "/") ? resolve(d, &u, &n) : -EPERM;
    if (!rc && n.is_dir) {
        uint32_t *kids = NULL;
        int count = LIBMTP_Get_Children(d->mtp, n.storage, n.id, &kids);
        free(kids);
        /* Android deletes a folder with everything in it */
        if (count < 0) rc = mtp_error(d);
        else if (count > 0) rc = -ENOTEMPTY;
    }
    if (!rc && LIBMTP_Delete_Object(d->mtp, n.id)) rc = mtp_error(d);
    cache_clear(d);
    end(d, &u);
    return rc;
}

static int mtp_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    fr_mtp_uri u;
    int rc;
    device *d = begin(uri, &u, &rc);
    if (!d) return rc;
    *free_bytes = *total_bytes = 0;
    rc = storages_refresh(d);
    LIBMTP_devicestorage_t *one = !rc && u.storage ? storage_find(d, u.storage) : NULL;
    if (!rc && u.storage && !one) rc = -ENOENT;
    for (LIBMTP_devicestorage_t *s = one ? one : d->mtp->storage; !rc && s; s = one ? NULL : s->next)
        *free_bytes += s->FreeSpaceInBytes, *total_bytes += s->MaxCapacity;
    end(d, &u);
    return rc;
}

const fr_backend fr_backend_mtp = {
    .scheme = "mtp",
    .list = mtp_list,
    .stat = mtp_stat,
    .open_read = mtp_open_read,
    .open_write = mtp_open_write,
    .mkdir = mtp_mkdir,
    .rename = mtp_rename,
    .remove = mtp_remove,
    .free_space = mtp_free_space,
};

/* ---- finding devices ---- */

static char *sanitized(const char *s)
{
    char *r = fr_strdup(s ? s : "");
    size_t n = 0;
    for (const char *c = r; *c; c++)
        if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '-' || *c == '_' || *c == '.')
            r[n++] = *c;
    r[n] = 0;
    return r;
}

static char *device_label(LIBMTP_mtpdevice_t *m, const LIBMTP_raw_device_t *raw)
{
    char *friendly = LIBMTP_Get_Friendlyname(m), *label = NULL;
    if (friendly && *friendly) label = fr_strdup(friendly);
    free(friendly);
    if (label) return label;
    char *maker = LIBMTP_Get_Manufacturername(m), *model = LIBMTP_Get_Modelname(m);
    if (model && *model) label = maker && *maker && !strstr(model, maker) ? fr_format("%s %s", maker, model) : fr_strdup(model);
    free(maker), free(model);
    if (label) return label;
    const LIBMTP_device_entry_t *e = &raw->device_entry;
    if (e->product) return e->vendor ? fr_format("%s %s", e->vendor, e->product) : fr_strdup(e->product);
    return fr_strdup("MTP device");
}

static int id_taken(const char *id)
{
    for (int i = 0; i < devices.n; i++)
        if (!strcmp(devices.v[i]->id, id)) return 1;
    return 0;
}

/* lock held */
static char *device_id(LIBMTP_mtpdevice_t *m, const LIBMTP_raw_device_t *raw)
{
    char *number = LIBMTP_Get_Serialnumber(m), *base = sanitized(number);
    free(number);
    if (!*base) {
        free(base);
        base = fr_format("%04x-%04x-%u-%u", raw->device_entry.vendor_id, raw->device_entry.product_id,
                         (unsigned)raw->bus_location, (unsigned)raw->devnum);
    }
    char *id = fr_strdup(base);
    for (int k = 2; id_taken(id); k++) free(id), id = fr_format("%s-%d", base, k);
    free(base);
    return id;
}

static device *device_open(LIBMTP_raw_device_t *raw)
{
    LIBMTP_mtpdevice_t *m = LIBMTP_Open_Raw_Device_Uncached(raw);
    if (!m) return NULL;
    device *d = fr_realloc(NULL, sizeof *d);
    *d = (device){.label = device_label(m, raw), .bus = raw->bus_location, .devnum = raw->devnum, .mtp = m, .refs = 1};
    pthread_mutex_init(&d->io, NULL);
    if (!LIBMTP_Get_Storage(m, LIBMTP_STORAGE_SORTBY_NOTSORTED))
        for (LIBMTP_devicestorage_t *s = m->storage; s; s = s->next) d->size += s->MaxCapacity;
    LIBMTP_Clear_Errorstack(m);
    return d;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* the USB device nodes there are: a cheap way to tell that something was plugged in or out */
static char *usb_nodes(void)
{
    fr_strs names = {0};
    DIR *top = opendir("/dev/bus/usb");
    struct dirent *bus, *dev;
    while (top && (bus = readdir(top))) {
        if (bus->d_name[0] == '.') continue;
        char *path = fr_format("/dev/bus/usb/%s", bus->d_name);
        DIR *b = opendir(path);
        while (b && (dev = readdir(b)))
            if (dev->d_name[0] != '.') FR_PUSH(names, fr_format("%s/%s", bus->d_name, dev->d_name));
        if (b) closedir(b);
        free(path);
    }
    if (top) closedir(top);
    if (names.n) qsort(names.v, (size_t)names.n, sizeof *names.v, by_name);
    size_t len = 1;
    for (int i = 0; i < names.n; i++) len += strlen(names.v[i]) + 1;
    char *r = fr_realloc(NULL, len), *p = r;
    for (int i = 0; i < names.n; i++) p += sprintf(p, "%s;", names.v[i]);
    *p = 0;
    fr_strs_free(&names);
    return r;
}

/* 1 when a device is there that could not be opened (another program holds it, or it is still starting) */
static int rescan(void)
{
    LIBMTP_raw_device_t *raw = NULL;
    int n = 0;
    LIBMTP_error_number_t e = LIBMTP_Detect_Raw_Devices(&raw, &n);
    if (e != LIBMTP_ERROR_NONE && e != LIBMTP_ERROR_NO_DEVICE_ATTACHED) {
        free(raw);
        return 1;
    }
    if (e == LIBMTP_ERROR_NO_DEVICE_ATTACHED) n = 0;
    int changed = 0, failed = 0;
    struct {
        device **v;
        int n, cap;
    } gone = {0};
    pthread_mutex_lock(&lock);
    for (int i = devices.n - 1; i >= 0; i--) {
        int there = 0;
        for (int k = 0; k < n && !there; k++) there = raw[k].bus_location == devices.v[i]->bus && raw[k].devnum == devices.v[i]->devnum;
        if (there) continue;
        FR_PUSH(gone, devices.v[i]);
        memmove(devices.v + i, devices.v + i + 1, sizeof *devices.v * (size_t)(devices.n - i - 1));
        devices.n--;
        changed = 1;
    }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < gone.n; i++) unref(gone.v[i]);
    free(gone.v);
    for (int k = 0; k < n; k++) {
        int known = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < devices.n && !known; i++) known = raw[k].bus_location == devices.v[i]->bus && raw[k].devnum == devices.v[i]->devnum;
        pthread_mutex_unlock(&lock);
        if (known) continue;
        device *d = device_open(&raw[k]);
        if (!d) {
            failed = 1;
            continue;
        }
        pthread_mutex_lock(&lock);
        d->id = device_id(d->mtp, &raw[k]);
        FR_PUSH(devices, d);
        pthread_mutex_unlock(&lock);
        changed = 1;
    }
    free(raw);
    if (changed) {
        pthread_mutex_lock(&lock);
        serial++;
        void (*wake)(void) = wake_fn;
        pthread_mutex_unlock(&lock);
        if (wake) wake();
    }
    return failed;
}

static void *scan(void *arg)
{
    LIBMTP_Init();
    char *seen = NULL;
    double retry_at = 0;
    for (;;) {
        char *now = usb_nodes();
        if (!seen || strcmp(now, seen) || (retry_at && fr_now() >= retry_at)) retry_at = rescan() ? fr_now() + RETRY_SECONDS : 0;
        free(seen);
        seen = now;
        sleep(SCAN_SECONDS);
    }
    return NULL;
}

static void start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, scan, NULL)) pthread_detach(t);
}

void fr_mtp_start(void (*wake)(void))
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_mutex_lock(&lock);
    if (!wake_fn) wake_fn = wake;
    pthread_mutex_unlock(&lock);
    pthread_once(&once, start);
}

unsigned fr_mtp_serial(void)
{
    pthread_mutex_lock(&lock);
    unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}

void fr_mtp_list_devices(fr_mtp_devices *out)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < devices.n; i++) {
        fr_mtp_device m = {fr_format("mtp://%s/", devices.v[i]->id), fr_strdup(devices.v[i]->label), devices.v[i]->size};
        FR_PUSH(*out, m);
    }
    pthread_mutex_unlock(&lock);
}

void fr_mtp_devices_free(fr_mtp_devices *d)
{
    for (int i = 0; i < d->n; i++) free(d->v[i].uri), free(d->v[i].label);
    free(d->v);
    *d = (fr_mtp_devices){0};
}
