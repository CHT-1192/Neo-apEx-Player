#pragma once
// The apEx tree includes the D3D10 headers for two things only: ID3D10Blob
// (used as the shader-compilation blob, see compat/D3Dcommon.h) and, via
// phxSplineExt.h, the D3DX10 math types.  Everything else it needs lives in
// the D3D11 shim, so these headers just forward to it instead of pretending to
// implement D3D10.
#include "D3Dcommon.h"
