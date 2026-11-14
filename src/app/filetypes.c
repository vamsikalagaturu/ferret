#include "app.h"
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

const char *kind_icon(fr_kind k)
{
    static const char *icons[] = {"folder",           "file-image", "file-video", "file-audio",
                                  "file-archive",     "file-text",  "file-spreadsheet", "presentation",
                                  "file-code",        "file-text",  "file"};
    return icons[k];
}

Color kind_color(fr_kind k)
{
    switch (k) {
    case KIND_FOLDER: return theme.folder;
    case KIND_IMAGE: return theme.image;
    case KIND_VIDEO: return theme.video;
    case KIND_AUDIO: return theme.audio;
    case KIND_ARCHIVE: return theme.archive;
    case KIND_DOCUMENT: return theme.document;
    case KIND_SHEET: return theme.sheet;
    case KIND_SLIDES: return theme.slides;
    case KIND_CODE: return theme.code;
    default: return theme.other;
    }
}

const char *kind_label(fr_kind k)
{
    static const char *labels[] = {"Folder", "Image",       "Video", "Audio", "Archive", "Document",
                                   "Spreadsheet", "Presentation", "Code",  "Text",  "File"};
    return labels[k];
}
