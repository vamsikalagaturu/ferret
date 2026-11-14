#include "app.h"
#include <string.h>
#include <strings.h>

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

/* IANA media types (iana.org/assignments/media-types), the common x- ones where none is registered */
static const char *const mimes[][2] = {
    {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"gif", "image/gif"}, {"webp", "image/webp"},
    {"bmp", "image/bmp"}, {"svg", "image/svg+xml"}, {"heic", "image/heic"}, {"heif", "image/heif"},
    {"tif", "image/tiff"}, {"tiff", "image/tiff"}, {"avif", "image/avif"}, {"ico", "image/vnd.microsoft.icon"},
    {"psd", "image/vnd.adobe.photoshop"}, {"jxl", "image/jxl"}, {"xcf", "image/x-xcf"},
    {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mkv", "video/x-matroska"}, {"webm", "video/webm"},
    {"mov", "video/quicktime"}, {"avi", "video/x-msvideo"}, {"3gp", "video/3gpp"}, {"wmv", "video/x-ms-wmv"},
    {"flv", "video/x-flv"}, {"mpg", "video/mpeg"}, {"mpeg", "video/mpeg"}, {"ts", "video/mp2t"},
    {"mp3", "audio/mpeg"}, {"flac", "audio/flac"}, {"ogg", "audio/ogg"}, {"opus", "audio/opus"}, {"wav", "audio/wav"},
    {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"wma", "audio/x-ms-wma"}, {"mid", "audio/midi"},
    {"midi", "audio/midi"}, {"amr", "audio/amr"},
    {"zip", "application/zip"}, {"tar", "application/x-tar"}, {"gz", "application/gzip"}, {"tgz", "application/gzip"},
    {"bz2", "application/x-bzip2"}, {"xz", "application/x-xz"}, {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"}, {"zst", "application/zstd"}, {"iso", "application/x-iso9660-image"},
    {"apk", "application/vnd.android.package-archive"}, {"jar", "application/java-archive"},
    {"deb", "application/vnd.debian.binary-package"}, {"rpm", "application/x-rpm"},
    {"pdf", "application/pdf"}, {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"odt", "application/vnd.oasis.opendocument.text"}, {"rtf", "application/rtf"}, {"epub", "application/epub+zip"},
    {"djvu", "image/vnd.djvu"}, {"xls", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet"}, {"csv", "text/csv"},
    {"tsv", "text/tab-separated-values"}, {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"odp", "application/vnd.oasis.opendocument.presentation"},
    {"c", "text/x-c"}, {"h", "text/x-c"}, {"cc", "text/x-c++"}, {"cpp", "text/x-c++"}, {"hpp", "text/x-c++"},
    {"py", "text/x-python"}, {"js", "text/javascript"}, {"json", "application/json"}, {"xml", "application/xml"},
    {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"sh", "application/x-shellscript"},
    {"rs", "text/rust"}, {"go", "text/x-go"}, {"java", "text/x-java"}, {"yaml", "application/yaml"},
    {"yml", "application/yaml"}, {"toml", "application/toml"}, {"sql", "application/sql"}, {"tex", "text/x-tex"},
    {"txt", "text/plain"}, {"log", "text/plain"}, {"md", "text/markdown"}, {"rst", "text/x-rst"},
};

const char *mime_of(const char *name, int is_dir)
{
    if (is_dir) return "inode/directory";
    const char *e = fr_ext(name);
    for (size_t i = 0; *e && i < sizeof mimes / sizeof *mimes; i++)
        if (!strcasecmp(e, mimes[i][0])) return mimes[i][1];
    return "application/octet-stream";
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
