#ifndef FR_DISKS_H
#define FR_DISKS_H
/* The disk manager's model, without a window or D-Bus: UDisks2's objects as plain structs (dk_obj, filled by
 * platform/linux/disks_linux.c), and from them the disks as the map shows them (dk_disk): partitions and free gaps in
 * disk order, each partition tagged with the operating system it belongs to. */
#include "core/util.h"
#include <stdint.h>

#define DK_MIB (1024ull * 1024)
/* smaller gaps are alignment leftovers (GPT backup header, 1 MiB alignment), not space anyone can use */
#define DK_MIN_FREE (16 * DK_MIB)

/* libblockdev BDFsResizeFlags, as UDisks2 Manager.CanResize returns them */
enum { DK_OFFLINE_SHRINK = 1 << 1, DK_OFFLINE_GROW = 1 << 2, DK_ONLINE_SHRINK = 1 << 3, DK_ONLINE_GROW = 1 << 4 };

/* ---- UDisks2's objects ---- */
typedef struct {
    char *path;
    int drive, block, partition, table, fs, encrypted, swap, loop, job; /* the interfaces it has */
    /* Drive */
    char *vendor, *model, *serial, *sort_key, *bus;
    int removable, ejectable, can_power_off;
    /* Block */
    char *device, *drive_path, *backing, *id_type, *id_label, *id_uuid, *id_usage;
    uint64_t size;
    int hint_system, hint_ignore;
    /* Partition */
    char *table_path, *part_type, *part_uuid, *part_name;
    uint64_t part_offset, part_size;
    unsigned number;
    int container;
    /* PartitionTable */
    char *table_type;
    /* Filesystem, Swapspace, Encrypted */
    fr_strs mounts;
    int swap_active;
    char *cleartext;
    /* Job */
    char *job_operation;
    double job_progress; /* -1 not known */
} dk_obj;

typedef struct {
    dk_obj *v;
    int n, cap;
} dk_objs;

typedef struct {
    char *fs;
    uint64_t modes; /* DK_*_SHRINK / GROW */
} dk_resize_mode;

/* everything the model is built from */
typedef struct {
    dk_objs objs;
    struct {
        dk_resize_mode *v;
        int n, cap;
    } resize;
    fr_strs formats; /* filesystems UDisks2 can make here, sorted */
    char *version;   /* of the UDisks2 daemon */
} dk_tree;

void dk_obj_free(dk_obj *o);
dk_obj dk_obj_copy(const dk_obj *o);
void dk_objs_free(dk_objs *o);
void dk_tree_free(dk_tree *t);
dk_tree dk_tree_copy(const dk_tree *t);
const dk_obj *dk_find(const dk_objs *o, const char *path);
/* the drive a block sits on, through an unlocked volume's encrypted backing; NULL none */
const dk_obj *dk_drive_of(const dk_objs *o, const dk_obj *b);
char *dk_root_drive(const dk_objs *o); /* the drive holding "/", NULL none */

/* ---- where operating systems are ---- */
typedef struct {
    char *name, *part_uuid; /* the ESP it boots from, lower case */
    int active;
} dk_boot;

typedef struct {
    struct {
        dk_boot *v;
        int n, cap;
    } boot;
    char *running; /* PRETTY_NAME of the running system */
    fr_strs swaps; /* active swap devices */
} dk_env;

/* a UEFI Boot#### variable as efivarfs holds it (4 attribute bytes, then EFI_LOAD_OPTION); 0 when it names an ESP */
int dk_boot_parse(const unsigned char *d, size_t n, dk_boot *out);
void dk_boot_read(const char *efivars_dir, dk_env *env); /* every Boot#### there */
void dk_swaps_read(const char *proc_swaps, fr_strs *out);
char *dk_os_release(const char *root); /* PRETTY_NAME under root, NULL none */
void dk_env_read(dk_env *env);         /* this machine's */
void dk_env_free(dk_env *env);

/* ---- the disks ---- */
enum { SEG_FREE, SEG_PARTITION, SEG_FILESYSTEM, SEG_IMAGE };
enum { GROUP_UNKNOWN, GROUP_RUNNING, GROUP_WINDOWS, GROUP_OS, GROUP_NEW };
enum { CHANGE_NONE, CHANGE_CREATE, CHANGE_DELETE, CHANGE_FORMAT, CHANGE_LABEL, CHANGE_RESIZE, CHANGE_IMAGE };

typedef struct {
    char *key; /* object path, "free:<offset>", "new:<n>", "image" */
    int kind;
    char *path, *device;
    unsigned number;
    uint64_t offset, size;
    char *fs_type, *label, *uuid, *part_uuid, *part_name, *part_type;
    const char *type_name;
    int has_fs, encrypted, system_mount, swap_active;
    fr_strs mounts;
    uint64_t resize_modes;
    int has_used;
    uint64_t used, used_total;
    char *group;
    int group_kind, inferred;
    int change;
    char *was; /* freed space: what was there */
} dk_seg;

typedef struct {
    dk_seg *v;
    int n, cap;
} dk_segs;

typedef struct {
    char *path, *block, *name, *device, *serial, *table_type, *connection;
    uint64_t size;
    int removable, ejectable, can_power_off, root_disk;
    int imaged; /* the plan writes an image over all of it */
    dk_segs segs;
} dk_disk;

typedef struct {
    dk_disk *v;
    int n, cap;
} dk_disks;

/* the disks in UDisks2's SortKey order; mounted filesystems' use comes from statvfs on their mount point */
void dk_build(const dk_tree *t, const dk_env *env, dk_disks *out);
void dk_seg_free(dk_seg *s);
dk_seg dk_seg_copy(const dk_seg *s);
dk_seg dk_free_seg(uint64_t offset, uint64_t size);
void dk_disk_free(dk_disk *d);
dk_disk dk_disk_copy(const dk_disk *d);
void dk_disks_free(dk_disks *d);
dk_disks dk_disks_copy(const dk_disks *d);
int dk_disk_index(const dk_disks *d, const char *path);
int dk_seg_index(const dk_segs *s, const char *key);
const char *dk_type_name(const char *part_type); /* "Linux filesystem", or the type itself */
const char *dk_seg_name(const dk_seg *s);         /* label, partition name, or the device without /dev/ */

/* "1.8 TiB", "512 MiB", "900 B": IEC, as partitions are sized */
void dk_size_text(uint64_t bytes, char *buf, size_t len);
/* "200G", "1.5 TiB", "512 MiB" -> bytes; -1 unreadable */
int64_t dk_parse_size(const char *text);

/* Why UDisks2 must not run method on object now, on live state, under the plan's own checks: nothing destructive on
 * what is mounted or active swap, no raw writes to, ejecting or powering off of the running system's disk, no
 * unmounting the system's own mounts. NULL when allowed. */
const char *dk_refusal(const dk_objs *o, const char *root_drive, const char *object, const char *method);
#endif
