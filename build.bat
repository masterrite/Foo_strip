@echo off
REM ===========================================================================
REM  build.bat - build foo_strip from a clean checkout.
REM
REM  Usage:
REM      build.bat Win32     build 32-bit only (run from x86 Dev Prompt, or any
REM                          prompt - it sets up the env itself, see below)
REM      build.bat x64       build 64-bit only
REM      build.bat all       build BOTH and package foo_strip.fb2k-component
REM      build.bat           same as "all"
REM
REM  "all" needs to switch between x86 and x64 compiler environments, so it
REM  locates vcvarsall.bat via vswhere and sets up each env itself. For single-
REM  platform builds you can either run from the matching Developer Command
REM  Prompt, or let the script set up the env (it tries vcvarsall too).
REM
REM  Outputs:
REM      bin\Win32\Release\foo_strip.dll
REM      bin\x64\Release\foo_strip.dll
REM      foo_strip.fb2k-component   (only in "all" mode: both DLLs zipped,
REM                                  32-bit as foo_strip.dll, 64-bit as
REM                                  foo_strip-x64.dll)
REM ===========================================================================

setlocal enabledelayedexpansion

set MODE=%1
if "%MODE%"=="" set MODE=all

set CONFIG=Release
set TOOLSET=v143

REM --- Locate vcvarsall.bat (for switching x86/x64 envs) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARSALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

REM --- Sanity check: we need a way to build. Either vcvarsall was found (so we
REM     can set up the env ourselves), or msbuild is already on PATH (you ran
REM     this from a Developer Command Prompt). If NEITHER, the build can only
REM     produce broken output -- bail with instructions. This is the #1 mistake:
REM     double-clicking the .bat, which has no compiler environment. ---
where msbuild >nul 2>&1
set "MSB_ON_PATH=%ERRORLEVEL%"
if not defined VCVARSALL if not "%MSB_ON_PATH%"=="0" (
    echo.
    echo ============================================================
    echo  ERROR: no Visual Studio build environment found.
    echo.
    echo  Don't double-click this file. Run it from a
    echo  "Developer Command Prompt for VS 2022" instead:
    echo.
    echo    1. Start menu - "Developer Command Prompt for VS 2022"
    echo    2. cd into this folder
    echo    3. build.bat all
    echo ============================================================
    echo.
    pause
    exit /b 1
)

if /I "%MODE%"=="all" goto :build_all
if /I "%MODE%"=="Win32" ( set ARCHES=Win32 & goto :build_list )
if /I "%MODE%"=="x64"   ( set ARCHES=x64   & goto :build_list )

echo ERROR: argument must be Win32, x64, all, or empty.
exit /b 1

:build_all
set ARCHES=Win32 x64

:build_list
for %%P in (%ARCHES%) do (
    call :build_one %%P || exit /b 1
)

if /I "%MODE%"=="all" call :package || exit /b 1
echo.
echo === Done ===
endlocal
exit /b 0

REM ---------------------------------------------------------------------------
:build_one
REM %1 = Win32 or x64
set "P=%~1"
echo.
echo === Building [%CONFIG% ^| %P%] ===

REM msbuild cross-targets both platforms via /p:Platform from a single Developer
REM Command Prompt environment -- so if msbuild is already on PATH (you ran this
REM from a Dev Prompt), DO NOT call vcvarsall. Re-running vcvarsall on top of an
REM already-configured environment can corrupt it and produce broken (1KB) DLLs.
REM Only set up the env via vcvarsall as a fallback when msbuild is NOT on PATH.
if not "%MSB_ON_PATH%"=="0" (
    if defined VCVARSALL (
        set "VCARCH=x86"
        if /I "%P%"=="x64" set "VCARCH=x64"
        call "%VCVARSALL%" !VCARCH! >nul || ( echo ERROR: vcvarsall failed & exit /b 1 )
    )
)

REM Clean this platform's artifacts (avoids PCH / arch mismatches).
if exist obj\%P% rmdir /s /q obj\%P%
if exist bin\%P% rmdir /s /q bin\%P%

msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild foo_strip.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1

echo   -^> bin\%P%\%CONFIG%\foo_strip.dll
exit /b 0

REM ---------------------------------------------------------------------------
:package
echo.
echo === Packaging foo_strip.fb2k-component ===
set "W32=bin\Win32\%CONFIG%\foo_strip.dll"
set "X64=bin\x64\%CONFIG%\foo_strip.dll"
if not exist "%W32%" ( echo ERROR: missing %W32% & exit /b 1 )
if not exist "%X64%" ( echo ERROR: missing %X64% & exit /b 1 )

REM foobar2000 packaging convention for a dual-arch component:
REM   foo_strip.dll          (32-bit, at the archive root)
REM   x64\foo_strip.dll      (64-bit, in an x64 subfolder)
REM BOTH are named foo_strip.dll; the architecture is determined by the folder,
REM not the filename. foobar loads the root DLL for 32-bit, x64\ for 64-bit.
REM We zip the built DLLs directly (no staging folder needed).

if exist foo_strip.fb2k-component del /q foo_strip.fb2k-component
if exist foo_strip.zip del /q foo_strip.zip
REM Build the zip by adding the two FILE entries explicitly. We do NOT use
REM Compress-Archive (PowerShell 5.1 writes entries foobar can't parse) and we
REM do NOT use ZipFile.CreateFromDirectory either, because that adds a standalone
REM "x64/" DIRECTORY entry to the archive. foobar2000's extractor chokes on that
REM empty-directory entry: its "overwrite root DLL with the x64\ DLL" step fails,
REM so 64-bit foobar loads the 32-bit root DLL ("not a valid Win32 application").
REM Adding only the two file entries (foo_strip.dll and x64/foo_strip.dll, with
REM the x64 folder IMPLIED by the path) matches what 7-Zip / the GitHub CI build
REM produce and installs correctly.
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $zip=[System.IO.Compression.ZipFile]::Open('foo_strip.zip','Create'); [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, '%W32%', 'foo_strip.dll'); [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, '%X64%', 'x64/foo_strip.dll'); $zip.Dispose()" || exit /b 1
ren foo_strip.zip foo_strip.fb2k-component
echo   -^> foo_strip.fb2k-component  (foo_strip.dll + x64/foo_strip.dll)
exit /b 0
