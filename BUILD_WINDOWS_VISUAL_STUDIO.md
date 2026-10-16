# Setting up Visual Studio for a Darkride build

This page covers step 1 of [BUILD_WINDOWS.md](BUILD_WINDOWS.md): which Visual Studio to install, and which parts of it the build needs. Everything here is free.

## Which Visual Studio?

| | Status | Use it? |
|---|---|---|
| **Visual Studio 2026** (Community) | Current, offered on Microsoft's download page | **Yes, recommended** |
| **Build Tools for Visual Studio 2026** | The same compilers without the editor | Yes, to build from the command line or [VS Code](BUILD_WINDOWS.md#building-in-vs-code) |
| **Visual Studio 2022** (Community or Build Tools) | Microsoft's download page no longer offers it, but the installer and winget still do; supported until 13 January 2027, and 17.14 gets security fixes until January 2032 | Yes, if you already have it |

## What to install

Only the **Desktop development with C++** workload, with its default components. Of those defaults, the build uses:

| Component (as named in the installer) | Why |
|---|---|
| **MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)** | The compiler everything is built with. Qt 5.15's headers don't compile with Visual Studio 2026's own compiler (14.5x), so the build uses this one (the presets' `v143` toolset). In Visual Studio 2022 it's the workload's own compiler, **MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)** |
| **C++ CMake tools for Windows** | CMake, which configures the build |
| **Windows 11 SDK** | Windows' headers and libraries |

Keep them ticked. The **English** language pack is needed too: vcpkg, which builds Darkride's dependencies, uses only compilers that have it. Visual Studio installs it with an English Windows; on another language, add it on the installer's **Language packs** tab.

> **Set up for an earlier version of this guide?** It asked for the **v14.39** build tools and **ATL**, because the build couldn't use anything newer. They're no longer needed, and you can remove them in the Visual Studio Installer (or leave them: the build uses the newest v143 compiler it finds).

## Installing with the Visual Studio Installer (point and click)

1.  Download **Visual Studio 2026 Community**, or **Build Tools for Visual Studio 2026**, from https://visualstudio.microsoft.com/downloads, and run it.

    For Visual Studio 2022 instead, use https://aka.ms/vs/17/release/vs_community.exe, a direct Microsoft link.

2.  On the **Workloads** tab, tick **Desktop development with C++**. The list on the right shows its components; leave the defaults as they are.
3.  Click **Install**. Expect about 8 GB.

If Visual Studio is already installed: open the **Visual Studio Installer** from the Start menu and click **Modify** on your installation, or in Visual Studio choose **Tools → Get Tools and Features...**. Then do step 2, and check that the components in [What to install](#what-to-install) are ticked; they may not have been if the workload was installed some other way.

## Installing from the command line

Run these in a terminal; Windows asks for administrator rights. `--includeRecommended` adds the workload's default components, and `--passive` shows progress without asking questions.

**Visual Studio 2026 Community:**

```cmd
winget install -e --id Microsoft.VisualStudio.Community --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --norestart"
```

**Build Tools for Visual Studio 2026:**

```cmd
winget install -e --id Microsoft.VisualStudio.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart"
```

**Visual Studio 2022:** the same, with `--id Microsoft.VisualStudio.2022.Community` or `--id Microsoft.VisualStudio.2022.BuildTools`.

**Adding the workload to an existing installation:**

```cmd
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\18\Community" --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --norestart
```

For Build Tools the path is `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` and the workload is `Microsoft.VisualStudio.Workload.VCTools`. For Visual Studio 2022, use `2022` in place of `18`.

## Checking the installation

Open **Developer PowerShell for VS 2026** (or for VS 2022) from the Start menu, and run:

```cmd
dir "$env:VCINSTALLDIR\Tools\MSVC"
cmake --version
```

The first should list a `14.4x.xxxxx` folder, such as `14.44.35207`, next to Visual Studio 2026's own `14.5x`. The second should print a CMake version.

## More than one Visual Studio installed

CMake uses the newest Visual Studio for the generator you name (`Visual Studio 18 2026` or `Visual Studio 17 2022`), and that can be a **Build Tools** installation rather than Community. Install the C++ workload in the one CMake actually uses, or name one on the first configure:

```cmd
cmake --preset vs2026 "-DCMAKE_GENERATOR_INSTANCE=C:/Program Files/Microsoft Visual Studio/18/Community"
```

vcpkg separately picks the newest Visual Studio that has a v143 compiler.

## Extra components for the optional Qt with H.264 and AAC

Only needed for [Optional: Qt with H.264 and AAC](BUILD_WINDOWS.md#optional-qt-with-h264-and-aac). Qt WebEngine 5.15.2 (Chromium 83) is built with Visual Studio 2019's compiler and an older Windows SDK. Both Visual Studio 2026 and 2022 still offer the compiler:

| Component | ID |
|---|---|
| **MSVC v142 - VS 2019 C++ x64/x86 build tools (v14.29-16.11)** | `Microsoft.VisualStudio.Component.VC.14.29.16.11.x86.x64` |
| **C++ v14.29 (16.11) ATL for v142 build tools (x86 & x64)** | `Microsoft.VisualStudio.Component.VC.14.29.16.11.ATL` |
| **Windows 10 SDK (10.0.19041.0)**: Visual Studio 2022 offers it as a component (`Microsoft.VisualStudio.Component.Windows10SDK.19041`). Visual Studio 2026 doesn't, so install it from Microsoft's [Windows SDK archive](https://developer.microsoft.com/windows/downloads/sdk-archive/). | |
