package io.github.vamsikalagaturu.ferret;

import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.text.InputType;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.security.KeyStore;
import java.util.Properties;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/* The native window plus what only Java can reach: the all-files permission, opening files in other apps, the soft
 * keyboard's text, the clipboard, haptics, the system bar insets and the Android Keystore (server passwords). Called from the native thread
 * (android_main.c); every public method takes and returns strings, null meaning "no" or "nothing". */
public class FerretActivity extends NativeActivity {
    /* NativeActivity drops intents delivered to a running instance (launchMode singleTask) */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    /* "left top right bottom" in pixels: the visible system bars, the display cutout and the soft keyboard (the
     * window does not resize for it: adjustNothing) */
    public String systemInsets() {
        WindowInsets w = getWindow().getDecorView().getRootWindowInsets();
        if (w == null) return null;
        Insets i = w.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
        return i.left + " " + i.top + " " + i.right + " " + i.bottom;
    }

    /* the clipboard wants a Looper thread on older releases */
    private String onUi(Callable<String> job) {
        FutureTask<String> task = new FutureTask<>(job);
        runOnUiThread(task);
        try {
            return task.get(5, TimeUnit.SECONDS);
        } catch (Exception e) {
            return null;
        }
    }

    /* ---- storage ---- */

    public String storageGranted() {
        return Environment.isExternalStorageManager() ? "1" : null;
    }

    public String requestStorage() {
        return onUi(() -> {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
            return "1";
        });
    }

    /* ---- SD cards and USB drives (Saf.java, for platform/android/saf.c) ---- */

    private volatile Saf saf;

    public String safVolumes() { return saf.volumes(); }
    public String safGranted(String tree) { return saf.isGranted(tree); }
    public String safRequest(String tree) { return saf.request(tree); }
    public String safStat(String tree, String path) { return saf.stat(tree, path); }
    public String safList(String tree, String path) { return saf.list(tree, path); }
    public String safOpen(String tree, String path, String mode) { return saf.open(tree, path, mode); }
    public String safMkdir(String tree, String path) { return saf.mkdir(tree, path); }
    public String safRename(String tree, String from, String to) { return saf.rename(tree, from, to); }
    public String safRemove(String tree, String path) { return saf.remove(tree, path); }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == Saf.REQUEST_TREE) saf.onResult(result, data);
    }

    /* ---- opening files ---- */

    /* path in the app the system picks for its type, through OpenProvider; null when no app takes it */
    public String open(String path) {
        Uri uri = OpenProvider.uriFor(getPackageName(), path);
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(uri, OpenProvider.mimeOf(path));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return onUi(() -> {
            try {
                startActivity(i);
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    /* path in an app picked from the system's chooser, every time; null when no app takes it */
    public String openWith(String path) {
        Uri uri = OpenProvider.uriFor(getPackageName(), path);
        Intent view = new Intent(Intent.ACTION_VIEW);
        view.setDataAndType(uri, OpenProvider.mimeOf(path));
        view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return onUi(() -> {
            try {
                startActivity(Intent.createChooser(view, null));
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    /* paths separated by '\n' to the system's share sheet; null when nothing can take them */
    public String share(String paths) {
        String[] list = paths.split("\n");
        java.util.ArrayList<Uri> uris = new java.util.ArrayList<>();
        String type = null;
        for (String p : list) {
            uris.add(OpenProvider.uriFor(getPackageName(), p));
            String t = OpenProvider.mimeOf(p);
            type = type == null || type.equals(t) ? t : "*/*";
        }
        Intent send = new Intent(uris.size() == 1 ? Intent.ACTION_SEND : Intent.ACTION_SEND_MULTIPLE);
        send.setType(type);
        if (uris.size() == 1) send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        else send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        /* the grant reaches the receiving app through the clip data */
        ClipData clip = ClipData.newRawUri(null, uris.get(0));
        for (int k = 1; k < uris.size(); k++) clip.addItem(new ClipData.Item(uris.get(k)));
        send.setClipData(clip);
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return onUi(() -> {
            try {
                startActivity(Intent.createChooser(send, null));
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    /* ---- server passwords ---- */

    /* One file in the app's private storage holds the remembered servers (server key -> "user\npassword"), sealed
     * with AES-GCM under a key that lives in the Android Keystore and never leaves it. */
    private static final String SECRET_KEY = "ferret-servers";

    private File secretsFile() { return new File(getFilesDir(), "servers.sealed"); }

    private SecretKey secretKey() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(SECRET_KEY)) return (SecretKey) ks.getKey(SECRET_KEY, null);
        KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        g.init(new KeyGenParameterSpec.Builder(SECRET_KEY, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build());
        return g.generateKey();
    }

    /* the file is the 12-byte IV, then the ciphertext of the Properties text */
    private Properties readSecrets() throws Exception {
        Properties p = new Properties();
        File f = secretsFile();
        if (!f.exists()) return p;
        byte[] all = java.nio.file.Files.readAllBytes(f.toPath());
        if (all.length < 12) return p;
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE, secretKey(), new GCMParameterSpec(128, all, 0, 12));
        p.load(new ByteArrayInputStream(c.doFinal(all, 12, all.length - 12)));
        return p;
    }

    private void writeSecrets(Properties p) throws Exception {
        ByteArrayOutputStream text = new ByteArrayOutputStream();
        p.store(text, null);
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.ENCRYPT_MODE, secretKey());
        byte[] iv = c.getIV(), sealed = c.doFinal(text.toByteArray());
        File tmp = new File(getFilesDir(), "servers.sealed.tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(iv);
            out.write(sealed);
            out.getFD().sync();
        }
        if (!tmp.renameTo(secretsFile())) throw new java.io.IOException("rename");
    }

    public synchronized String secretGet(String key) {
        try {
            return readSecrets().getProperty(key);
        } catch (Exception e) {
            return null;
        }
    }

    public synchronized String secretPut(String key, String value) {
        try {
            Properties p = readSecrets();
            p.setProperty(key, value);
            writeSecrets(p);
            return "1";
        } catch (Exception e) {
            return null;
        }
    }

    public synchronized String secretDelete(String key) {
        try {
            Properties p = readSecrets();
            if (p.remove(key) != null) writeSecrets(p);
            return "1";
        } catch (Exception e) {
            return null;
        }
    }

    /* ---- the system's font size (Settings > Display > Font size) ---- */

    public String fontScale() {
        return Float.toString(getResources().getConfiguration().fontScale);
    }

    /* ---- haptics ---- */

    public String haptic() {
        runOnUiThread(() -> getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.LONG_PRESS));
        return null;
    }

    /* ---- soft keyboard ---- */

    /* code points, and these, for android_main.c's native_text() */
    private static final int IME_BACKSPACE = -1, IME_ENTER = -2, IME_DELETE = -3;
    private static native void nativeText(int[] codes);

    private TextSink sink;

    @Override
    protected void onCreate(Bundle state) {
        /* before super.onCreate(), which starts the native thread that calls it */
        saf = new Saf(this);
        super.onCreate(state);
        /* the window reaches into the camera cutout so the strip shows the app's background, not black; the
         * native side keeps clear of it through systemInsets() */
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        sink = new TextSink();
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
    }

    public String showKeyboard() {
        runOnUiThread(() -> {
            sink.requestFocus();
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.restartInput(sink);
            imm.showSoftInput(sink, 0);
        });
        return null;
    }

    public String hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
        });
        return null;
    }

    /* NativeActivity's own view takes key events only, so composed text (CJK, swipe, autocorrect) would never
     * arrive. What the keyboard is composing is typed as it goes and replaced (backspaces, then the new text) when
     * it changes or is committed. */
    private final class TextSink extends View {
        TextSink() {
            super(FerretActivity.this);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT;
            info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
            return new SinkConnection(this);
        }
    }

    private static final class SinkConnection extends BaseInputConnection {
        private String composing = "";

        SinkConnection(View view) { super(view, false); }

        private static void send(int backspaces, CharSequence text) {
            String s = text == null ? "" : text.toString();
            int[] codes = new int[backspaces + s.codePointCount(0, s.length())];
            int k = 0;
            for (int i = 0; i < backspaces; i++) codes[k++] = IME_BACKSPACE;
            for (int i = 0; i < s.length(); i += Character.charCount(s.codePointAt(i))) {
                int c = s.codePointAt(i);
                codes[k++] = c == '\n' ? IME_ENTER : c;
            }
            if (codes.length > 0) nativeText(codes);
        }

        private void replaceComposing(CharSequence text) {
            send(composing.codePointCount(0, composing.length()), text);
        }

        @Override
        public boolean setComposingText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = text == null ? "" : text.toString();
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = "";
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = "";
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            int[] codes = new int[before + after];
            for (int i = 0; i < before; i++) codes[i] = IME_BACKSPACE;
            for (int i = 0; i < after; i++) codes[before + i] = IME_DELETE;
            if (codes.length > 0) nativeText(codes);
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            nativeText(new int[] {IME_ENTER});
            return true;
        }
    }

    /* ---- clipboard ---- */

    public String setClipboard(String text) {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("ferret", text));
            return "1";
        });
    }

    public String clipboard() {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            ClipData clip = cm.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return null;
            CharSequence s = clip.getItemAt(0).coerceToText(this);
            return s == null ? null : s.toString();
        });
    }
}
