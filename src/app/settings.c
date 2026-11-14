#include "app.h"
#include <stdlib.h>
#include <string.h>

fr_settings settings;

void settings_load(void) {}

void settings_poll(void) {}

int favorite_index(const char *uri)
{
    for (int i = 0; i < settings.favorites.n; i++)
        if (!strcmp(settings.favorites.v[i], uri)) return i;
    return -1;
}

void favorite_toggle(const char *uri)
{
    int i = favorite_index(uri);
    if (i < 0) {
        FR_PUSH(settings.favorites, fr_strdup(uri));
        return;
    }
    free(settings.favorites.v[i]);
    memmove(settings.favorites.v + i, settings.favorites.v + i + 1, sizeof *settings.favorites.v * (size_t)(settings.favorites.n - i - 1));
    settings.favorites.n--;
}
