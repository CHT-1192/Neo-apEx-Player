#pragma once
// <pathcch.h> is the modern Windows path API.  The loader only needs
// PathCchRemoveFileSpec/PathCchCombine, which our shlwapi shim already covers.
#include "shlwapi.h"
#define PathCchRemoveFileSpec PathRemoveFileSpecA
#define PathCchCombine PathCombineA
#define PathCchAppend PathAppendA
