# foo_strip

<p align="center">
<img width="300" height="352" alt="image" src="https://github.com/user-attachments/assets/84127c8e-566b-4d8f-9a6f-ac19a067a89f" />
<img width="300" height="352" alt="image" src="https://github.com/user-attachments/assets/a4e22952-a722-4bb3-9c9e-dd47abdb5d5a" />
</p>

A standalone, draggable, always-on-top **playback strip** for foobar2000 on
Windows, inspired by Deskband Controls. Album art, scrolling title/artist, transport controls, and a working
seek bar — floating over your desktop, independent of foobar's own window. Reads position/length and seeks **directly**
through foobar's `playback_control` (no SMTC, no bridge).

This is a personal project to solve the problem that Windows 11 doesn't allow taskbar plugins anymore, which means Deskband Controls is no longer available. Since nobody seems to be interested in picking up where Eldarien left off, I asked Claude, Opus 4.8, to build me this for personal use. PRs welcome, especially for build/optimization improvements.

> Independent, unofficial component for foobar2000. Not affiliated with or
> endorsed by the foobar2000 project. See [NOTICE.md](NOTICE.md).

## Features

- Floating, draggable window
  - Hold down shift to snap to edges of your screen
  - Remembers its previous position upon restart
- Scales with DPI
- Album art, title (continuous marquee when long), artist
  - Album art pop-up on hover, and its position depends on where the strip is (pops up below the strip if there are no space on top)
- Previous / play-pause / next with hover + press feedback
- Seek bar with click/drag scrubbing, accurate time readout
- Hides automatically when a fullscreen game/video is in front
- Right-click in the strip to open customization settings
  - change basically everything but the album art thumbnail
- Double-click album art to bring up foobar window
- "Toggle floating strip" under View menu and bindable to a button in Column UI


## Install

Download the build that matches your foobar2000 from [Releases](../../releases):

- **32-bit foobar2000** -> `foo_strip.dll`
  Place in `%APPDATA%\foobar2000-v2\user-components\foo_strip\foo_strip.dll`
- **64-bit foobar2000** -> `foo_strip-x64.dll`
  Place in `%APPDATA%\foobar2000-v2\user-components-x64\foo_strip-x64\foo_strip-x64.dll`

Or download the foo_strip.fb2k-component file and install through Preferences -> Components -> Install

## Build from source

Requirements: **Visual Studio 2022 Build Tools** with the **Desktop development
with C++** workload (provides MSBuild + the v143 toolset). Targets both 32-bit
(`Win32`) and 64-bit (`x64`).

The foobar2000 SDK is vendored under `lib/foobar_sdk/` (source only — the static
libs are built as part of the build, not committed), so no separate SDK download
is needed.

### Easiest: build.bat

Open the Developer Command Prompt for Visual Studio 2022:

```
build.bat all
```

The script cleans stale artifacts, builds the three SDK libs, then the
component. Output: `bin\<Platform>\Release\foo_strip.dll`, and a bundled foo_strip.fb2k-component in the root folder (where the bat file is) for easy install.
 
### Manual

If you'd rather run the steps yourself (same as the script), for each platform
build the three SDK libs first, then the component — e.g. for Win32:

```bat
msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild foo_strip.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
```

Use `/p:Platform=x64` for the 64-bit build. The prebuilt `shared-Win32.lib` /
`shared-x64.lib` ship with the SDK; the other three libs are built by the
commands above.

## Project layout

```
foo_strip.cpp        component plumbing: initquit + play_callback, direct seek
StripWindow.cpp      GDI+ window: paint, drag, hit-testing, marquee, fullscreen
strip_shared.h       shared StripState + cross-file declarations
stdafx.h / .cpp      precompiled header (SDK + Windows includes)
foo_strip.vcxproj    MSBuild project (Win32/x64 DLL, /MD, PCH)
build.bat            one-command build helper
lib/foobar_sdk/      vendored SDK source (build inputs; see NOTICE.md)
```

## License

Component source: [GPLv3](LICENSE). Vendored SDK: BSD-style (GPL-compatible) — see [NOTICE.md](NOTICE.md).

## Acknoledgement

I'd like to thank [@Eldarien](https://github.com/Eldarien) for his Deskband Controls. Please check out his [source code](https://github.com/Eldarien/DeskbandControls)
