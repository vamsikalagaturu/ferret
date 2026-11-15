#ifndef FR_KINDS_H
#define FR_KINDS_H
/* What a file is, by its name's extension, and the groups the type filter shows. */

typedef enum { KIND_FOLDER, KIND_IMAGE, KIND_VIDEO, KIND_AUDIO, KIND_ARCHIVE, KIND_DOCUMENT, KIND_SHEET, KIND_SLIDES, KIND_CODE, KIND_TEXT, KIND_OTHER } fr_kind;
fr_kind kind_of(const char *name, int is_dir);

typedef enum { KF_ALL, KF_IMAGES, KF_VIDEO, KF_AUDIO, KF_DOCUMENTS, KF_ARCHIVES, KF_CODE, KF_COUNT } fr_kind_filter;
/* a file of kind k shows under filter f; folders always do, so the filter does not stop the way down */
int kind_filter_shows(fr_kind_filter f, fr_kind k);
const char *kind_filter_label(fr_kind_filter f); /* "Images", ... ("All" for KF_ALL) */
#endif
