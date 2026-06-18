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

REM Map MSBuild platform name to vcvarsall arch name.
set "VCARCH=x86"
if /I "%P%"=="x64" set "VCARCH=x64"

REM Set up the matching compiler environment if we found vcvarsall.
if defined VCVARSALL (
    call "%VCVARSALL%" %VCARCH% >nul || ( echo ERROR: vcvarsall %VCARCH% failed & exit /b 1 )
)

REM Clean this platform's artifacts (avoids PCH / arch mismatches).
if exist obj\%P% rmdir /s /q obj\%P%
if exist bin\%P% rmdir /s /q bin\%P%

msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1
msbuild foo_strip.vcxproj /p:Configuration=%CONFIG% /p:Platform=%P% /p:PlatformToolset=%TOOLSET% || exit /b 1

echo   -> bin\%P%\%CONFIG%\foo_strip.dll
exit /b 0

REM ---------------------------------------------------------------------------
:package
echo.
echo === Packaging foo_strip.fb2k-component ===
set "W32=bin\Win32\%CONFIG%\foo_strip.dll"
set "X64=bin\x64\%CONFIG%\foo_strip.dll"
if not exist "%W32%" ( echo ERROR: missing %W32% & exit /b 1 )
if not exist "%X64%" ( echo ERROR: missing %X64% & exit /b 1 )

if exist pkg rmdir /s /q pkg
mkdir pkg
copy /y "%W32%" pkg\foo_strip.dll >nul
copy /y "%X64%" pkg\foo_strip-x64.dll >nul

if exist foo_strip.fb2k-component del /q foo_strip.fb2k-component
powershell -NoProfile -Command "Compress-Archive -Path pkg\* -DestinationPath foo_strip.zip -Force; Rename-Item foo_strip.zip foo_strip.fb2k-component" || exit /b 1
rmdir /s /q pkg
echo   -> foo_strip.fb2k-component  (install via Preferences ^> Components)
exit /b 0
