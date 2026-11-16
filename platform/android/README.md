# ferret for Android (arm64-v8a and x86_64, Android 11+)

## Requirements

- Android SDK at `~/Android/Sdk` (`ANDROID_HOME` overrides): build-tools 36.1.0, platform android-36.1,
  NDK 28.2.13676358 (`ANDROID_NDK` overrides).
- JDK (`javac`, `keytool`), CMake, Ninja, curl, git, zip, perl (OpenSSL's `Configure`).

## Build

```sh
platform/android/build.sh          # -> ~/.cache/ferret/android/out/ferret.apk
platform/android/build.sh check    # only compiles libferret.so for both ABIs
```

Everything goes under `~/.cache/ferret/android` (`FR_ANDROID_OUT` overrides), including the debug signing key;
nothing is written into the repo. `deps.sh` fetches raylib and nanosvg into `thirdparty/` first, and builds
libarchive with zstd, xz and bzip2 (zlib is the NDK's), and OpenSSL's libcrypto, libsmb2 and libssh2, static for
each ABI (once; the folders are named after their versions). No Gradle:
`javac`, `d8`, `aapt2`, `zipalign` and `apksigner`. versionName is the build's `FR_VERSION` (root
`CMakeLists.txt`), versionCode the numeric version (1.2.3 -> 10203). targetSdk is 34 because 35+ forces
edge-to-edge drawing.

## Install

```sh
adb install -r ~/.cache/ferret/android/out/ferret.apk
```

On first start ferret asks for "All files access" (`MANAGE_EXTERNAL_STORAGE`) and opens the system page for
it. For testing it can be granted directly:

```sh
adb shell appops set --uid io.github.vamsikalagaturu.ferret MANAGE_EXTERNAL_STORAGE allow
```

## Files

| File | What |
| --- | --- |
| `android_main.c` | `platform.h` for Android and `main()`: environment (`HOME`, `XDG_*` in the app's files folder), touch to clicks, long press and wheel scrolling, keys and the soft keyboard, Back, insets, places. The `java()` JNI helper (`android.h`). |
| `saf.c` | The `saf://` VFS backend: SD cards and USB drives through the Storage Access Framework. |
| `java/.../FerretActivity.java` | `NativeActivity` subclass: storage permission, opening files, soft keyboard text, clipboard, haptics, insets, remembered server passwords (`servers.sealed` in the app's files, AES-GCM under an Android Keystore key). |
| `java/.../Saf.java` | Removable volumes, their granted folder trees and the `DocumentsContract` calls behind `saf.c`. |
| `java/.../OpenProvider.java` | Serves a file read-only to the app it is opened in (`content://<package>.files/<path>`). |
| `res/mipmap-*/ic_launcher*` | Lucide `folder-search` in #88C0D0: an adaptive icon (`mipmap-anydpi-v26`, Android 8+) over #2E3440 (`values/colors.xml`), the square `ic_launcher.png` for older launchers and the Linux desktop entry. |

## SD cards and USB drives

All files access reads removable volumes at `/storage/XXXX-XXXX`, but writes there fail. The drawer lists each
mounted removable volume (`StorageManager.getStorageVolumes()`, named by its description, "SD card") as
`saf://<tree URI, percent-encoded>/`. Opening one without a grant opens the system's folder picker at that volume;
the chosen tree's permission is persisted and remembered per volume UUID (shared preferences `saf`). Listing,
`stat` and reading use the `/storage` path when it is readable; creating, writing, renaming, moving and deleting
go through `DocumentsContract` (`openFileDescriptor()` hands `saf.c` a detached fd). Moves to another folder use
`moveDocument()` where the provider supports it, else `-EXDEV` makes the job copy. No trash, permissions or
modification times there.

JNI from the jobs thread: `java()` works on any thread. A thread other than `android_main` that it attaches stays
attached until it exits, when a `pthread` key destructor detaches it (ART aborts a process whose attached thread
exits).

Testing without a card: `adb shell sm set-virtual-disk true` adds a virtual removable disk (format it as portable
storage when Android asks).

Touch: a tap clicks on release, a moving finger scrolls (60 dp = one wheel notch) and flings, a finger held 0.4 s
right-clicks.
