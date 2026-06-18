# Third-party notices

This repository vendors portions of the **foobar2000 SDK** under `lib/foobar_sdk/`
solely to allow the component to build (locally and in CI). Those files remain
under their original licenses, and their license files are preserved in place:

- **foobar2000 SDK** — BSD-style license (2-clause + no-endorsement).
  See `lib/foobar_sdk/foobar2000/SDK/` and the SDK's `readme` / license text.
  Copyright (c) 2002-2025, Peter Pawlowski. All rights reserved.

- **pfc** and **libPPUI** — separate, less restrictive licenses. See the license
  files inside `lib/foobar_sdk/pfc/` and the respective folders.

This project is an independent, unofficial component **for** foobar2000. It is
**not** affiliated with, endorsed by, or sponsored by Peter Pawlowski or the
foobar2000 project. The foobar2000 name is used only to describe compatibility.

The component's own source (`foo_strip.cpp`, `StripWindow.cpp`, `strip_shared.h`,
`stdafx.*`) is licensed under the **GNU General Public License v3.0** (see
`LICENSE`). The vendored SDK's BSD-style license is GPL-compatible, so the
combined work is distributable under the GPLv3 while the SDK files retain their
original BSD notices.
