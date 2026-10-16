@echo off
rem Builds Qt WebEngine 5.15.2 with Chromium's proprietary codecs (H.264, AAC, MP3) and installs it into an
rem existing Qt 5.15.2 for MSVC 2019 x64, such as a copy of the official one. Only WebEngine is rebuilt.
rem See "Optional: Qt with H.264 and AAC" in BUILD_WINDOWS.md for the tools it needs.
rem
rem   build-qtwebengine-codecs-windows.bat <Qt dir> <work dir> <Python 2.7 dir>
rem
rem   <Qt dir>          the Qt to build against and install into, e.g. C:\Qt-codecs\5.15.2\msvc2019_64
rem   <work dir>        a short path outside any git repository, for the source and build (about 60 GB), e.g. C:\qwe
rem   <Python 2.7 dir>  the folder holding python.exe 2.7, e.g. C:\python27
rem
rem NINJAJOBS sets the number of parallel compiles (default -j12); lower it if the machine runs out of memory.
rem Running the script again carries on where it stopped.

setlocal
if "%~3"=="" (
    echo usage: %~nx0 ^<Qt dir^> ^<work dir^> ^<Python 2.7 dir^>
    exit /b 1
)
set QT_DIR=%~1
set WORK=%~2
set PY2=%~3
set PATCHES=%~dp0patches
if "%NINJAJOBS%"=="" set NINJAJOBS=-j12

set SRC_NAME=qtwebengine-everywhere-src-5.15.2
set SRC_URL=https://download.qt.io/archive/qt/5.15/5.15.2/submodules/%SRC_NAME%.tar.xz
set SRC_SHA256=c8afca0e43d84f7bd595436fbe4d13a5bbdb81ec5104d605085d07545b6f91e0

if not exist "%QT_DIR%\bin\qmake.exe" (
    echo No qmake.exe in "%QT_DIR%\bin".
    exit /b 1
)
if not exist "%PY2%\python.exe" (
    echo No python.exe in "%PY2%"; Chromium 83's build scripts need Python 2.7.
    exit /b 1
)

rem Visual Studio 2022 with the v142 (14.29) compiler, which Chromium 83 was written for
set VSDIR=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.14.29.16.11.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo No Visual Studio with the MSVC v142 14.29 build tools; see BUILD_WINDOWS.md.
    exit /b 1
)
if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.19041.0" (
    echo The Windows 10 SDK 10.0.19041.0 is missing; see BUILD_WINDOWS.md.
    exit /b 1
)

if not exist "%WORK%" mkdir "%WORK%"
cd /d "%WORK%" || exit /b 1

rem the source, checked against Qt's published SHA-256, with this repository's patches applied once
if not exist src (
    if not exist %SRC_NAME%.tar.xz (
        echo Downloading %SRC_URL%
        curl -fL -o %SRC_NAME%.tar.xz %SRC_URL% || exit /b 1
    )
    certutil -hashfile %SRC_NAME%.tar.xz SHA256 | findstr /i /c:"%SRC_SHA256%" >nul || (
        echo %SRC_NAME%.tar.xz does not match its SHA-256; delete it and run this again.
        exit /b 1
    )
    echo Extracting the source
    "%SystemRoot%\System32\tar.exe" -xf %SRC_NAME%.tar.xz || exit /b 1
    pushd %SRC_NAME%
    for %%p in (chromium-msvc-fixes chromium-rtc-use-h264-with-msvc chromium-override-audio-output-permission) do (
        git apply -p2 --ignore-space-change --ignore-whitespace "%PATCHES%\%%p.patch" || exit /b 1
    )
    popd
    ren %SRC_NAME% src || exit /b 1
)

rem jom (parallel nmake), and gperf, bison and flex from Qt's own Windows bundle
if not exist tools\jom\jom.exe (
    if not exist tools\jom mkdir tools\jom
    curl -fL -o tools\jom.zip https://download.qt.io/official_releases/jom/jom_1_1_4.zip || exit /b 1
    "%SystemRoot%\System32\tar.exe" -xf tools\jom.zip -C tools\jom || exit /b 1
)
if not exist tools\qt5-5.15.2\gnuwin32\bin\gperf.exe (
    curl -fL -o tools\qt5-5.15.2.zip https://github.com/qt/qt5/archive/refs/tags/v5.15.2.zip || exit /b 1
    "%SystemRoot%\System32\tar.exe" -xf tools\qt5-5.15.2.zip -C tools || exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.19041.0 -vcvars_ver=14.29 || exit /b 1
set PATH=%PY2%;%WORK%\tools\qt5-5.15.2\gnuwin32\bin;%WORK%\tools\jom;%PATH%
rem the build runs tools it has just made by bare name from the current directory (ninja's bootstrap, gn)
set NoDefaultCurrentDirectoryInExePath=
rem the official Qt ships qtmain.lib without its PDB, and WebEngine links with warnings as errors, so
rem QtWebEngineProcess.exe would stop at LNK4099 (PDB not found); link.exe reads extra options from LINK
set LINK=/ignore:4099

rem Release only. The official Qt lists debug_and_release and build_all among its features (qconfig.pri),
rem and WebEngine's gn_run.pro asks qtConfig() for them, which overrides CONFIG on qmake's command line,
rem so it would also build a debug Chromium (hours more). The Qt being built against is a copy, so its
rem qconfig.pri is made release-only; the original is kept as qconfig.pri.orig. Darkride's CMake build
rem does not read this file.
powershell -NoProfile -Command "$f = '%QT_DIR%\mkspecs\qconfig.pri'; $c = Get-Content $f; if ($c -match '^QT\.global\.enabled_features = .* debug_and_release ') { if (-not (Test-Path ($f + '.orig'))) { Copy-Item $f ($f + '.orig') }; $c = $c -replace '^(QT_CONFIG \+= .*) debug_and_release debug release build_all ', '$1 release ' -replace '^(QT\.global\.enabled_features = .*) debug_and_release build_all ', '$1 ' -replace '^(QT\.global\.disabled_features = .*)$', '$1 debug_and_release build_all'; Set-Content $f $c -Encoding ascii }" || exit /b 1

if not exist build mkdir build
cd build
"%QT_DIR%\bin\qmake.exe" ..\src\qtwebengine.pro -- -webengine-proprietary-codecs -no-build-qtpdf || exit /b 1
if "%CONFIGURE_ONLY%"=="1" exit /b 0
jom || exit /b 1
jom install || exit /b 1
echo Qt WebEngine with proprietary codecs is installed in %QT_DIR%.
