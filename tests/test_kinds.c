/* src/core/kinds.c: kinds by extension, and what each type filter shows */
#include "check.h"
#include "core/kinds.h"

int main(void)
{
    CHECK(kind_of("photo.JPG", 0) == KIND_IMAGE);
    CHECK(kind_of("clip.mkv", 0) == KIND_VIDEO);
    CHECK(kind_of("song.flac", 0) == KIND_AUDIO);
    CHECK(kind_of("box.tar.gz", 0) == KIND_ARCHIVE);
    CHECK(kind_of("paper.pdf", 0) == KIND_DOCUMENT);
    CHECK(kind_of("main.c", 0) == KIND_CODE);
    CHECK(kind_of("notes.md", 0) == KIND_TEXT);
    CHECK(kind_of("Makefile", 0) == KIND_OTHER);
    CHECK(kind_of("photo.jpg", 1) == KIND_FOLDER);
    CHECK(kind_of(".bashrc", 0) == KIND_OTHER);

    for (int k = 0; k <= KIND_OTHER; k++) CHECK(kind_filter_shows(KF_ALL, (fr_kind)k));
    for (int f = 0; f < KF_COUNT; f++) CHECK(kind_filter_shows((fr_kind_filter)f, KIND_FOLDER));
    CHECK(kind_filter_shows(KF_IMAGES, kind_of("a.png", 0)) && !kind_filter_shows(KF_IMAGES, kind_of("a.mp4", 0)));
    CHECK(kind_filter_shows(KF_VIDEO, kind_of("a.webm", 0)) && !kind_filter_shows(KF_VIDEO, kind_of("a.mp3", 0)));
    CHECK(kind_filter_shows(KF_AUDIO, kind_of("a.opus", 0)) && !kind_filter_shows(KF_AUDIO, kind_of("a.txt", 0)));
    /* documents take sheets, slides and plain text too */
    CHECK(kind_filter_shows(KF_DOCUMENTS, kind_of("a.docx", 0)) && kind_filter_shows(KF_DOCUMENTS, kind_of("a.ods", 0)));
    CHECK(kind_filter_shows(KF_DOCUMENTS, kind_of("a.pptx", 0)) && kind_filter_shows(KF_DOCUMENTS, kind_of("a.txt", 0)));
    CHECK(!kind_filter_shows(KF_DOCUMENTS, kind_of("a.c", 0)));
    CHECK(kind_filter_shows(KF_ARCHIVES, kind_of("a.7z", 0)) && !kind_filter_shows(KF_ARCHIVES, kind_of("a.pdf", 0)));
    CHECK(kind_filter_shows(KF_CODE, kind_of("a.rs", 0)) && !kind_filter_shows(KF_CODE, kind_of("a.md", 0)));
    CHECK(!kind_filter_shows(KF_IMAGES, KIND_OTHER));
    CHECK_STR(kind_filter_label(KF_DOCUMENTS), "Documents");
    CHECK_STR(kind_filter_label(KF_ALL), "All");
    return CHECK_DONE();
}
