#ifndef FR_ICONS_H
#define FR_ICONS_H
/* The Lucide SVG files compiled in (CMakeLists.txt), ending with a NULL name. */
typedef struct {
    const char *name;
    const unsigned char *data;
    const int *size;
} fr_icon_data;

extern const fr_icon_data fr_icon_files[];
#endif
