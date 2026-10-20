# Releasing Darkride

This page covers how a release is made: the tag, the three downloads, the checks each download must pass, and the
notes. Today these steps are done by hand. They're written down so that a GitHub Actions workflow can do the same.
The first release made this way was [v1.0.1](https://github.com/caitlynmeeks/darkride-interface/releases/tag/v1.0.1).

## Tags

- A release is a tag `vMAJOR.MINOR.PATCH` on `main`. Only Caity can create, move or delete these tags. A ruleset
  enforces that.
- Every download is built from exactly the tagged commit, in a fresh build folder.
- A release that changes the network protocol goes out together with the live servers' update, and its notes say
  so.

## What a release contains

| File | For |
|---|---|
| `Darkride-<version>-mac.zip` | Macs with Apple silicon, on macOS 12 or later |
| `Darkride-<version>-windows.zip` | 64-bit Windows 10 and 11. Unzips to a `Darkride` folder with `Darkride.exe` |
| `darkride-server-linux-x64-<version>.tar.gz` | The world servers, for hosting your own world. Built on Ubuntu 24.04 |

The notes have these sections:
- Downloads, with each file's SHA-256.
- How to open the unsigned downloads the first time.
- What's new.
- Compatibility, meaning whether the protocol changed.
- How it was tested.
- Known issues.

## Release build settings

Configure a fresh build folder with these three in the environment:

```
RELEASE_TYPE=PRODUCTION
RELEASE_NUMBER=1.0.1
STABLE_BUILD=1
```

These do the following:
- The version reads 1.0.1, both in Help > About and in the log's `[VERSION]` lines.
- A release keeps its settings apart from development builds.
- Qt's deploy tool runs after the build. On macOS, development builds skip it.

**The JavaScript guest module** (`darkride-js.wasm`) is the same file on every platform, and every download includes
it. The build copies it from the first place it exists:
1. `$DARKRIDE_JS_WASM`
2. `tools/wasm-js/build/darkride-js.wasm`
3. `../downloads/darkride-js-0.17.0.wasm` beside the checkout

If it's in none of them, the build warns and scripts run on QtScript only. See `cmake/macros/CopyJavaScriptGuest.cmake`.

## macOS

You need:
- An Apple silicon Mac.
- CMake 3.x. CMake 4 refuses the older CMake scripts.
- Python 3.11. `prebuild.py` fails on 3.14.
- An arm64 Qt 5.15 with QtWebEngine. Qt's own 5.15 downloads are Intel-only on macOS. We use Overte's Qt 5.15.18
  build, which is made of plain libraries rather than frameworks.

### 1. Build

```
PY=/path/to/python3.11
RELEASE_TYPE=PRODUCTION RELEASE_NUMBER=1.0.1 STABLE_BUILD=1 \
  cmake -G Ninja -S . -B build-release -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DHIFI_PYTHON_EXEC=$PY -DPython3_EXECUTABLE=$PY
ninja -C build-release interface
```

macdeployqt runs after the build and copies Qt and Darkride's own libraries into `Darkride.app/Contents/Frameworks`.

### 2. Work on a copy

```
ditto build-release/interface/Darkride.app stage/Darkride.app
A=stage/Darkride.app/Contents
QT=/path/to/qt-5.15
```

### 3. Add QtWebEngine's helper

macdeployqt leaves out the helper program that web pages run in when Qt is made of plain libraries:

```
cp "$QT/libexec/QtWebEngineProcess" "$A/MacOS/"
mkdir -p "$A/Resources/qtwebengine/resources" "$A/Resources/qtwebengine/translations"
cp "$QT"/resources/* "$A/Resources/qtwebengine/resources/"
cp -R "$QT/translations/qtwebengine_locales" "$A/Resources/qtwebengine/translations/"
```

Then replace `Contents/Resources/qt.conf` with:

```
[Paths]
Plugins = PlugIns
Imports = Resources/qml
Qml2Imports = Resources/qml
LibraryExecutables = MacOS
Data = Resources/qtwebengine
Translations = Resources/qtwebengine/translations
```

### 4. Remove the build machine's paths

Every library still carries search paths (LC_RPATH) into the build folder and the Qt folder. They name your home
folder, and on the build machine they can hide a library that's missing from the app. Delete every search path
that doesn't start with `@`, and give each file that loads `@rpath/…` libraries the app's own Frameworks folder:

```
find "$A" -type f | while IFS= read -r f; do
  file "$f" | grep -q Mach-O || continue
  for r in $(otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}' | grep -v '^@'); do
    install_name_tool -delete_rpath "$r" "$f" 2>/dev/null
  done
  if otool -L "$f" | grep -q '@rpath/' && ! otool -l "$f" | grep -q '@executable_path/../Frameworks'; then
    install_name_tool -add_rpath @executable_path/../Frameworks "$f" 2>/dev/null
  fi
done
```

### 5. Sign it

There's no Apple Developer ID yet, so the app gets an ad-hoc signature. Sign every library and helper first, then
the app:

```
find "$A" -type f | while IFS= read -r f; do
  if [ "$f" = "$A/MacOS/Darkride" ]; then continue; fi
  if file "$f" | grep -q Mach-O; then codesign --force -s - "$f" 2>/dev/null; fi
done
codesign --force -s - stage/Darkride.app
codesign --verify --deep --strict stage/Darkride.app
```

### 6. Zip it

```
ditto -c -k --sequesterRsrc --keepParent stage/Darkride.app Darkride-1.0.1-mac.zip
```

### Checks

- **`python3 tools/packaging/check-mac-bundle.py stage/Darkride.app`** must report 0 search paths outside the app
  and 0 unresolved dependencies. For v1.0.1 it checked 131 Mach-O files.
- **Start it.**
  - Run `stage/Darkride.app/Contents/MacOS/Darkride --allowMultipleInstances` with `USERNAME` set to something unique.
    Every build shares a local socket named after `$USERNAME`, so without that, a second copy hands off to the
    Darkride you already have open.
  - It should reach the sign-in screen.
  - `lsof -p <pid>` should show no library from outside the app.
  - `QtWebEngineProcess` should run from inside the app.
  - Close it, then delete the settings it made: `~/.config/Tivoli Cloud VR` and
    `~/Library/Application Support/Tivoli Cloud VR`.
- **macOS refuses the unsigned app at first open** ("could not verify"). People get past that with
  System Settings > Privacy & Security > Open Anyway. A Developer ID with notarization would remove that step.

## Windows

You need Visual Studio 2026 or 2022, or its Build Tools, with the v143 toolset, and Qt 5.15.2, as in
[BUILD_WINDOWS.md](BUILD_WINDOWS.md). The commands below use the `vs2022` preset; with Visual Studio 2026, use
`cmake --preset vs2026` instead (the same v143 compiler).

### 1. Build

Use a fresh worktree at the tag:

```
set RELEASE_TYPE=PRODUCTION
set RELEASE_NUMBER=1.0.1
set STABLE_BUILD=1
cmake --preset vs2022
cmake --build build --config RelWithDebInfo --target interface
```

- A release configure needs the Windows SDK's `signtool.exe` to be present. It only signs when `HF_PFX_FILE` is
  set, so leave that unset.
- Smart App Control on the build machine can block `vcpkg.exe` during prebuild (WinError 4551).

### 2. Stage it

- Copy `build\interface\RelWithDebInfo\` to a folder named `Darkride`.
- Delete the debug files from it: `*.pdb`, `*.ilk`, `*.lib` and `*.exp`.
- Make sure `resources\wasm\darkride-js.wasm` is in it.

### 3. Add Microsoft's runtime

windeployqt runs with `--no-compiler-runtime`, so copy these DLLs in beside `Darkride.exe`. Microsoft allows shipping
them with the app.
- From `VC\Redist\MSVC\<version>\x64\Microsoft.VC143.CRT\`: `concrt140`, `msvcp140`, `msvcp140_1`, `msvcp140_2`,
  `msvcp140_atomic_wait`, `msvcp140_codecvt_ids`, `vcruntime140`, `vcruntime140_1` and `vcruntime140_threads`.
- From `Microsoft.VC143.OpenMP\`: `vcomp140.dll`, which `aristo_interface.dll` needs.

### 4. Zip it

Zip it with `Darkride\` as the only top folder, as `Darkride-1.0.1-windows.zip`.

### Checks

- **Run `dumpbin /dependents` on every .exe and .dll,** including those in `plugins\` and `QtWebEngineProcess.exe`.
  - Every dependency must be in the folder or be a Windows system DLL.
  - OpenSSL 1.1 (`libssl-1_1-x64`, `libcrypto-1_1-x64`) must be in the folder.
  - A build machine with Microsoft's runtime installed hides a missing runtime DLL. That's how `vcomp140.dll` was
    caught.
- **Start it from the staged folder** with `HIFI_ALLOW_MULTIPLE_INSTANCES=1`.
  - It should reach the sign-in screen.
  - The log should have no missing-DLL errors.
  - `QtWebEngineProcess.exe` should run from the folder.
- **Windows SmartScreen warns about the unsigned app.** People get past it with More info > Run anyway. Smart App
  Control blocks unsigned apps outright, so signing is what fixes that.

## Linux world servers

These are built on Ubuntu 24.04 x86_64, and the result needs a Linux at least that new. Today's servers are
development builds, with no release settings.

- **Packages:** the build tools, Ubuntu's Qt 5.15 development packages and the usual X11, GL and audio development
  headers.
- **Qt:** Ubuntu's Qt 5.15, behind a small prefix laid out the way `cmake/macros/SetupQt.cmake` expects. The prefix
  holds `include`, `lib/cmake`, `lib/libQt5*.so`, `bin` and `plugins`, all as links into the Ubuntu packages. Point
  `QT_CMAKE_PREFIX_PATH` at its `lib/cmake`.
- **GCC 13** needs `CXXFLAGS="-include cstdint -Wno-changes-meaning"` for some older dependencies.

```
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DSERVER_ONLY=1 -DVCPKG_BUILD_TYPE=release -DBUILD_TOOLS_INCLUDE=oven
ninja domain-server assignment-client oven
python3 ../tools/make_appdir.py server .
tar czf darkride-server-linux-x64-1.0.1.tar.gz server.AppDir
```

Some dependency archives, such as the Tea protocol plugin's, can no longer be downloaded from where they used to
live. Our builds feed saved copies through `VCPKG_DOWNLOADS`, so an automated build needs those copies hosted
somewhere it can fetch them.

To run a world from the tarball:
- Start `./server.AppDir/domain-server`.
- Start `./server.AppDir/assignment-client -n 7`.
- The world's settings page is at http://localhost:40100.

## Publishing

1. Caity tags `main` (`git tag -a v1.0.1`) and pushes the tag.
2. Build the three downloads from the tag, and run every check above.
3. Write the notes, with each file's SHA-256 (`shasum -a 256` or `Get-FileHash`).
4. Publish with `gh release create v1.0.1 --verify-tag --latest --title "…" --notes-file notes.md <files>`.
   A download can follow later with `gh release upload v1.0.1 <file>`.

Caity publishes releases, or a release is published after she says so, even when a workflow built the files.

## Automating it

GitHub's runners are free for public repositories. These are the hard parts:
- **Qt for arm64 macOS.** Qt 5.15 with WebEngine isn't among Qt's arm64 downloads, so the Mac job needs a prebuilt
  Qt it can fetch.
- **Dependency caches.** vcpkg's dependencies, plus the archives whose original downloads are gone.
- **The JavaScript guest module.** It can be built with `tools/wasm-js/build.sh`, or fetched from a published build.

A good first step is a workflow that builds and runs the checks on every pull request. Building the downloads when a
tag is pushed, as a draft release for Caity to publish, can follow.

No secrets are needed until the downloads are signed, and the signing keys stay with Caity.
