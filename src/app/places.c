#include "platform.h"
#include <stdlib.h>

void fr_places_free(fr_places *p)
{
    for (int i = 0; i < p->n; i++) free(p->v[i].label), free(p->v[i].uri), free(p->v[i].icon);
    free(p->v);
    *p = (fr_places){0};
}
