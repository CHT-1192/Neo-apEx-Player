#pragma once
// Minimal <shlwapi.h>: the shell path helpers the project loader uses to
// resolve <songfile> relative to the .apx.  Implemented in
// src/platform/win32_compat.cpp.
#include "Windows.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL PathRemoveFileSpecA(char *path);
BOOL PathCombineA(char *dest, const char *dir, const char *file);
BOOL PathAppendA(char *path, const char *more);
BOOL PathFileExistsA(const char *path);
BOOL PathIsRelativeA(const char *path);
char *PathFindFileNameA(const char *path);
void PathStripPathA(char *path);
void PathRemoveExtensionA(char *path);
char *PathFindExtensionA(const char *path);

#ifdef __cplusplus
}
#endif
