// stdafx.h — precompiled header
#pragma once

// Windows
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>  // timeGetTime (used by pfc/timers.h)
#include <shlwapi.h>   // SHCreateMemStream

// foobar2000 SDK. The SDK root is on the include path (AdditionalIncludeDirectories
// = $(SdkRoot)), so this resolves to <SdkRoot>\foobar2000\SDK\foobar2000.h.
#include "foobar2000/SDK/foobar2000.h"
