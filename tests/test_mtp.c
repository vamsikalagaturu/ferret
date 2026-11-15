#include "check.h"
#include "vfs/vfs_mtp.h"
#include <errno.h>
#include <stdlib.h>

static void parses(const char *uri, const char *device, const char *storage, const char *path)
{
    fr_mtp_uri u;
    int rc = fr_mtp_parse(uri, &u);
    CHECK(rc == 0);
    if (rc) {
        fprintf(stderr, "  for %s\n", uri);
        return;
    }
    CHECK_STR(u.device, device);
    if (storage) CHECK_STR(u.storage, storage);
    else CHECK(u.storage == NULL);
    CHECK_STR(u.path, path);
    fr_mtp_uri_free(&u);
}

static void rejects(const char *uri)
{
    fr_mtp_uri u;
    CHECK(fr_mtp_parse(uri, &u) == -EINVAL);
    CHECK(!u.device && !u.storage && !u.path);
}

int main(void)
{
    parses("mtp://R58M12ABC", "R58M12ABC", NULL, "/");
    parses("mtp://R58M12ABC/", "R58M12ABC", NULL, "/");
    parses("mtp://R58M12ABC//", "R58M12ABC", NULL, "/");
    parses("mtp://R58M12ABC/Internal shared storage", "R58M12ABC", "Internal shared storage", "/");
    parses("mtp://R58M12ABC/Internal shared storage/", "R58M12ABC", "Internal shared storage", "/");
    parses("mtp://dev/SD card/DCIM/Camera", "dev", "SD card", "/DCIM/Camera");
    parses("mtp://dev/SD card/DCIM/Camera/", "dev", "SD card", "/DCIM/Camera");
    parses("mtp://dev/s//a///b c.jpg", "dev", "s", "/a/b c.jpg");
    parses("mtp://dev/storage-00010001/.hidden/x", "dev", "storage-00010001", "/.hidden/x");
    parses("mtp://dev/s/a..b/...", "dev", "s", "/a..b/...");

    rejects("file:///home");
    rejects("mtp:/dev/s");
    rejects("mtp://");
    rejects("mtp:///s/a");
    rejects("mtp://dev/s/../etc");
    rejects("mtp://dev/s/a/./b");
    rejects("mtp://dev/../s");
    rejects("mtp://dev/./a");

    CHECK(fr_vfs_backend("mtp://dev/s/a") == &fr_backend_mtp);
    char *p = fr_uri_parent("mtp://dev/s/DCIM");
    CHECK_STR(p, "mtp://dev/s");
    free(p);
    p = fr_uri_parent("mtp://dev/s");
    CHECK_STR(p, "mtp://dev/");
    free(p);
    CHECK(fr_uri_parent("mtp://dev/") == NULL);
    p = fr_uri_name("mtp://dev/s/DCIM/a.jpg");
    CHECK_STR(p, "a.jpg");
    free(p);
    CHECK(fr_uri_within("mtp://dev/", "mtp://dev/s/DCIM"));
    CHECK(!fr_uri_within("mtp://dev/", "mtp://device/s"));
    CHECK(fr_uri_same_backend("mtp://dev/s/a", "mtp://dev/t/b"));
    CHECK(!fr_uri_same_backend("mtp://dev/s/a", "mtp://other/s/a"));
    CHECK(fr_vfs_rename("mtp://dev/s/a", "mtp://other/s/a") == -EXDEV);
    CHECK(fr_vfs_rename("mtp://dev/s/a", "file:///tmp/a") == -EXDEV);

    /* nothing is plugged in for the tests, and nothing is looked for until fr_mtp_start() */
    fr_entries es = {0};
    CHECK(fr_vfs_list("mtp://dev/s", &es) == -ENODEV);
    CHECK(es.n == 0);
    CHECK(fr_vfs_list("mtp://dev/s/..", &es) == -EINVAL);
    fr_entry e = {0};
    CHECK(fr_vfs_stat("mtp://dev/", &e) == -ENODEV);
    int err = 0;
    CHECK(fr_vfs_open_read("mtp://dev/s/a", &err) == NULL && err == -ENODEV);
    CHECK(fr_vfs_open_write("mtp://dev/s/a", 1, &err) == NULL && err == -ENODEV);
    CHECK(fr_vfs_mkdir("mtp://dev/s/new") == -ENODEV);
    CHECK(fr_vfs_remove("mtp://dev/s/a") == -ENODEV);
    CHECK(!fr_vfs_can_trash("mtp://dev/s/a"));
    CHECK(fr_vfs_set_mode("mtp://dev/s/a", 0644) == -ENOTSUP);
    fr_mtp_devices devs = {0};
    fr_mtp_list_devices(&devs);
    CHECK(devs.n == 0);
    fr_mtp_devices_free(&devs);
    return CHECK_DONE();
}
