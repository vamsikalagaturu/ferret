package io.github.vamsikalagaturu.ferret;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.system.OsConstants;
import java.io.FileNotFoundException;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/* The Storage Access Framework side of platform/android/saf.c: removable volumes, their granted trees and the
 * document operations on them. A path is the one under the tree ("/" its top); results are strings, "-<errno>" for
 * a failure. Called from any native thread. */
final class Saf {
    static final int REQUEST_TREE = 0x5af;
    private static final String EXTERNAL = "com.android.externalstorage.documents";
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS};

    private final Activity act;
    private final Set<String> granted = ConcurrentHashMap.newKeySet();
    private volatile boolean picking;

    private static final class Errno extends Exception {
        final int n;
        Errno(int n) { this.n = n; }
    }

    Saf(Activity act) {
        this.act = act;
        for (UriPermission p : resolver().getPersistedUriPermissions())
            if (p.isReadPermission() && p.isWritePermission()) granted.add(p.getUri().toString());
    }

    private ContentResolver resolver() { return act.getContentResolver(); }

    /* the volume's tree chosen last, kept per volume UUID */
    private SharedPreferences prefs() { return act.getSharedPreferences("saf", Activity.MODE_PRIVATE); }

    private static String fail(Exception e) {
        int n = e instanceof Errno ? ((Errno) e).n
            : e instanceof FileNotFoundException ? OsConstants.ENOENT
            : e instanceof SecurityException ? OsConstants.EACCES
            : e instanceof UnsupportedOperationException ? OsConstants.ENOTSUP
            : OsConstants.EIO;
        return "-" + n;
    }

    /* ---- volumes and grants ---- */

    /* "<label>\t<tree URI>\n" for each mounted removable volume: its granted tree, else the tree of its top */
    String volumes() {
        StringBuilder out = new StringBuilder();
        StorageManager sm = act.getSystemService(StorageManager.class);
        for (StorageVolume v : sm.getStorageVolumes()) {
            String uuid = v.getUuid(), state = v.getState();
            if (!v.isRemovable() || uuid == null) continue;
            if (!Environment.MEDIA_MOUNTED.equals(state) && !Environment.MEDIA_MOUNTED_READ_ONLY.equals(state)) continue;
            String tree = prefs().getString(uuid, null);
            if (tree == null || !granted.contains(tree))
                tree = DocumentsContract.buildTreeDocumentUri(EXTERNAL, uuid + ":").toString();
            String label = v.getDescription(act).replace('\t', ' ').replace('\n', ' ');
            out.append(label).append('\t').append(tree).append('\n');
        }
        return out.toString();
    }

    String isGranted(String tree) { return granted.contains(tree) ? "0" : "-" + OsConstants.EACCES; }

    private static String volumeOf(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        return colon < 0 ? id : id.substring(0, colon);
    }

    /* the system's folder picker, opened at the tree's volume; once at a time */
    String request(String tree) {
        if (picking) return null;
        Intent pick = null;
        String uuid = volumeOf(Uri.parse(tree));
        for (StorageVolume v : act.getSystemService(StorageManager.class).getStorageVolumes())
            if (uuid.equals(v.getUuid())) pick = v.createOpenDocumentTreeIntent();
        if (pick == null) return null;
        picking = true;
        final Intent intent = pick;
        act.runOnUiThread(() -> {
            try {
                act.startActivityForResult(intent, REQUEST_TREE);
            } catch (ActivityNotFoundException e) {
                picking = false;
            }
        });
        return "0";
    }

    void onResult(int result, Intent data) {
        picking = false;
        Uri tree = data == null ? null : data.getData();
        if (result != Activity.RESULT_OK || tree == null) return;
        try {
            resolver().takePersistableUriPermission(tree,
                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (SecurityException e) {
            return;
        }
        granted.add(tree.toString());
        prefs().edit().putString(volumeOf(tree), tree.toString()).apply();
    }

    /* ---- documents ---- */

    private static String strip(String path) {
        int a = 0, b = path.length();
        while (a < b && path.charAt(a) == '/') a++;
        while (b > a && path.charAt(b - 1) == '/') b--;
        return path.substring(a, b);
    }

    private static String parentOf(String rel) {
        int slash = rel.lastIndexOf('/');
        return slash < 0 ? "" : rel.substring(0, slash);
    }

    private static String nameOf(String rel) { return rel.substring(rel.lastIndexOf('/') + 1); }

    private Cursor query(Uri uri) { return resolver().query(uri, COLUMNS, null, null, null); }

    /* the document at rel, null when nothing is there. The external storage provider's IDs are "<volume>:<path>";
     * any other provider is walked by name. */
    private Uri find(Uri tree, String rel) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (!rel.isEmpty() && EXTERNAL.equals(tree.getAuthority())) {
            id = id + (id.endsWith(":") ? "" : "/") + rel;
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
            try (Cursor c = query(doc)) {
                return c != null && c.moveToFirst() ? doc : null;
            }
        }
        if (!rel.isEmpty())
            for (String part : rel.split("/")) {
                if (part.isEmpty()) continue;
                String next = null;
                try (Cursor c = query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id))) {
                    while (c != null && next == null && c.moveToNext())
                        if (part.equals(c.getString(1))) next = c.getString(0);
                }
                if (next == null) return null;
                id = next;
            }
        return DocumentsContract.buildDocumentUriUsingTree(tree, id);
    }

    private Uri need(Uri tree, String rel) throws Errno {
        Uri doc = find(tree, rel);
        if (doc == null) throw new Errno(OsConstants.ENOENT);
        return doc;
    }

    private boolean isDir(Uri doc) {
        try (Cursor c = query(doc)) {
            return c != null && c.moveToFirst() && Document.MIME_TYPE_DIR.equals(c.getString(2));
        }
    }

    private String nameAt(Uri doc) {
        try (Cursor c = query(doc)) {
            return c != null && c.moveToFirst() ? c.getString(1) : null;
        }
    }

    private static String line(Cursor c) {
        boolean dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
        long size = c.isNull(3) ? 0 : c.getLong(3), mtime = c.isNull(4) ? 0 : c.getLong(4) / 1000;
        return (dir ? "d" : "f") + "\t" + size + "\t" + mtime + "\t" + c.getString(1);
    }

    /* "d|f\t<size>\t<mtime s>\t<name>" */
    String stat(String t, String path) {
        try {
            Uri doc = need(Uri.parse(t), strip(path));
            try (Cursor c = query(doc)) {
                if (c == null || !c.moveToFirst()) return "-" + OsConstants.ENOENT;
                return line(c);
            }
        } catch (Exception e) {
            return fail(e);
        }
    }

    /* stat()'s lines, one per child; names with a line break are left out */
    String list(String t, String path) {
        try {
            Uri tree = Uri.parse(t), doc = need(tree, strip(path));
            StringBuilder out = new StringBuilder();
            Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(doc));
            try (Cursor c = query(kids)) {
                if (c == null) return "-" + OsConstants.EIO;
                while (c.moveToNext()) {
                    String name = c.getString(1);
                    if (name != null && name.indexOf('\n') < 0) out.append(line(c)).append('\n');
                }
            }
            return out.toString();
        } catch (Exception e) {
            return fail(e);
        }
    }

    /* a new document named by the end of rel; providers rename clashes ("a (1)") and unfit names, which is a
     * failure here */
    private Uri create(Uri tree, String rel, String mime) throws Exception {
        String name = nameOf(rel);
        if (find(tree, rel) != null) throw new Errno(OsConstants.EEXIST);
        Uri made = DocumentsContract.createDocument(resolver(), need(tree, parentOf(rel)), mime, name);
        if (made == null) throw new Errno(OsConstants.EIO);
        if (!name.equals(nameAt(made))) {
            DocumentsContract.deleteDocument(resolver(), made);
            throw new Errno(OsConstants.EINVAL);
        }
        return made;
    }

    /* mode "r", "w" (truncates) or "x" (creates, -EEXIST when taken); a file descriptor the caller owns */
    String open(String t, String path, String mode) {
        try {
            Uri tree = Uri.parse(t);
            String rel = strip(path);
            Uri doc = find(tree, rel);
            if (doc != null && mode.equals("x")) return "-" + OsConstants.EEXIST;
            if (doc != null && isDir(doc)) return "-" + OsConstants.EISDIR;
            if (doc == null && mode.equals("r")) return "-" + OsConstants.ENOENT;
            /* octet-stream keeps the name as given; another type may get its extension appended */
            if (doc == null) doc = create(tree, rel, "application/octet-stream");
            ParcelFileDescriptor fd = resolver().openFileDescriptor(doc, mode.equals("r") ? "r" : "wt");
            if (fd == null) return "-" + OsConstants.EIO;
            return Integer.toString(fd.detachFd());
        } catch (Exception e) {
            return fail(e);
        }
    }

    String mkdir(String t, String path) {
        try {
            create(Uri.parse(t), strip(path), Document.MIME_TYPE_DIR);
            return "0";
        } catch (Exception e) {
            return fail(e);
        }
    }

    /* renameDocument() answers null when the document ID stayed the same */
    private Uri renamed(Uri doc, String name) throws FileNotFoundException {
        Uri r = DocumentsContract.renameDocument(resolver(), doc, name);
        return r != null ? r : doc;
    }

    /* within one tree; another folder needs the provider's move, else -EXDEV and the caller copies */
    String rename(String t, String from, String to) {
        try {
            Uri tree = Uri.parse(t);
            String a = strip(from), b = strip(to), name = nameOf(b);
            if (a.isEmpty() || b.isEmpty()) return "-" + OsConstants.EINVAL;
            Uri doc = need(tree, a);
            boolean sameFolder = parentOf(a).equals(parentOf(b));
            boolean caseOnly = sameFolder && nameOf(a).equalsIgnoreCase(name) && !nameOf(a).equals(name);
            if (!caseOnly && find(tree, b) != null) return "-" + OsConstants.EEXIST;
            if (!sameFolder) {
                Uri dst = need(tree, parentOf(b));
                try (Cursor c = query(doc)) {
                    if (c == null || !c.moveToFirst() || (c.getInt(5) & Document.FLAG_SUPPORTS_MOVE) == 0)
                        return "-" + OsConstants.EXDEV;
                }
                doc = DocumentsContract.moveDocument(resolver(), doc, need(tree, parentOf(a)), dst);
                if (doc == null) return "-" + OsConstants.EIO;
            }
            if (caseOnly) doc = renamed(doc, name + ".ferret-renaming");
            if (!name.equals(nameAt(doc))) doc = renamed(doc, name);
            return name.equals(nameAt(doc)) ? "0" : "-" + OsConstants.EINVAL;
        } catch (Exception e) {
            return fail(e);
        }
    }

    /* a file or an empty folder: deleteDocument would take a folder's contents with it */
    String remove(String t, String path) {
        try {
            Uri tree = Uri.parse(t), doc = need(tree, strip(path));
            if (isDir(doc)) {
                Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(doc));
                try (Cursor c = query(kids)) {
                    if (c != null && c.getCount() > 0) return "-" + OsConstants.ENOTEMPTY;
                }
            }
            return DocumentsContract.deleteDocument(resolver(), doc) ? "0" : "-" + OsConstants.EIO;
        } catch (Exception e) {
            return fail(e);
        }
    }
}
