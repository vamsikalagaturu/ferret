/* Server passwords in the desktop's keystore through libsecret (the Secret Service: GNOME Keyring, KWallet). Built
 * without libsecret there is none, and a password lasts until ferret quits. */
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef FR_HAVE_LIBSECRET
#include <libsecret/secret.h>

/* one item per server key; the user name rides along as an attribute, so the item names the account it opens */
static const SecretSchema schema = {
    .name = "io.github.vamsikalagaturu.ferret.Server",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"server", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"user", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
};

static int failed(GError *err)
{
    if (!err) return 0;
    fprintf(stderr, "ferret: keystore: %s\n", err->message);
    g_error_free(err);
    return 1;
}

int plat_secret_available(void) { return 1; }

int plat_secret_load(const char *key, char **user, char **password)
{
    GError *err = NULL;
    GHashTable *attrs = secret_attributes_build(&schema, "server", key, NULL);
    GList *found = secret_password_searchv_sync(&schema, attrs, SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS, NULL, &err);
    g_hash_table_unref(attrs);
    int rc = -1;
    if (!failed(err) && found) {
        SecretRetrievable *item = found->data;
        GHashTable *a = secret_retrievable_get_attributes(item);
        SecretValue *v = secret_retrievable_retrieve_secret_sync(item, NULL, &err);
        const char *u = a ? g_hash_table_lookup(a, "user") : NULL, *p = v ? secret_value_get_text(v) : NULL;
        if (!failed(err) && u && p) {
            *user = fr_strdup(u), *password = fr_strdup(p);
            rc = 0;
        }
        if (v) secret_value_unref(v);
        if (a) g_hash_table_unref(a);
    }
    g_list_free_full(found, g_object_unref);
    return rc;
}

int plat_secret_save(const char *key, const char *user, const char *password)
{
    GError *err = NULL;
    secret_password_clear_sync(&schema, NULL, &err, "server", key, NULL);
    failed(err), err = NULL;
    char *label = fr_format("ferret: %s", key);
    secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, label, password, NULL, &err, "server", key, "user", user, NULL);
    free(label);
    return failed(err) ? -1 : 0;
}

int plat_secret_forget(const char *key)
{
    GError *err = NULL;
    secret_password_clear_sync(&schema, NULL, &err, "server", key, NULL);
    return failed(err) ? -1 : 0;
}
#else
int plat_secret_available(void) { return 0; }
int plat_secret_load(const char *key, char **user, char **password) { return -1; }
int plat_secret_save(const char *key, const char *user, const char *password) { return -1; }
int plat_secret_forget(const char *key) { return -1; }
#endif
