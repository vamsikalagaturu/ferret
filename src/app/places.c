#include "platform.h"
#include <stdlib.h>

void fr_places_free(fr_places *p)
{
    for (int i = 0; i < p->n; i++) free(p->v[i].label), free(p->v[i].uri), free(p->v[i].icon);
    free(p->v);
    *p = (fr_places){0};
}

void fr_disks_free(fr_disks *d)
{
    for (int i = 0; i < d->n; i++)
        free(d->v[i].id), free(d->v[i].label), free(d->v[i].device), free(d->v[i].uri), free(d->v[i].drive), fr_strs_free(&d->v[i].mounts);
    free(d->v);
    *d = (fr_disks){0};
}

void fr_disk_done_free(fr_disk_done *d)
{
    free(d->id), free(d->label), free(d->path), free(d->error);
    fr_strs_free(&d->unmounted);
    *d = (fr_disk_done){0};
}
