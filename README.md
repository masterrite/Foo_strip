# foo_strip

A standalone, draggable, always-on-top **playback strip** for foobar2000 on
Windows. Album art, scrolling title/artist, transport controls, and a working
seek bar — floating over your desktop, independent of foobar's own window.

<img width="300" height="54" alt="Snipaste_2026-06-17_19-50-11" src="https://github.com/user-attachments/assets/6e2e2ddc-e437-4969-849a-7844d262afaf" />

Because it runs in-process, it reads position/length and seeks **directly**
through foobar's `playback_control` (no SMTC, no bridge).

> Independent, unofficial component **for** foobar2000. Not affiliated with or
> endorsed by the foobar2000 project. See [NOTICE.md](NOTICE.md).

## Features

- Floating, draggable window.
- Album art, title (continuous marquee when long), artist.
- Previous / play-pause / next with hover + press feedback.
- Seek bar with click/drag scrubbing, accurate time readout.
- Hides automatically when a fullscreen game/video is in front.

## Install

Grab the latest `foo_strip.dll` (or `foo_strip.fb2k-component`) from
[Releases](../../releases), then in foobar2000:

- **Preferences -> Components -> Install...**, pick the file, and restart. Or drop
  `foo_strip.dll` into
  `%APPDATA%\foobar2000-v2\user-components\foo_strip\`.

The strip appears bottom-right on startup. Drag it anywhere by an empty area.

## Build from source

Requirements: **Visual Studio 2022 Build Tools** (Desktop C++ workload).
Targets both **32-bit** (`Win32`) and **64-bit** (`x64`) foobar2000.

The foobar2000 SDK is vendored under `lib/foobar_sdk/`, so no separate download
is needed. From a **Developer Command Prompt**, pick your platform (`Win32` or
`x64`) and build the SDK libs once for it, then the component:

```bat
:: --- 32-bit (use the x86 Developer Command Prompt) ---
msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild foo_strip.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143

:: --- 64-bit (use the x64 Developer Command Prompt) ---
msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
msbuild foo_strip.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

Output: `bin\Win32\Release\foo_strip.dll` and `bin\x64\Release\foo_strip.dll`.
Install the one matching your foobar2000 architecture. `shared-Win32.lib` and
`shared-x64.lib` both ship prebuilt in the SDK; the other three libs you build
per-platform as shown.

Using the SDK from a different location instead of the vendored copy? Override:
```bat
msbuild foo_strip.vcxproj /p:SdkRoot=C:\path\to\sdk\ /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
```

## Project layout

```
foo_strip.cpp        component plumbing: initquit + play_callback, direct seek
StripWindow.cpp      GDI+ window: paint, drag, hit-testing, marquee, fullscreen
strip_shared.h       shared StripState + cross-file declarations
stdafx.h / .cpp      precompiled header (SDK + Windows includes)
foo_strip.vcxproj    MSBuild project (Win32 DLL, /MD, PCH)
lib/foobar_sdk/      vendored SDK (build inputs; see NOTICE.md)
```

## License

Component source: [GPLv3](LICENSE). Vendored SDK: BSD-style (GPL-compatible) — see [NOTICE.md](NOTICE.md).
