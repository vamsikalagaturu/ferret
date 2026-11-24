# ferret

A keyboard-first file explorer for Linux and Android, in C with [raylib](https://www.raylib.com). Nord theme, vim-style
keys (`?` lists them all), and the same app on the desktop and the phone.

## Features

- Browse local storage, archives (zip, tar, 7z, rar, iso), the trash, and phones over MTP
- Network storage: SMB, SFTP, FTP/FTPS, WebDAV, NFS
- Fuzzy search over every file name (fzf's matcher), plus search inside files
- Tabs, split view with folder compare and sync, bookmarks, recent folders and files
- Copy, move, rename (also in bulk), undo, links, permissions, duplicate finder, folder sizes
- Disk manager on Linux (UDisks2): partitions, formats, bootable USB sticks
- Updates itself from GitHub releases

## Install

**Android** (sideload, Android 11+): download `ferret.apk` from the
[latest release](https://github.com/vamsikalagaturu/ferret/releases/latest), install it and allow access to all files.

**Linux** (x86_64): download `ferret-linux-x86_64` from the latest release to `~/.local/bin/ferret` and make it
executable, or build it (below) and run `platform/linux/install.sh`, which also adds a desktop entry. Either way,
ferret updates that copy itself.

## Build

```sh
platform/linux/build.sh            # release build + unit tests -> ~/.cache/ferret/linux/build/ferret
platform/linux/build.sh debug      # AddressSanitizer build
platform/android/build.sh          # APK -> ~/.cache/ferret/android/out/ferret.apk
```

The scripts fetch pinned third-party sources (`platform/versions.sh`) into `thirdparty/`. Linux needs CMake, Ninja,
pkg-config and the development files listed in `.github/workflows/ci.yml`; Android needs the SDK with NDK
28.2.13676358, build-tools 36.1.0 and platform android-36.1.

## Releasing

Changes reach `main` only through pull requests that pass CI. A PR that raises the version in `CMakeLists.txt`'s
`project()` is released when it merges: `.github/workflows/release.yml` builds both apps and publishes `vX.Y.Z`.

## License

MIT, see [LICENSE](LICENSE). Third-party code keeps its own licences (fetched into `thirdparty/`).
