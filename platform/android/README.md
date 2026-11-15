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
nothing is written into the repo. `deps.sh` fetches raylib, nanosvg, libsmb2, libssh2 and OpenSSL into
`thirdparty/` first and builds the last three static per ABI (once; the folder is named after their versions). No Gradle:
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
| `android_main.c` | `platform.h` for Android and `main()`: environment (`HOME`, `XDG_*` in the app's files folder), touch to clicks, long press and wheel scrolling, keys and the soft keyboard, Back, insets, places under `/storage`. |
| `java/.../FerretActivity.java` | `NativeActivity` subclass: storage permission, opening files, soft keyboard text, clipboard, haptics, insets, remembered server passwords (`servers.sealed` in the app's files, AES-GCM under an Android Keystore key). |
| `java/.../OpenProvider.java` | Serves a file read-only to the app it is opened in (`content://<package>.files/<path>`). |
| `res/mipmap-*/ic_launcher.png` | Lucide `folder-search` in #88C0D0 on #2E3440. |

Touch: a tap clicks on release, a moving finger scrolls (60 dp = one wheel notch) and flings, a finger held 0.4 s
right-clicks.
