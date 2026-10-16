# Building Darkride for Windows

This guide builds the Darkride client (`interface.exe`) and, if you want them, the servers (`domain-server.exe`, `assignment-client.exe`) on Windows 10 or 11 (64-bit).

**Plan for:**

-   **About 1 hour.** The first build compiles every third-party library, which takes 15–30 minutes, then compiles Darkride itself in 30–40 minutes. Later builds take a few minutes.
-   **About 30 GB of disk space.**
-   **16 GB of RAM or more.** Step 5 says how to build with less.

## 1. Install the tools

Darkride builds with Microsoft's C++ compiler. Pick how you'd like to get it:

-   **A. Visual Studio 2026 Community**, Microsoft's free IDE, with the compiler.
-   **B. Build Tools for Visual Studio 2026 with VS Code**: just the compiler, and you edit and build in [VS Code](#building-in-vs-code).

Either way, install the **Desktop development with C++** workload and leave its default components ticked. That's everything the build needs from Visual Studio: no individual components to add.

The quickest way is to paste these into a terminal (PowerShell or Command Prompt). Windows asks for permission for each installer.

**A. Visual Studio 2026 Community:**

```cmd
winget install -e --id Microsoft.VisualStudio.Community --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --norestart"
```

**B. Build Tools and VS Code:**

```cmd
winget install -e --id Microsoft.VisualStudio.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart"
winget install -e --id Microsoft.VisualStudioCode
```

**Then, for both:**

```cmd
winget install -e --id Microsoft.PowerShell
winget install -e --id Python.Python.3.13
winget install -e --id OpenJS.NodeJS.LTS
winget install -e --id Git.Git
```

| Tool | Why |
|---|---|
| Visual Studio 2026 or its Build Tools | The C++ compiler, the Windows SDK and CMake. [BUILD_WINDOWS_VISUAL_STUDIO.md](BUILD_WINDOWS_VISUAL_STUDIO.md) has point-and-click instructions, and covers Visual Studio 2022 |
| PowerShell 7 | Some build steps run `pwsh`. Not the Microsoft Store version: the build looks in `C:\Program Files\PowerShell\7` |
| Python 3 | Runs the build's setup scripts. 3.13 is tested |
| Node.js (LTS) | Builds the in-world web UI |
| Git | Gets the code, and some dependencies |

Close the terminal and open a new one, so it sees the new programs, and check that each one answers:

```cmd
pwsh --version
python --version
node --version
git --version
```

## 2. Install Qt 5.15.2

Darkride needs Qt 5.15.2 for MSVC, with **Qt WebEngine** and **Qt Script**. [aqtinstall](https://github.com/miurahr/aqtinstall) downloads the official build without a Qt account (about 2 GB):

```cmd
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 -m qtwebengine qtscript qtwebglplugin -O C:\Qt
```

This installs Qt into `C:\Qt\5.15.2\msvc2019_64`, where the build finds it.

To use a Qt somewhere else, set `QT_CMAKE_PREFIX_PATH` to its `lib\cmake` folder before step 4, for example `set QT_CMAKE_PREFIX_PATH=D:/Qt/5.15.2/msvc2019_64/lib/cmake`. With neither, the build downloads a prebuilt Qt instead (1.65 GB).

The official Qt's web browser component can't play H.264 or AAC video (MP4 files and most live streams) inside web entities; WebM, VP9 and Opus work. To add those codecs, see [Optional: Qt with H.264 and AAC](#optional-qt-with-h264-and-aac) at the end.

## 3. Get the code

```cmd
git clone https://github.com/caitlynmeeks/darkride-interface.git
cd darkride-interface
```

Keep it near the root of a drive, such as `C:\code\darkride-interface`. Windows limits paths to 260 characters unless long paths are enabled, and the build's folders go deep.

## 4. Configure

Open **Developer PowerShell for VS 2026** from the Start menu (it puts the compiler and CMake on the PATH), go to the `darkride-interface` folder, and run:

```cmd
cmake --preset vs2026
```

With Visual Studio 2022, use `cmake --preset vs2022`.

The preset is in `CMakePresets.json`. It configures a 64-bit build in the `build` folder, with Visual Studio 2022's v143 compiler (Qt 5.15's headers don't compile with Visual Studio 2026's own) and the setting CMake 4 needs for the older parts of the tree. Visual Studio and VS Code use the same presets.

The first run downloads and compiles all the dependencies, which takes 15–30 minutes. It's finished when CMake prints `-- Generating done`.

vcpkg, which builds the dependencies, keeps them in `C:\Users\<you>\tivoli\vcpkg`. To put them somewhere else, set `TIVOLI_VCPKG_BASE` before configuring, for example `set TIVOLI_VCPKG_BASE=C:\vcpkg`; keep that path short too. Any change to the files in `cmake/ports` makes vcpkg rebuild the dependencies, but it reuses unchanged packages from its cache in `%LOCALAPPDATA%\vcpkg\archives`.

## 5. Build

```cmd
cmake --build --preset vs2026
```

That builds the client. The `vs2026-servers` preset builds the servers too (`vs2022` and `vs2022-servers` for Visual Studio 2022).

**If the PC runs short of memory** (the build slows to a crawl, or tools quit with out-of-memory errors), build fewer projects at once by adding `-- /m:4` to the end of the command. By default Visual Studio builds as many projects at once as the PC has CPU threads, each compiling several files at a time, which can need more than 32 GB on a 24-thread CPU.

Or open `build\hifi.sln` in Visual Studio, set the configuration to **RelWithDebInfo**, right-click **interface** in the Solution Explorer and choose **Build**.

## 6. Run

```cmd
build\interface\RelWithDebInfo\interface.exe
```

**Darkride's metaverse.** The worlds listed in **Explore**, and accounts, come from `https://vr.darkride.ai`. The client's built-in default is still Tivoli's old server, which is gone, so set this once for your Windows user. Programs started afterwards pick it up.

```cmd
setx HIFI_METAVERSE_URL https://vr.darkride.ai
```

**A world on your own PC.** Start the servers in two command prompts, then go to `hifi://localhost` in interface:

```cmd
build\domain-server\RelWithDebInfo\domain-server.exe
build\assignment-client\RelWithDebInfo\assignment-client.exe --min 6 --max 10
```

The world's admin page is http://localhost:40100.

## Building in VS Code

VS Code builds Darkride with the same compiler and presets as steps 4 and 5, through Microsoft's CMake Tools extension. You need Visual Studio 2026 or its Build Tools from step 1 (VS Code has no compiler of its own), plus Qt from step 2 and the code from step 3.

1.  **Install two extensions:** **C/C++** and **CMake Tools**, both from Microsoft. From a terminal:

    ```cmd
    code --install-extension ms-vscode.cpptools
    code --install-extension ms-vscode.cmake-tools
    ```

2.  **Open the folder from a Developer PowerShell for VS 2026**, so VS Code can find CMake and the compiler:

    ```cmd
    cd C:\code\darkride-interface
    code .
    ```

    Starting VS Code from the Start menu works too, if CMake is on your PATH: `winget install -e --id Kitware.CMake` puts it there.

3.  **Pick the preset.** CMake Tools reads `CMakePresets.json` and asks for a configure preset: choose **Visual Studio 2026 (or its Build Tools)**. If it doesn't ask, press **Ctrl+Shift+P** and run **CMake: Select Configure Preset**. It then configures, which the first time takes as long as step 4; the **Output** panel shows progress, and it's done at `-- Generating done`.

4.  **Build:** press **F7** (or run **CMake: Build**). The first time, it asks for a build preset: **Darkride (interface)**, or **Darkride and the servers**.

5.  **Run or debug:** run **CMake: Set Launch/Debug Target** and choose **interface**, then **CMake: Run Without Debugging** (Shift+F5) or **CMake: Debug** (Ctrl+F5).

## What's left out on Windows

These SDKs can no longer be downloaded, so the build leaves out the plugins that need them:

-   **Oculus PC SDK.** Rift and Quest headsets work through SteamVR with the OpenVR plugin. If you have `LibOVR.lib` in the vcpkg tree, `oculus-plugin` is built again.
-   **Leap Motion 2.3 SDK.**
-   **Tea protocol plugin**, a prebuilt binary on every platform. It is used if its extracted archive is in `plugins/tea-protocol-plugin-prebuilt/tea-protocol-plugin-2021-12-12`.

## Troubleshooting

| Error | Fix |
|---|---|
| `PowerShell 7 (pwsh) is not on the PATH`, `PowerShell 7 is the Microsoft Store version`, or `Could not find PowerShell Core` | Install PowerShell 7 with `winget install -e --id Microsoft.PowerShell` (step 1), not the Microsoft Store version, then close every terminal and open a **new** one. `where.exe pwsh` should find `C:\Program Files\PowerShell\7\pwsh.exe`; a `pwsh.exe` in `AppData\Local\Microsoft\WindowsApps` is the Store version, which vcpkg can't use: uninstall it (**Settings → Apps → Installed apps → PowerShell**) and install the regular one with `winget install -e --id Microsoft.PowerShell --source winget`. Configure again; vcpkg keeps the libraries it already built |
| `Git (git) is not on the PATH` | Install Git with `winget install -e --id Git.Git` (step 1), then open a **new** terminal and configure again |
| `No suitable Visual Studio instances were found`, then `vcpkg configuration missing` | vcpkg found no compiler it can use. In the Visual Studio Installer, check that **MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)** is ticked (a default of the C++ workload), and that the **English** language pack is installed (**Language packs** tab), which vcpkg also needs. Then configure again |
| `has no v143 build tools`, or `error MSB8020: The build tools for Visual Studio 2022 (Platform Toolset = 'v143') cannot be found` | The Visual Studio CMake picked lacks the v143 compiler. The message names it and gives the command to add the component; or, in the Visual Studio Installer, click **Modify** on that install and tick **MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)**. With more than one install (say Community and Build Tools), it may be the one you didn't expect: see [More than one Visual Studio installed](BUILD_WINDOWS_VISUAL_STUDIO.md#more-than-one-visual-studio-installed). Then delete the `build` folder and configure again |
| Errors about paths that are too long | Keep the repository near the root of the drive (step 3), and if your Windows user name is long, set a short `TIVOLI_VCPKG_BASE` such as `C:\vcpkg` (step 4) |
| `'stdext': is not a class or namespace name` in Qt's `qvector.h` | Visual Studio 2026's own compiler can't build Qt 5.15's headers. Configure with the preset (step 4), which uses the v143 compiler, in a new build folder. Configuring by hand, add `-T v143` |
| `Compatibility with CMake < 3.5 has been removed`, when configuring Darkride | Configure with the preset (step 4), which sets it. Configuring by hand, add `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` |
| `Invalid CMAKE_POLICY_VERSION_MINIMUM value "3"` | Configuring by hand in PowerShell, the unquoted argument was split at the dot. Use the preset (step 4), or quote it: `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` |
| `generator platform: x64 Does not match the platform used previously` | The build folder was configured before in another way, for example with a plain `cmake ..`, even if that run failed. Delete the `build` folder (or its `CMakeCache.txt` and `CMakeFiles`) and run step 4 again |
| The same CMake error after you fixed its cause | Delete `build\CMakeCache.txt`, or the whole `build` folder, and run step 4 again |
| `cmake` isn't recognised | Use **Developer PowerShell for VS 2026** from the Start menu (step 4), which puts Visual Studio's CMake on the PATH, or install CMake: `winget install -e --id Kitware.CMake` |
| A dependency failed to build | vcpkg's log for it is in `%TIVOLI_VCPKG_BASE%\<id>\buildtrees\<package>\` |
| Explore shows no worlds | Set `HIFI_METAVERSE_URL` (step 6) |

## Optional: Qt with H.264 and AAC

To play H.264/AAC video in web entities, build Qt WebEngine with Chromium's proprietary codecs, then build Darkride against that Qt. Only the WebEngine module is rebuilt; everything else comes from the official Qt you installed in step 2.

**H.264 and AAC are patented.** A build with these codecs is fine for your own use, but distributing it may require patent licences.

**What it costs:**

-   **1½–3 hours** on a fast PC, with the machine busy throughout.
-   **About 60 GB** of disk space.
-   **At least 16 GB of RAM.** Lower `NINJAJOBS` if memory is short.

**Additional tools.** Qt WebEngine 5.15.2 (Chromium 83) predates current Visual Studio:

-   **Visual Studio 2019's compiler (v14.29) with its ATL, and the Windows 10 SDK 10.0.19041.** Visual Studio 2026 and 2022 both still offer the compiler as a component. Visual Studio 2026 doesn't offer that SDK, but Microsoft's SDK archive does. See [Extra components for the optional Qt with H.264 and AAC](BUILD_WINDOWS_VISUAL_STUDIO.md#extra-components-for-the-optional-qt-with-h264-and-aac).
-   **Python 2.7**, which Chromium 83's build scripts need. It doesn't have to go on your PATH, or even be installed: this unpacks it into a folder without administrator rights:

    ```cmd
    curl -LO https://www.python.org/ftp/python/2.7.18/python-2.7.18.amd64.msi
    msiexec /a python-2.7.18.amd64.msi /qn TARGETDIR=C:\python27
    ```

**Build it:**

1.  **Copy the Qt from step 2**, so you keep the original. Add a `qt.conf` so the copy's `qmake` installs into the copy:

    ```cmd
    robocopy C:\Qt\5.15.2\msvc2019_64 C:\Qt-codecs\5.15.2\msvc2019_64 /E /NFL /NDL
    (echo [Paths]& echo Prefix=..) > C:\Qt-codecs\5.15.2\msvc2019_64\bin\qt.conf
    ```

2.  **Run the build script** from a normal command prompt. It sets up the Visual Studio environment itself. The work folder must be outside any git repository and have a short path.

    ```cmd
    tools\qt-builder\build-qtwebengine-codecs-windows.bat C:\Qt-codecs\5.15.2\msvc2019_64 C:\qwe C:\python27
    ```

    The script:
    -   downloads the Qt WebEngine 5.15.2 source and checks its SHA-256;
    -   applies the patches in `tools/qt-builder/patches`;
    -   downloads jom and Qt's gperf/bison/flex;
    -   builds only what Darkride needs: release only (it marks the Qt copy's `mkspecs\qconfig.pri` release-only and keeps the original as `qconfig.pri.orig`), with `-webengine-proprietary-codecs`, and without QtPdf;
    -   installs into the Qt copy.

    If it stops, run it again: it carries on where it left off.

3.  **Build Darkride against the copy**, in a new build folder, because CMake won't switch Qt in an existing one:

    ```cmd
    set QT_CMAKE_PREFIX_PATH=C:/Qt-codecs/5.15.2/msvc2019_64/lib/cmake
    mkdir build-codecs
    cd build-codecs
    cmake .. -G "Visual Studio 18 2026" -A x64 -T v143 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    cmake --build . --target interface --config RelWithDebInfo
    ```
