#include "dirsize.h"
#include <stdlib.h>

fr_dirsize *fr_dirsize_new(const fr_strs *uris)
{
    fr_dirsize *d = fr_realloc(NULL, sizeof *d);
    *d = (fr_dirsize){.uris = fr_strs_copy(uris)};
    d->counts = fr_realloc(NULL, sizeof *d->counts * (size_t)(uris->n ? uris->n : 1));
    for (int i = 0; i < uris->n; i++) d->counts[i] = (fr_tree_count){0};
    return d;
}

void fr_dirsize_work(void *arg)
{
    fr_dirsize *d = arg;
    for (int i = 0; i < d->uris.n && !__atomic_load_n(&d->cancel, __ATOMIC_RELAXED); i++) {
        fr_vfs_measure(d->uris.v[i], &d->counts[i], &d->cancel);
        if (!__atomic_load_n(&d->cancel, __ATOMIC_RELAXED)) __atomic_add_fetch(&d->finished, 1, __ATOMIC_RELEASE);
    }
}

void fr_dirsize_free(fr_dirsize *d)
{
    if (!d) return;
    fr_strs_free(&d->uris);
    free(d->counts);
    free(d);
}
