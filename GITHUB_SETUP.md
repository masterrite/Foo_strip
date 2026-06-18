# Setting up the GitHub repo

These steps arrange your files into the repo layout the project + CI expect,
then push to GitHub. Do this once.

## 1. Make the repo folder

Create a fresh folder for the repo (separate from your messy SDK build folder):

```
C:\dev\foo_strip\
```

Copy your component files into it (the root of the repo):

```
foo_strip.cpp
StripWindow.cpp
strip_shared.h
stdafx.cpp
stdafx.h
foo_strip.vcxproj
.gitignore
.github\workflows\build.yml
README.md
NOTICE.md
LICENSE          (see note below — paste the full GPLv3 text + your name)
```

> **LICENSE needs one action:** the included `LICENSE` has the GPLv3 *notice* and
> instructions, but not the full license text. Open
> <https://www.gnu.org/licenses/gpl-3.0.txt>, copy the entire plain-text license,
> and replace the marked section in `LICENSE` with it. Also put your name in the
> copyright line. GitHub will then correctly detect the repo as GPL-3.0.

## 2. Vendor the SDK

Copy the SDK into `lib\foobar_sdk\` inside the repo, preserving its structure:

```
C:\dev\foo_strip\lib\foobar_sdk\
  ├── foobar2000\
  │   ├── SDK\
  │   ├── shared\          (keep the prebuilt shared-*.lib here)
  │   └── foobar2000_component_client\
  └── pfc\
```

You only need those folders (the ones the build references). **Keep every
license/readme file** inside them — that's the redistribution condition.

> The `.gitignore` excludes SDK *build outputs* (Release/, Debug/, *.lib, *.obj)
> but force-keeps `foobar2000/shared/*.lib`, which ships prebuilt. So a local
> build won't accidentally commit compiled libs, but the prebuilt shared lib is
> retained.

## 3. Verify it builds locally in the new layout

From a 32-bit Developer Command Prompt, in `C:\dev\foo_strip`:

```bat
msbuild lib\foobar_sdk\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\pfc\pfc.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild lib\foobar_sdk\foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
msbuild foo_strip.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143
```

If `bin\Release\foo_strip.dll` appears, the layout is correct and CI will work
too (it runs the same commands).

## 4. Initialize git and push

Create an empty repo on GitHub first (no README/license — you have them).
Then, in `C:\dev\foo_strip`:

```bash
git init
git add .
git status        # SANITY CHECK: confirm no *.lib/*.obj/bin/obj are staged,
                  # but lib/foobar_sdk source files ARE. License files present.
git commit -m "Initial commit: floating playback strip for foobar2000"
git branch -M main
git remote add origin https://github.com/<you>/foo_strip.git
git push -u origin main
```

Watch the **Actions** tab — the build should run and produce a `foo_strip-dll`
artifact.

## 5. Cut a release

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow builds, packages `foo_strip.fb2k-component`, and creates a Release
with both files attached.

## Gotchas

- **`git status` before committing.** The #1 mistake is accidentally committing
  built `.lib`/`.obj` from a local SDK build, or — worse — NOT committing the SDK
  source the CI needs. Confirm: SDK `.cpp`/`.h` present, build outputs absent,
  `shared-Win32.lib` present.
- **Repo size.** The vendored SDK source is a few MB — fine. If `git status`
  shows hundreds of MB, you're including build output; fix `.gitignore` matches.
- **CI toolset.** The workflow pins `v143` (VS 2022 on the `windows-2022`
  runner). If you later switch the runner to `windows-2025`, change to the
  toolset that image provides.
- **First CI run fails on SDK paths?** The runner layout matches yours only if
  the SDK is under `lib/foobar_sdk/` exactly. Compare the failing path in the
  Actions log to where the file actually is in your repo tree on GitHub.
