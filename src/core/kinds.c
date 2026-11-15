#include "kinds.h"
#include "util.h"
#include <string.h>

static const struct {
    fr_kind kind;
    const char *exts;
} table[] = {
    {KIND_IMAGE, " jpg jpeg png gif webp bmp svg heic heif tif tiff avif ico raw cr2 nef arw dng psd xcf jxl "},
    {KIND_VIDEO, " mp4 mkv webm mov avi m4v 3gp wmv flv mpg mpeg ts "},
    {KIND_AUDIO, " mp3 flac ogg opus wav m4a aac wma mid midi amr "},
    {KIND_ARCHIVE, " zip tar gz tgz bz2 tbz xz txz 7z rar zst lz4 lzma iso apk jar deb rpm cab "},
    {KIND_DOCUMENT, " pdf doc docx odt rtf epub mobi djvu xps "},
    {KIND_SHEET, " xls xlsx ods csv tsv "},
    {KIND_SLIDES, " ppt pptx odp key "},
    {KIND_CODE, " c h cc cpp hpp py js ts jsx tsx json xml html htm css sh bash zsh rs go java kt swift rb php lua sql yaml yml toml ini cfg conf cmake mk tex nix "},
    {KIND_TEXT, " txt md log rst org "},
};

fr_kind kind_of(const char *name, int is_dir)
{
    if (is_dir) return KIND_FOLDER;
    const char *e = fr_ext(name);
    size_t n = strlen(e);
    if (!n || n > 8) return KIND_OTHER;
    char key[12] = " ";
    for (size_t i = 0; i < n; i++) key[i + 1] = (char)(e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (strstr(table[i].exts, key)) return table[i].kind;
    return KIND_OTHER;
}

int kind_filter_shows(fr_kind_filter f, fr_kind k)
{
    switch (f) {
    case KF_ALL: return 1;
    case KF_IMAGES: return k == KIND_FOLDER || k == KIND_IMAGE;
    case KF_VIDEO: return k == KIND_FOLDER || k == KIND_VIDEO;
    case KF_AUDIO: return k == KIND_FOLDER || k == KIND_AUDIO;
    case KF_DOCUMENTS: return k == KIND_FOLDER || k == KIND_DOCUMENT || k == KIND_SHEET || k == KIND_SLIDES || k == KIND_TEXT;
    case KF_ARCHIVES: return k == KIND_FOLDER || k == KIND_ARCHIVE;
    case KF_CODE: return k == KIND_FOLDER || k == KIND_CODE;
    default: return 1;
    }
}

const char *kind_filter_label(fr_kind_filter f)
{
    static const char *const labels[KF_COUNT] = {"All", "Images", "Video", "Audio", "Documents", "Archives", "Code"};
    return f >= 0 && f < KF_COUNT ? labels[f] : labels[0];
}
