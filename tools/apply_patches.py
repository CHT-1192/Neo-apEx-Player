#!/usr/bin/env python3
"""Apply the (small, documented) set of source patches to the vendored apEx tree.

third_party/apEx is a copy of the Conspiracy apEx source release, which targets
MSVC/Windows.  Everything else is handled by the compat/ shims, compiler flags
and src/platform; only the changes below touch the vendored code.  Run this
script after (re)vendoring:

    python3 tools/apply_patches.py

Every patch is idempotent: re-running is a no-op.
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TP = os.path.join(ROOT, "third_party", "apEx")

# (relative path, old text, new text, note)
PATCHES = [
    (
        "Bedrock/BaseLib/StackTracker.h",
        "enum LOGVERBOSITY;",
        "enum LOGVERBOSITY : int;",
        "clang rejects forward declarations of unscoped enums without a fixed "
        "underlying type (MSVC extension).",
    ),
    (
        "Bedrock/BaseLib/Logger.h",
        "enum LOGVERBOSITY\n{",
        "enum LOGVERBOSITY : int\n{",
        "keep the definition consistent with the forward declaration above.",
    ),
    (
        "Bedrock/BaseLib/Dictionary.h",
        """  typedef TS32( *VALUESORTCALLBACK )( const ItemType &a, const ItemType &b );

private:""",
        """  typedef TS32( *VALUESORTCALLBACK )( const ItemType &a, const ItemType &b );
  // clang needs the inherited nested type to be named explicitly...
  typedef typename CDictionary<KeyType, ItemType>::KDPair KDPair;
  // ...and so do inherited members of a dependent base class (MSVC resolves
  // ItemCount/Find/Add/Delete permissively at instantiation time).
  using CDictionary<KeyType, ItemType>::ItemCount;
  using CDictionary<KeyType, ItemType>::Find;
  using CDictionary<KeyType, ItemType>::Add;
  using CDictionary<KeyType, ItemType>::Delete;

private:""",
        "CDictionaryEnumerable uses the base class' nested KDPair and the members "
        "ItemCount/Find/Add/Delete unqualified.",
    ),
    (
        "Bedrock/BaseLib/BaseLib.h",
        "#include <WinSock2.h>\n#include <Windows.h>\n#include <comdef.h>\n",
        "#include <Windows.h>\n",
        "the sockets/COM headers are not needed by the player and drag in a lot "
        "of unimplemented Win32 surface.",
    ),
    (
        "Bedrock/BaseLib/BaseLib.h",
        "#include \"Socket.h\"\n",
        "",
        "CSocket is WinSock-only and unused by the engine.",
    ),
    (
        "Bedrock/BaseLib/BaseLib.h",
        "#include \"CrashTracker.h\"\n",
        "",
        "CCrashTracker walks the Windows TEB/PEB; unused by the player.",
    ),
    (
        "Bedrock/BaseLib/Matrix.cpp",
        "  CVector3 &Z = Target - Eye;\n  Z.Normalize();\n  CVector3 &X = Up%Z;\n  CVector3 &Y = Z%X;",
        "  CVector3 Z = Target - Eye;\n  Z.Normalize();\n  CVector3 X = Up%Z;\n  CVector3 Y = Z%X;",
        "MSVC allows binding a temporary to a non-const reference; clang does not.",
    ),
    (
        "Bedrock/BaseLib/Matrix.cpp",
        "  CVector3 &Z = Target - Eye;\n  Z.Normalize();\n  CVector3 &X = Up%Z;\n  CVector3 &Y = Z%X;",
        "  CVector3 Z = Target - Eye;\n  Z.Normalize();\n  CVector3 X = Up%Z;\n  CVector3 Y = Z%X;",
        "second occurrence (SetLookAtLH).",
    ),
    (
        "Bedrock/BaseLib/Quaternion.cpp",
        "  CQuaternion &invq1 = q1.Inverted();",
        "  CQuaternion invq1 = q1.Inverted();",
        "same MSVC non-const-reference-to-temporary extension.",
    ),
    (
        "Bedrock/BaseLib/Logger.h",
        "#define LOG(v,s,...) Logger.Log(v,true,true,s,__VA_ARGS__)",
        "#define LOG(v,s,...) Logger.Log(v,true,true,s, ##__VA_ARGS__)",
        "MSVC's traditional preprocessor drops the trailing comma when __VA_ARGS__ is empty.",
    ),
    (
        "Bedrock/BaseLib/Logger.h",
        "#define LOGSIMPLE(v,s,...) Logger.Log(v,false,false,s,__VA_ARGS__)",
        "#define LOGSIMPLE(v,s,...) Logger.Log(v,false,false,s, ##__VA_ARGS__)",
        "same empty-__VA_ARGS__ issue.",
    ),
    (
        "Bedrock/BaseLib/Logger.h",
        """#define LOG_WARN(s,...) LOG(LOG_WARNING,_T(s),__VA_ARGS__)
#define LOG_ERR(s,...) LOG(LOG_ERROR,_T(s),__VA_ARGS__)
#define LOG_DBG(s,...) LOG(LOG_DEBUG,_T(s),__VA_ARGS__)
#define LOG_NFO(s,...) LOG(LOG_INFO,_T(s),__VA_ARGS__)""",
        """#define LOG_WARN(s,...) LOG(LOG_WARNING,_T(s), ##__VA_ARGS__)
#define LOG_ERR(s,...) LOG(LOG_ERROR,_T(s), ##__VA_ARGS__)
#define LOG_DBG(s,...) LOG(LOG_DEBUG,_T(s), ##__VA_ARGS__)
#define LOG_NFO(s,...) LOG(LOG_INFO,_T(s), ##__VA_ARGS__)""",
        "same empty-__VA_ARGS__ issue.",
    ),
    (
        "Bedrock/BaseLib/String.cpp",
        "CString CStringArray::Implode( TCHAR *sDelimiter )\n{\n  return Implode( CString( sDelimiter ) );\n}",
        "CString CStringArray::Implode( TCHAR *sDelimiter )\n{\n  CString Delimiter( sDelimiter );\n  return Implode( Delimiter );\n}",
        "clang will not bind a temporary to CString& (MSVC extension).",
    ),
    (
        "Bedrock/BaseLib/String.cpp",
        "CStringArray CString::Explode( TCHAR *sDelimiter ) const\n{\n  return Explode( CString( sDelimiter ) );\n}",
        "CStringArray CString::Explode( TCHAR *sDelimiter ) const\n{\n  CString Delimiter( sDelimiter );\n  return Explode( Delimiter );\n}",
        "same.",
    ),
    (
        "Bedrock/BaseLib/String.cpp",
        "CStringArrayMarkers CString::GetExplodeMarkers( TCHAR *sDelimiter ) const\n{\n  return GetExplodeMarkers( CString( sDelimiter ) );\n}",
        "CStringArrayMarkers CString::GetExplodeMarkers( TCHAR *sDelimiter ) const\n{\n  CString Delimiter( sDelimiter );\n  return GetExplodeMarkers( Delimiter );\n}",
        "same.",
    ),
    (
        "Bedrock/BaseLib/String.cpp",
        "  TS32 n = _vsctprintf( format, argList );",
        "  va_list probe;\n  va_copy( probe, argList );\n  TS32 n = _vsctprintf( format, probe );\n  va_end( probe );",
        "_vsctprintf is MSVC-only; implemented over vsnprintf in compat/Windows.h and needs a "
        "va_copy so the caller's list survives the probe call.",
    ),
    (
        "Phoenix/Project.cpp",
        """  datastreams = (unsigned char**)DATA;
  for ( int x = 0; x < StreamCount; x++ )
    datastreams[ x ] += (int)DATA;""",
        """  datastreams = datastreamPointers;
  for ( int x = 0; x < StreamCount; x++ )
    datastreams[ x ] = DATA + ( (unsigned int*)DATA )[ x ];""",
        "the original converts the file's stream offsets into pointers in place, "
        "which needs a 32-bit pointer: (int)DATA truncates on a 64-bit build, so "
        "every stream starts in unmapped memory.  The binary loader only became "
        "reachable once the port loaded the loading-screen project.",
    ),
    (
        "Phoenix/Project.cpp",
        '#include "..\\LibCTiny\\libcminimal.h"',
        '#include "../LibCTiny/libcminimal.h"',
        "backslash include separators are a hard error outside MSVC.",
    ),
    (
        "Phoenix/Scene.cpp",
        '#include "..\\LibCTiny\\libcminimal.h"',
        '#include "../LibCTiny/libcminimal.h"',
        "same.",
    ),
    (
        "Phoenix/Mesh.cpp",
        "#include <gl\\gl.h>\n#include <gl\\glu.h>",
        "#ifdef __APPLE__\n// macOS has GLU inside the OpenGL framework; the text mesh generator uses its\n// tessellator.\n#include <OpenGL/gl.h>\n#include <OpenGL/glu.h>\n#else\n#include <GL/gl.h>\n#include <GL/glu.h>\n#endif",
        "backslash include separators plus the macOS OpenGL header location.",
    ),
    (
        "Bedrock/CoRE2/Resource.h",
        "// implement the OnDeviceLost() and OnDeviceReset() functions\n\nclass CCoreResource",
        "// implement the OnDeviceLost() and OnDeviceReset() functions\n\n"
        "// Resource.h refers to CCoreDevice before any header declares it; the original\n"
        "// MSVC build only worked because Device.h happened to be included first\n"
        "// through the precompiled header.\n"
        "class CCoreDevice;\n\n"
        "class CCoreResource",
        "clang needs the forward declaration (MSVC permissive include order).",
    ),
    (
        "Bedrock/CoRE2/Material.cpp",
        """		CString sh = Root->GetAttributeAsString(_T("ShaderCode"));
		GetVertexShader()->SetCode(sh, CString(_T("vsmain")), CString(_T("vs_5_0")));
		GetVertexShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_VERTEX, GetVertexShader());
		GetPixelShader()->SetCode(sh, CString(_T("psmain")), CString(_T("ps_5_0")));
		GetPixelShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_PIXEL, GetPixelShader());
		GetGeometryShader()->SetCode(sh, CString(_T("gsmain")), CString(_T("gs_5_0")));
		GetGeometryShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_GEOMETRY, GetGeometryShader());""",
        """		CString sh = Root->GetAttributeAsString(_T("ShaderCode"));
		// clang will not bind a temporary to CString& (MSVC extension), so the
		// entry point and profile have to be named locals.
		CString vsEntry(_T("vsmain")), vsProfile(_T("vs_5_0"));
		CString psEntry(_T("psmain")), psProfile(_T("ps_5_0"));
		CString gsEntry(_T("gsmain")), gsProfile(_T("gs_5_0"));
		GetVertexShader()->SetCode(sh, vsEntry, vsProfile);
		GetVertexShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_VERTEX, GetVertexShader());
		GetPixelShader()->SetCode(sh, psEntry, psProfile);
		GetPixelShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_PIXEL, GetPixelShader());
		GetGeometryShader()->SetCode(sh, gsEntry, gsProfile);
		GetGeometryShader()->CompileAndCreate(NULL);
		//SetConstantBufferIndices(CORE_SHADER_GEOMETRY, GetGeometryShader());""",
        "same non-const-reference-to-temporary MSVC extension.",
    ),
    (
        "Bedrock/CoRE2/Shader.h",
        "	void SetCode(CString &code, CString &entry, CString &shaderversion)",
        "	// const&: clang will not bind the temporaries the call sites pass\n"
        "	// (MSVC extension).\n"
        "	void SetCode(const CString &code, const CString &entry, const CString &shaderversion)",
        "DX11Device.cpp calls SetCode(CString(...), CString(...), CString(...)) six times.",
    ),
    (
        "Bedrock/CoRE2/Material.h",
        "	void SetName(CString &s);",
        "	void SetName(const CString &s);",
        "two declarations; clang will not bind the temporary that Material.cpp:217 passes.",
        True,
    ),
    (
        "Bedrock/CoRE2/Material.h",
        "	void SetName(CString &name);",
        "	void SetName(const CString &name);",
        "third declaration.",
        True,
    ),
    (
        "Bedrock/CoRE2/Material.cpp",
        "void CCoreMaterialParameter::SetName(CString &s)",
        "void CCoreMaterialParameter::SetName(const CString &s)",
        "match the const-ref declaration.",
    ),
    (
        "Bedrock/CoRE2/Material.cpp",
        "void CCoreMaterialRenderPass::SetName(CString &s)",
        "void CCoreMaterialRenderPass::SetName(const CString &s)",
        "match the const-ref declaration.",
    ),
    (
        "Bedrock/CoRE2/Material.cpp",
        "void CCoreMaterialTechnique::SetName(CString &name)",
        "void CCoreMaterialTechnique::SetName(const CString &name)",
        "match the const-ref declaration.",
    ),
    (
        "MVX/mvx.h",
        "#include <dsound.h>\n\n#define MVX_SAMPLERATE",
        "// The DirectSound output path is not part of this port (MVX ships as\n"
        "// closed-source 32-bit Windows object code - see docs/spec/music-mvx.md).\n"
        "// No symbol from <dsound.h> is referenced by these declarations, so the\n"
        "// include is simply dropped rather than stubbed.\n"
        "// DirectSound output is not part of this port: MVX ships as closed-source\n"
        "// 32-bit Windows object code (docs/spec/music-mvx.md), and no symbol from\n"
        "// dsound.h is referenced by these declarations, so the include is dropped\n"
        "// rather than stubbed.\n"
        "\n"
        "#define MVX_SAMPLERATE",
        "removes the last Windows-only header from the shared declarations.",
    ),
    (
        "Phoenix/phxSpline.cpp",
        '#include "phxSpline.h"\n#ifdef PHX_MINIMAL_BUILD',
        '#include "phxSpline.h"\n'
        "// phxSpline.cpp uses min/max, memset and rand()/RAND_MAX with the Windows CRT\n"
        "// semantics that BaseLib re-declares (Random.h pins RAND_MAX to 0x7fff to\n"
        "// match MSVC).  The original build got all of this from its precompiled\n"
        "// header; here it has to be explicit.\n"
        '#include "../../Bedrock/BaseLib/BaseLib.h"\n#ifdef PHX_MINIMAL_BUILD',
        "clang needs explicit includes; also required for MSVC-equivalent rand().",
    ),
    (
        "Phoenix/phxSpline.cpp",
        """  if ( !KeyCount )
    return;

  if ( KeyCount == 1""",
        """  if ( !KeyCount )
    return;

  // clang rejects a goto that jumps over a variable initialisation, and
  // POSTPROCESS is `goto postproc`.  Hoist the two initialised locals above
  // the first jump (they are assigned further down and never read before).
  int pos;
  float partialt;

  if ( KeyCount == 1""",
        "POSTPROCESS jumps over `int pos = -1;` and `float partialt = ...;`.",
    ),
    (
        "Phoenix/phxSpline.cpp",
        "  int pos = -1;\n  while ( pos < KeyCount - 1",
        "  pos = -1;\n  while ( pos < KeyCount - 1",
        "declaration hoisted above the goto.",
    ),
    (
        "Phoenix/phxSpline.cpp",
        "  float partialt = t - NeededKeys[ 1 ]->GetTime() + 1;",
        "  partialt = t - NeededKeys[ 1 ]->GetTime() + 1;",
        "declaration hoisted above the goto.",
    ),
    (
        "Phoenix/qsort2.cpp",
        '//#include "libct.h"\n\nstatic void shortsort',
        '//#include "libct.h"\n#include <stddef.h>  // clang does not get size_t transitively here\n\nstatic void shortsort',
        "qsort2.cpp uses size_t with no header that declares it.",
    ),
    (
        "Bedrock/CoRE2/DX11Texture.cpp",
        """    LOG( LOG_ERROR, _T( "[core] CreateShaderResourceView failed (%s)" ), err.ErrorMessage() );
    return false;""",
        """    LOG( LOG_ERROR, _T( "[core] CreateShaderResourceView failed (%s)" ), err.ErrorMessage() );
    return NULL;  // MSVC converts `false` to a null pointer, clang does not""",
        "CCoreDX11Texture2D::Copy() returns CCoreTexture2D*; MSVC converts `false` "
        "to a null pointer. Four occurrences.",
        True,
    ),
    (
        "Phoenix_Tool/Audio/phxSound.h",
        """#include "phxSongBase.h"
#include "phxSongMVM.h"
#include "phxSongOGG.h"
#include "phxSongV2M.h"
#include "phxSongWaveSabre.h\"""",
        """#include "phxSongBase.h"
// The concrete synth hosts (MVX, Ogg Vorbis, V2, WaveSabre) are NOT part of
// this port: MVX ships as closed-source 32-bit Windows object code and the
// other backends are Windows/DirectSound only (docs/spec/music-mvx.md).
// Their translation units are excluded from the build, so only the abstract
// base is needed here - and dropping them also removes the last dependency on
// the Windows multimedia headers.
//#include "phxSongMVM.h"
//#include "phxSongOGG.h"
//#include "phxSongV2M.h"
//#include "phxSongWaveSabre.h\"""",
        "apExProject.h pulls this in; the synth backends are not built.",
    ),
    # --- Arbaro -----------------------------------------------------------------
    # clang will not bind a temporary to a non-const reference, and Arbaro
    # passes temporaries in several places.  The engine's own code is left
    # untouched apart from const-correctness.
    (
        "Phoenix/Arbaro.h",
        "    Transformation( D3DXMATRIX& m, D3DXVECTOR3& v );\n\n"
        "    D3DXVECTOR3 apply( D3DXVECTOR3& p );\n"
        "    D3DXVECTOR3 getZ();",
        "    Transformation( const D3DXMATRIX& m, const D3DXVECTOR3& v );\n\n"
        "    D3DXVECTOR3 apply( const D3DXVECTOR3& p ) const;\n"
        "    D3DXVECTOR3 getZ() const;",
        "Arbaro.cpp:189/244/335 pass temporaries and call these on const objects.",
    ),
    (
        "Phoenix/Arbaro.h",
        "    Transformation translate( D3DXVECTOR3& v );\n"
        "    void rotaxisz( float delta, float rho );\n"
        "    void rotaxis( float angle, D3DXVECTOR3& axis );",
        "    Transformation translate( const D3DXVECTOR3& v ) const;\n"
        "    void rotaxisz( float delta, float rho );\n"
        "    void rotaxis( float angle, const D3DXVECTOR3& axis );",
        "same.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  Transformation::Transformation( D3DXMATRIX& m, D3DXVECTOR3& v )",
        "  Transformation::Transformation( const D3DXMATRIX& m, const D3DXVECTOR3& v )",
        "match the const-ref declaration.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  D3DXVECTOR3 Transformation::apply( D3DXVECTOR3& p )",
        "  D3DXVECTOR3 Transformation::apply( const D3DXVECTOR3& p ) const",
        "match.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  D3DXVECTOR3 Transformation::getZ()\n",
        "  D3DXVECTOR3 Transformation::getZ() const\n",
        "match.  The trailing newline is part of the MATCH pattern: without it "
        "the replacement text still contains the pattern and every re-run "
        "appends another `const`.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  Transformation Transformation::translate( D3DXVECTOR3& v )",
        "  Transformation Transformation::translate( const D3DXVECTOR3& v ) const",
        "match.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  void Transformation::rotaxis( float angle, D3DXVECTOR3& axis )",
        "  void Transformation::rotaxis( float angle, const D3DXVECTOR3& axis )",
        "match.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "      D3DXVECTOR3 &vx = transf.apply(",
        "      D3DXVECTOR3 vx = transf.apply(",
        "binding a temporary to a local reference (MSVC extension).",
    ),
    (
        "Phoenix/Arbaro.h",
        "    SubsegmentImpl( D3DXVECTOR3& p, float r, float h, SegmentImpl* seg );",
        "    SubsegmentImpl( const D3DXVECTOR3& p, float r, float h, SegmentImpl* seg );",
        "Arbaro.cpp:284/299/315 pass the by-value result of apply().",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "  SubsegmentImpl::SubsegmentImpl( D3DXVECTOR3& p, float r, float h, SegmentImpl* seg )",
        "  SubsegmentImpl::SubsegmentImpl( const D3DXVECTOR3& p, float r, float h, SegmentImpl* seg )",
        "match.",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "      getSectionPoints( branchIdx, mesh, (float)subsegments[ x ]->rad, transf.translate( d ), cntr, last, l, uvscale );",
        "      // clang will not bind the temporary to getSectionPoints'\n"
        "      // Transformation& parameter, so name it.\n"
        "      Transformation sectionTrf = transf.translate( d );\n"
        "      getSectionPoints( branchIdx, mesh, (float)subsegments[ x ]->rad, sectionTrf, cntr, last, l, uvscale );",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix/Arbaro.cpp",
        "        Transformation& trf = substemDirection( segment->transf, ( segment->index + where )*stemdata.segmentLength ).translate( segment->transf.getZ()*(float)( where*stemdata.segmentLength ) );",
        "        // as above: this is a by-value result, so bind it to a value.\n"
        "        Transformation trf = substemDirection( segment->transf, ( segment->index + where )*stemdata.segmentLength ).translate( segment->transf.getZ()*(float)( where*stemdata.segmentLength ) );",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix/phxSpline.cpp",
        "WAVEFORMSPLINENOISEBUFFERSIZE / max( 1, WaveformFrequency )",
        "WAVEFORMSPLINENOISEBUFFERSIZE / max( 1.0f, (float)WaveformFrequency )",
        "clang: the ternary is ambiguous because D3DXFLOAT16 converts both ways "
        "(inherent to the D3DX type, not the port).",
    ),
    # --- Phoenix_Tool: decoupling the .apx loader from the tool UI -------------
    # The editor UI (Bedrock/Whiteboard + apEx/apEx/*) is deliberately not
    # vendored.  These entries drop its headers, its status bar and the editor
    # config globals, remove the two places that pushed data into editor
    # viewports, and fix the MSVC-only constructs the surrounding code used.
    # The CapexProject public API (Import/Reset/UpdateRTCollections and the
    # Timeline/Scenes/Models/... accessors) is unchanged.
    (
        "Phoenix_Tool/apxProject.cpp",
        "#include \"apxProject.h\"\n"
        "#include \"../apEx/apExRoot.h\"\n"
        "#include \"../Phoenix_Tool/Material_Tool.h\"\n"
        "#include \"../apEx/TimelinePreview.h\"\n"
        "#include \"../Phoenix/Material.h\"\n"
        "#include \"../Phoenix/Timeline.h\"\n"
        "#include \"../apEx/Config.h\"\n",
        "#include \"apxProject.h\"\n"
        "// [port] The tool-UI headers ../apEx/apExRoot.h, ../apEx/TimelinePreview.h\n"
        "// and ../apEx/Config.h are not vendored: they pull in Bedrock/Whiteboard\n"
        "// and the whole apEx editor (see docs/STATUS.md).  Nothing the .apx loader\n"
        "// needs lives in them.  The only symbols taken from them here were\n"
        "// `CapexRoot *Root` (mouse-button state + the status bar, both pure UI) and\n"
        "// the editor config globals - every use of those is dropped below.\n"
        "#include \"../Phoenix_Tool/Material_Tool.h\"\n"
        "#include \"../Phoenix/Material.h\"\n"
        "#include \"../Phoenix/Timeline.h\"\n",
        "the tool UI is deliberately not vendored; apxProject.cpp only used the editor root (input state / status bar) and the config globals from these headers, and neither affects the loaded data.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        "extern CapexRoot *Root;\n"
        "\n",
        "",
        "the editor root object is instantiated by the tool UI (apExRoot.cpp, not vendored).  Nothing in the loader reads it any more, so the declaration is dropped rather than left dangling.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        "  //delete[] sz8;\n"
        "\n"
        "  SetStatusbarText( CString::Format( _T( \"Project successfully saved to %s\" ), FileName.GetPointer() ) );\n"
        "}",
        "  //delete[] sz8;\n"
        "\n"
        "  // [port] SetStatusbarText() is declared in the unvendored\n"
        "  // ../apEx/apExRoot.h and only wrote into the tool's status bar.  The save\n"
        "  // itself (above) is unchanged; the message is dropped because a player has\n"
        "  // no status bar and nothing about the saved data depends on it.\n"
        "}",
        "SetStatusbarText is tool-UI-only; the file write it reported is untouched.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        "void CapexProject::DoAutosave( TS32 Seconds )\n"
        "{\n"
        "  if ( Root->GetApplication()->GetLeftButtonState() || Root->GetApplication()->GetRightButtonState() || Root->GetApplication()->GetMiddleButtonState() )\n"
        "  {\n"
        "    LastClickedTime = globalTimer.GetTime();\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  if ( Project.MusicData.Playing )",
        "void CapexProject::DoAutosave( TS32 Seconds )\n"
        "{\n"
        "  // [port] The original first asked CapexRoot::GetApplication() for a pressed\n"
        "  // mouse button so that interacting with the editor UI postponed the autosave.\n"
        "  // CapexRoot and the application window live in the unvendored editor UI and a\n"
        "  // player has no such input state, so that guard is dropped.  Everything else\n"
        "  // (the song-playing guard, both timers and the save) is unchanged.\n"
        "  if ( Project.MusicData.Playing )",
        "the mouse-button guard is editor-input state; it never affected the data.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        "  int timeLeft = max( (TS32)( 1000 * Seconds ) - ( (TS32)globalTimer.GetTime() - LastSaveTime ), (TS32)( 1000 * 3 ) - ( (TS32)globalTimer.GetTime() - LastClickedTime ) );\n"
        "  if ( timeLeft <= 5000 )\n"
        "    SetStatusbarText( CString::Format( _T( \"Autosaving in %d...\" ), (int)( timeLeft / 1000 ) + 1 ) );\n",
        "  int timeLeft = max( (TS32)( 1000 * Seconds ) - ( (TS32)globalTimer.GetTime() - LastSaveTime ), (TS32)( 1000 * 3 ) - ( (TS32)globalTimer.GetTime() - LastClickedTime ) );\n"
        "  // [port] `if ( timeLeft <= 5000 ) SetStatusbarText( \"Autosaving in N...\" )`\n"
        "  // was dropped: SetStatusbarText and the editor status bar are tool-UI-only.\n"
        "  // It only displayed the countdown, it never gated the save below.\n",
        "the autosave countdown is a status-bar message and nothing else.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        "  TS32 max = Config::MonitorAspectX;\n"
        "  TS32 may = Config::MonitorAspectY;\n"
        "\n"
        "  TS32 sax = GetSystemMetrics( SM_CXSCREEN );\n"
        "  TS32 say = GetSystemMetrics( SM_CYSCREEN );\n"
        "\n"
        "  TF32 aspect = ( tax*max*say ) / (TF32)( tay*may*sax );",
        "  // [port] Config::MonitorAspectX/Y came from the unvendored editor header\n"
        "  // ../apEx/Config.h: the resolution of the monitor the user said they were\n"
        "  // viewing on.  The editor wrote it from its own settings dialog; the release\n"
        "  // player asks the same question in its setup dialog (\"Monitor Aspect\"), so\n"
        "  // the port keeps the answer in src/platform/phx_monitor_aspect.* and reads it\n"
        "  // here at runtime instead of hard-coding the screen.  0,0 means \"auto\", i.e.\n"
        "  // the real screen - the value this used to be fixed to, and the one that\n"
        "  // makes the correction factor 1 and `aspect` the timeline's authored aspect.\n"
        "  TS32 sax = GetSystemMetrics( SM_CXSCREEN );\n"
        "  TS32 say = GetSystemMetrics( SM_CYSCREEN );\n"
        "  TS32 max = sax;\n"
        "  TS32 may = say;\n"
        "\n"
        "  extern void PhxMonitorAspect( TS32 *x, TS32 *y );\n"
        "  PhxMonitorAspect( &max, &may );\n"
        "\n"
        "  TF32 aspect = ( tax*max*say ) / (TF32)( tay*may*sax );",
        "Config::MonitorAspectX/Y is editor config; the release setup dialog asks the "
        "same question, so the resolution it selects now reaches the aspect-corrected "
        "render-target size through src/platform/phx_monitor_aspect.*.",
    ),
    (
        "Phoenix_Tool/Model_Tool.h",
        "class CphxModelObject_Tool_Mesh;\n"
        "class CphxTextureOperator_Tool;\n"
        "class CphxMaterial_Tool;\n"
        "class CphxMaterialTechnique_Tool;\n"
        "\n"
        "class CphxMeshFilter_Tool\n",
        "class CphxModelObject_Tool_Mesh;\n"
        "class CphxTextureOperator_Tool;\n"
        "class CphxMaterial_Tool;\n"
        "class CphxMaterialTechnique_Tool;\n"
        "// CphxModelObject_Tool holds a CphxModel_Tool*, but CphxModel_Tool is only\n"
        "// defined further down this header.  The MSVC build resolved it because\n"
        "// apxProject.h happened to be included first; clang needs the declaration.\n"
        "class CphxModel_Tool;\n"
        "\n"
        "class CphxMeshFilter_Tool\n",
        "clang needs the forward declaration (MSVC permissive include order); without it every use of CphxModelObject_Tool loses its CphxResource base and Model_Tool.cpp reports a cascade of bogus conversion errors.",
    ),
    (
        "Phoenix_Tool/Model_Tool.h",
        "void SetName( CString &name );",
        "void SetName( const CString &name );",
        "clang will not bind the temporaries the call sites pass (MSVC extension): Model_Tool.cpp:326/423 pass a CString by value.",
        True,
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "void CphxModel_Tool::SetName( CString &name )",
        "void CphxModel_Tool::SetName( const CString &name )",
        "match the const-ref declaration.",
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "void CphxModelObject_Tool::SetName( CString &name )",
        "void CphxModelObject_Tool::SetName( const CString &name )",
        "match the const-ref declaration.",
    ),
    (
        "Phoenix_Tool/Scene_tool.h",
        "void SetName( CString &name ) { Name = name; }",
        "void SetName( const CString &name ) { Name = name; }",
        "clang will not bind the temporaries the call sites pass (MSVC extension): Scene_tool.cpp:316-381 build CString(\"New Camera\") etc. and :332/:612/:1070 pass the by-value result of GetName().",
        True,
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "CXMLNode &n = ",
        "CXMLNode n = ",
        "CXMLNode::GetChild returns CXMLNode by value; binding the temporary to a non-const reference is an MSVC extension (5 occurrences).  A value copy is what the original code already had - the reference never aliased anything.",
        True,
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "CXMLNode &nn = n.GetChild( name.GetPointer() );",
        "CXMLNode nn = n.GetChild( name.GetPointer() );",
        "same.",
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "  CMatrix4x4 mx( &m._11 );\n"
        "  ApplyTransformation( CPRS( mx ) );",
        "  CMatrix4x4 mx( &m._11 );\n"
        "  // clang will not bind the temporary to ApplyTransformation's CPRS&\n"
        "  // parameter (MSVC extension); ApplyTransformation only reads it.\n"
        "  CPRS prs( mx );\n"
        "  ApplyTransformation( prs );",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "#include \"apxProject.h\"\n"
        "#include \"../apEx/apExRoot.h\"\n",
        "#include \"apxProject.h\"\n"
        "// [port] ../apEx/apExRoot.h (the editor UI root) is not vendored.  Nothing in\n"
        "// Timeline_tool.cpp referenced anything from it - only the editor's event\n"
        "// panels did - so the include is simply dropped.\n",
        "the tool UI is deliberately not vendored and this file used nothing from it.",
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "void SetStatusbarText( CString Text );\n"
        "void SetStatusbarError( CString Text );\n"
        "\n"
        "\n",
        "// [port] SetStatusbarText/SetStatusbarError were defined by the unvendored\n"
        "// editor UI (../apEx/apExRoot.cpp) and wrote into the tool's status bar.\n"
        "// Both declarations and their call sites below are deleted; every call only\n"
        "// displayed a message and the two failure paths already LOG_ERR the same\n"
        "// information.\n"
        "\n",
        "the status bar is tool-UI-only.",
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "    SetStatusbarText( \"Not all objects could be deleted due to dependencies\" );\n"
        "    LOG_ERR( \"Not all objects could be deleted due to dependencies\" );\n",
        "    // [port] the SetStatusbarText() twin of this LOG_ERR was tool-UI-only.\n"
        "    LOG_ERR( \"Not all objects could be deleted due to dependencies\" );\n",
        "the same message is already logged on the line below.",
    ),
    (
        "Phoenix_Tool/Model_Tool.cpp",
        "    LOG_ERR( \"Memory exception thrown!\" );\n"
        "    SetStatusbarError( CString::Format( _T( \"%s FAILED TO GENERATE DUE TO OUT OF MEMORY ERROR!\" ), GetName().GetPointer() ) );\n",
        "    LOG_ERR( \"Memory exception thrown!\" );\n"
        "    // [port] the SetStatusbarError( \"<name> FAILED TO GENERATE DUE TO OUT OF\n"
        "    // MEMORY ERROR!\" ) call was tool-UI-only; the LOG_ERR above still\n"
        "    // reports the failure.\n",
        "the status bar is tool-UI-only; the failure stays logged.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "void SetStatusbarText( CString Text );\n",
        "// [port] SetStatusbarText was defined by the unvendored editor UI\n"
        "// (../apEx/apExRoot.cpp) and wrote into the tool's status bar.  Its single\n"
        "// call site below is deleted; it only displayed a message.\n",
        "the status bar is tool-UI-only.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "      if ( parent && parent->FindParent( Objects[ x ] ) )\n"
        "      {\n"
        "        SetStatusbarText( _T( \"Some of the objects were not linked to avoid a circular hierarchy.\" ) );\n"
        "        continue;\n"
        "      }\n",
        "      if ( parent && parent->FindParent( Objects[ x ] ) )\n"
        "      {\n"
        "        // [port] the SetStatusbarText( \"Some of the objects were not linked to\n"
        "        // avoid a circular hierarchy.\" ) warning was tool-UI-only; the `continue`\n"
        "        // - the actual behaviour - is unchanged.\n"
        "        continue;\n"
        "      }\n",
        "the status bar is tool-UI-only; the control flow is unchanged.",
    ),
    (
        "Phoenix_Tool/TexgenPage.cpp",
        "void SetStatusbarText( CString Text );\n"
        "\n",
        "// [port] SetStatusbarText was defined by the unvendored editor UI\n"
        "// (../apEx/apExRoot.cpp) and wrote into the tool's status bar.  Its call site\n"
        "// in DeleteSelected() below is deleted; it only displayed a warning, the\n"
        "// `Cantdelete` flag it accompanied is untouched.\n"
        "\n",
        "the status bar is tool-UI-only.",
    ),
    (
        "Phoenix_Tool/TexgenPage.cpp",
        "            SetStatusbarText( _T( \"WARNING: Not all operators were deleted due to dependency issues.\" ) );\n"
        "            Cantdelete = true;\n",
        "            // [port] SetStatusbarText( \"WARNING: Not all operators were deleted\n"
        "            // due to dependency issues.\" ) was tool-UI-only; `Cantdelete`, which\n"
        "            // drives the behaviour, is unchanged.\n"
        "            Cantdelete = true;\n",
        "the status bar is tool-UI-only.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "#include \"../apEx/apexRoot.h\"\n"
        "#include \"../apEx/ModelView.h\"\n"
        "#include \"../apEx/SceneView.h\"\n",
        "// [port] ../apEx/apexRoot.h, ../apEx/ModelView.h and ../apEx/SceneView.h are\n"
        "// the editor's root object and its model/scene viewport windows.  They are not\n"
        "// vendored; the only thing this file took from them is the Root workbench walk\n"
        "// in ApplyCameraDataForFirstCam() below, which pushed camera data into the open\n"
        "// editor viewports.  That block is deleted there.\n",
        "the editor viewport windows are tool-UI-only.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "#include \"../apEx/WorkBench.h\"\n"
        "\n"
        "TBOOL CphxObject_SubScene_Tool::Pick( CRect ClientRect, CPoint MousePos, D3DXMATRIX ProjectionMatrix, D3DXMATRIX CameraMatrix, D3DXMATRIX TransformationMatrix, float &t )\n"
        "{\n"
        "  auto* editedScene = GetActiveWorkBench()->GetEditedScene();\n"
        "  if ( !editedScene )\n"
        "    return false;\n"
        "\n"
        "  TS32 clipID = editedScene->GetActiveClip();\n"
        "\n"
        "  auto* Scene = GetClip( clipID )->GetSubSceneTarget();\n"
        "\n"
        "  if ( !Scene ) return false;\n"
        "\n"
        "  TBOOL Hit = false;\n"
        "\n"
        "  D3DXMATRIX mx;\n"
        "  D3DXMatrixMultiply( &mx, &TransformationMatrix, &GetMatrix() );\n"
        "\n"
        "  for ( TS32 x = 0; x < Scene->GetObjectCount(); x++ )\n"
        "    Hit |= Scene->GetObjectByIndex( x )->Pick( ClientRect, MousePos, ProjectionMatrix, CameraMatrix, mx, t );\n"
        "\n"
        "  return Hit;\n"
        "}\n",
        "// [port] ../apEx/WorkBench.h is the editor's workbench/tab UI and is not\n"
        "// vendored.  CphxObject_SubScene_Tool::Pick() used the *active editor\n"
        "// workbench's* edited scene and its active clip to decide which sub-scene to\n"
        "// hit-test.  That is editor interaction state: a player never calls Pick(), and\n"
        "// with no editor there is no \"active clip\" to substitute - guessing one (e.g.\n"
        "// clip 0) would report hits against a clip the user never selected.  The\n"
        "// override therefore reports \"no hit\", which is exactly what the original\n"
        "// returned when there was no edited scene or no sub-scene target.\n"
        "// LOUD: this removes editor picking of sub-scene objects; it is on no\n"
        "// load/render path.\n"
        "TBOOL CphxObject_SubScene_Tool::Pick( CRect ClientRect, CPoint MousePos, D3DXMATRIX ProjectionMatrix, D3DXMATRIX CameraMatrix, D3DXMATRIX TransformationMatrix, float &t )\n"
        "{\n"
        "  return false;\n"
        "}\n",
        "picking against the active editor workbench's clip is editor interaction state; the override keeps its signature and returns \"no hit\".",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "        for ( TS32 z = 0; z < n.GetChildCount( _T( \"Spline\" ) ); z++ )\n"
        "          MaterialSplines.Splines[ y ]->Splines[ z ].ImportData( n.GetChild( _T( \"Spline\" ), z ) );\n",
        "        for ( TS32 z = 0; z < n.GetChildCount( _T( \"Spline\" ) ); z++ )\n"
        "        {\n"
        "          // clang will not bind CXMLNode::GetChild()'s by-value result to\n"
        "          // ImportData's CXMLNode& parameter (MSVC extension).\n"
        "          CXMLNode splineNode = n.GetChild( _T( \"Spline\" ), z );\n"
        "          MaterialSplines.Splines[ y ]->Splines[ z ].ImportData( splineNode );\n"
        "        }\n",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "  if ( GetObjectA()->camCenterX != 0 )\n"
        "    Node->AddChild( _T( \"camcenterx\" ) ).SetInt( GetObjectA()->camCenterX );\n"
        "  if ( GetObjectA()->camCenterY != 0 )\n"
        "    Node->AddChild( _T( \"camcentery\" ) ).SetInt( GetObjectA()->camCenterY );\n",
        "  // [port] These four lines spelled the accessor GetObjectA() (an upstream\n"
        "  // typo).  It only worked because the Windows SDK maps GetObject -> GetObjectA\n"
        "  // with a macro, which renamed the member declaration in Scene_tool.h too;\n"
        "  // compat/wingdi.h deliberately does not define that macro (it would also\n"
        "  // rewrite the engine's own GetObject() members), so the real member name is\n"
        "  // used here.  This is the same member the rest of the file calls.\n"
        "  if ( GetObject()->camCenterX != 0 )\n"
        "    Node->AddChild( _T( \"camcenterx\" ) ).SetInt( GetObject()->camCenterX );\n"
        "  if ( GetObject()->camCenterY != 0 )\n"
        "    Node->AddChild( _T( \"camcentery\" ) ).SetInt( GetObject()->camCenterY );\n",
        "GetObjectA() is an upstream typo that only compiled because the Windows SDK renames GetObject to GetObjectA with a macro; compat/ drops that macro, so the member is called by its real name.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "void CphxScene_Tool::ApplyCameraDataForFirstCam()\n"
        "{\n"
        "  bool updated = false;\n"
        "  for ( int x = 0; x < Objects.NumItems(); x++ )\n"
        "  {\n"
        "    if ( Objects[ x ]->GetObjectType() == Object_CamEye )\n"
        "    {      \n"
        "      if ( !updated )\n"
        "      {\n"
        "        UpdateSceneGraph( 0, 0 );\n"
        "        updated = true;\n"
        "      }\n"
        "      auto mat = Objects[ x ]->GetMatrix();\n"
        "      D3DXVECTOR4 pos = D3DXVECTOR4( 0, 0, 0, 1 );\n"
        "      D3DXVec4Transform( &pos, &pos, &mat );\n"
        "      D3DXVECTOR4 trg = pos + D3DXVECTOR4( 0, 0, 1, 0 );\n"
        "      if ( Objects[ x ]->TargetObject )\n"
        "      {\n"
        "        mat = Objects[ x ]->TargetObject->GetMatrix();\n"
        "        trg = D3DXVECTOR4( 0, 0, 0, 1 );\n"
        "        D3DXVec4Transform( &trg, &trg, &mat );\n"
        "      }\n"
        "\n"
        "      CAMERADATA c;\n"
        "      c.OrthoY = c.Up = D3DXVECTOR3( 0, 1, 0 );\n"
        "\n"
        "      c.Up = D3DXVECTOR3( 0, 1, 0 );\n"
        "\n"
        "      c.Eye = D3DXVECTOR3( pos );\n"
        "      c.Up = D3DXVECTOR3( 0, 1, 0 );\n"
        "      c.Target = D3DXVECTOR3( trg );\n"
        "\n"
        "      extern CapexRoot *Root;\n"
        "\n"
        "      if (Root)\n"
        "      for ( int y = 0; y < Root->GetWorkBenchCount(); y++ )\n"
        "      {\n"
        "        auto* wb = Root->GetWorkBench( y );\n"
        "        int cnt = wb->GetWindowCount( apEx_SceneView );\n"
        "\n"
        "        for ( int z = 0; z < cnt; z++ )\n"
        "        {\n"
        "          auto *win = (CapexSceneView*)wb->GetWindow( apEx_SceneView, z );\n"
        "          CWBSceneDisplay *t = (CWBSceneDisplay*)win->FindChildByID( _T( \"display\" ), _T( \"scenedisplay\" ) );\n"
        "          if ( t->GetMode() == CAMERA_NORMAL )\n"
        "          {\n"
        "            t->SetCameraData( GetGUID(), c );\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n",
        "void CphxScene_Tool::ApplyCameraDataForFirstCam()\n"
        "{\n"
        "  // [port] The original built a CAMERADATA from the first camera of the scene\n"
        "  // and pushed it into every open editor scene viewport, so that the editor's\n"
        "  // scene views followed the first camera.  CAMERADATA (../apEx/ModelView.h),\n"
        "  // CapexRoot (apExRoot.h) and CapexSceneView / CWBSceneDisplay (SceneView.h,\n"
        "  // WorkBench.h) are all tool-UI types and none of those headers is vendored.\n"
        "  // The viewport push is a pure UI refresh; the one part of this function that\n"
        "  // is not UI - bringing the scene graph up to date before the camera transform\n"
        "  // is read, via the real engine call UpdateSceneGraph() - is kept.\n"
        "  // LOUD: the editor viewports no longer follow the first camera; nothing on\n"
        "  // the load/render path changes, because the demo's own camera is applied by\n"
        "  // the engine when the scene is rendered.\n"
        "  for ( int x = 0; x < Objects.NumItems(); x++ )\n"
        "  {\n"
        "    if ( Objects[ x ]->GetObjectType() == Object_CamEye )\n"
        "    {\n"
        "      // Called for the first camera only, as before (`updated` flag).\n"
        "      UpdateSceneGraph( 0, 0 );\n"
        "      return;\n"
        "    }\n"
        "  }\n"
        "}\n",
        "CAMERADATA and the editor viewport push are tool-UI-only; the real engine call (UpdateSceneGraph) and the `first camera only` logic are preserved.",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "    Time->ImportData( Node->GetChild( _T( \"TimeSpline\" ) ) );\n",
        "    // clang will not bind CXMLNode::GetChild()'s by-value result to\n"
        "    // ImportData's CXMLNode& parameter (MSVC extension).\n"
        "    CXMLNode timeSplineNode = Node->GetChild( _T( \"TimeSpline\" ) );\n"
        "    Time->ImportData( timeSplineNode );\n",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "          for ( TS32 z = 0; z < n.GetChildCount( _T( \"Spline\" ) ); z++ )\n"
        "            ShaderSplines.Splines[ y ]->Splines[ z ].ImportData( n.GetChild( _T( \"Spline\" ), z ) );\n",
        "          for ( TS32 z = 0; z < n.GetChildCount( _T( \"Spline\" ) ); z++ )\n"
        "          {\n"
        "            // as above: name the by-value CXMLNode before passing it on.\n"
        "            CXMLNode splineNode = n.GetChild( _T( \"Spline\" ), z );\n"
        "            ShaderSplines.Splines[ y ]->Splines[ z ].ImportData( splineNode );\n"
        "          }\n",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "    EyeIntensity->ImportData( Node->GetChild( _T( \"EyeIntensitySpline\" ) ) );\n",
        "    // as above: name the by-value CXMLNode before passing it on.\n"
        "    CXMLNode eyeNode = Node->GetChild( _T( \"EyeIntensitySpline\" ) );\n"
        "    EyeIntensity->ImportData( eyeNode );\n",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "    TargetIntensity->ImportData( Node->GetChild( _T( \"TargetIntensitySpline\" ) ) );\n",
        "    // as above: name the by-value CXMLNode before passing it on.\n"
        "    CXMLNode targetNode = Node->GetChild( _T( \"TargetIntensitySpline\" ) );\n"
        "    TargetIntensity->ImportData( targetNode );\n",
        "non-const reference to a temporary (MSVC extension).",
    ),
    (
        "Phoenix_Tool/Timeline_tool.cpp",
        "  SetTech( Tech );\n"
        "  UpdateMaterialState();\n"
        "\n"
        "  UpdateWindowData( apEx_TimelineEventSplines );\n",
        "  SetTech( Tech );\n"
        "  UpdateMaterialState();\n"
        "\n"
        "  // [port] UpdateWindowData( apEx_TimelineEventSplines ) told the editor's\n"
        "  // \"TimeLine Event Splines\" window to refresh; both the window and the\n"
        "  // enumeration (../apEx/apExWindow.h) are tool-UI-only.  GenerateResource()\n"
        "  // still builds the shader/technique state above - that is the part the\n"
        "  // renderer needs.\n",
        "the timeline event-splines window refresh is tool-UI-only.",
    ),

    (
        "Bedrock/CoRE2/DX11Device.h",
        "	INLINE ID3D11Device *GetDevice() { return Device; }\n	INLINE ID3D11DeviceContext *GetDeviceContext()",
        "	INLINE ID3D11Device *GetDevice() { return Device; }\n"
        "	// The engine keeps its own global `SwapChain` (Phoenix/phxEngine.cpp:84)\n"
        "	// which MinimalPlayer and Phoenix/Project.cpp's DoPrecalc() present\n"
        "	// through, but nothing ever assigned it: CCoreDX11Device has always owned\n"
        "	// a private member with the same name.  A player that creates the device\n"
        "	// through this class needs the accessor to publish it.\n"
        "	INLINE IDXGISwapChain *GetSwapChain() { return SwapChain; }\n	INLINE ID3D11DeviceContext *GetDeviceContext()",
        "the engine global `SwapChain` is otherwise never assigned, and dereferencing\n"
        "it (Project.cpp:182 DoPrecalc, MinimalPlayer's main loop) would crash.",
    ),
    # --- audio declarations we still compile against -----------------------------
    # Every Audio/*.cpp is excluded (MVX is closed-source Windows object code),
    # but apxProject.h includes Audio/phxSound.h, so these headers must parse.
    (
        "Phoenix_Tool/Audio/phxSongBase.h",
        '#include "..\\..\\..\\Bedrock\\BaseLib\\BaseLib.h"',
        '#include "../../../Bedrock/BaseLib/BaseLib.h"',
        "backslash include separators are a hard error outside MSVC.",
    ),
    # --- remaining Phoenix portability fixes ------------------------------------
    (
        "Phoenix/Mesh.cpp",
        "  CSGPoly( CphxArray<CphxVertex> *vxData, int a, int b, int c, CphxUV& uva, CphxUV& uvb, CphxUV& uvc )",
        "  // const refs: the call sites at Mesh.cpp:2470/2480 pass CphxUV temporaries.\n"
        "  CSGPoly( CphxArray<CphxVertex> *vxData, int a, int b, int c, const CphxUV& uva, const CphxUV& uvb, const CphxUV& uvc )",
        "clang will not bind a temporary to a non-const reference.",
    ),
    (
        "Phoenix/Texgen.cpp",
        "  long hmWidth, hmHeight;",
        "  // OLE_XSIZE_HIMETRIC is LONG (32-bit); `long` is 64-bit on LP64, so the\n"
        "  // IPicture::get_Width/get_Height out-parameters would not match.\n"
        "  OLE_XSIZE_HIMETRIC hmWidth;\n"
        "  OLE_YSIZE_HIMETRIC hmHeight;",
        "the OLE picture path only compiled because long == LONG on Windows.",
    ),
    # --- 64-bit pointer truncation in the emitter's type tag ---------------------
    # The original tagged an emitted-model/emitted-scene pointer by stealing its
    # lowest bit *through a TU32 cast* (the author even wrote "OUCH").  That is
    # fine on Win32 and destroys the pointer on LP64: the decoder produced
    # 0x00000000acd16e60 and CphxResource::AddParent dereferenced it.
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "        Data = (void*)( (TU32)( Data ) | 0x01 ); // OUCH",
        "        // Tag bit 0.  Must go through uintptr_t: the original TU32 cast\n"
        "        // threw away the upper half of the pointer on 64-bit.\n"
        "        Data = (void*)( (uintptr_t)( Data ) | 0x01 ); // OUCH",
        "64-bit pointer truncation.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        "      bool dataIsScene = (TU32)Data & 0x01;\n\n      Data = (void*)( ( (TU32)Data ) & 0xfffffffc );",
        "      bool dataIsScene = ( (uintptr_t)Data & 0x01 ) != 0;\n\n      Data = (void*)( (uintptr_t)Data & ~(uintptr_t)0x3 );",
        "matching decode.",
    ),
    # --- 64-bit: `ExtraDataSize` is a POINTER, not a size -----------------------
    # PHXTEXTUREFILTER::Render/GetLookupTexture take one extra slot that is
    # overloaded by LookupType: a byte count for LookupType 1, but the *text
    # pointer* for LookupType 2 (see the `// if (!t->Text)` comment at
    # Texgen.cpp:247, and Texgen.h:117's own note "used for data size and as the
    # text pointer").  It was declared `int`, so on LP64 the pointer arrived
    # truncated and Texgen.cpp:276's `while (((char*)ExtraDataSize)[len])` walked
    # a wild address.  Widening the slot is the whole fix; the casts stay as the
    # original wrote them.
    (
        "Phoenix/Texgen.h",
        "  void Render( CphxTexturePoolTexture *&Target, CphxTexturePoolTexture *&SwapBuffer, CphxTexturePoolTexture *Inputs[ TEXGEN_MAX_PARENTS ], unsigned char RandSeed, unsigned char Parameters[ TEXGEN_MAX_PARAMS ], void *ExtraData, int ExtraDataSize );",
        "  void Render( CphxTexturePoolTexture *&Target, CphxTexturePoolTexture *&SwapBuffer, CphxTexturePoolTexture *Inputs[ TEXGEN_MAX_PARENTS ], unsigned char RandSeed, unsigned char Parameters[ TEXGEN_MAX_PARAMS ], void *ExtraData, intptr_t ExtraDataSize );",
        "the slot carries a text pointer for LookupType 2; `int` truncated it on LP64.",
    ),
    (
        "Phoenix/Texgen.h",
        "  virtual CphxTexturePoolTexture *GetLookupTexture( unsigned char Res, void *ExtraData, int ExtraDataSize );",
        "  virtual CphxTexturePoolTexture *GetLookupTexture( unsigned char Res, void *ExtraData, intptr_t ExtraDataSize );",
        "same slot.",
    ),
    (
        "Phoenix/Texgen.h",
        "  int minimportData2; //used for data size and as the text pointer",
        "  // Widened from `int`: see the note above - this holds a text POINTER for\n"
        "  // text operators, and an `int` truncated it on LP64.\n"
        "  intptr_t minimportData2; //used for data size and as the text pointer",
        "same.",
    ),
    (
        "Phoenix/Texgen.cpp",
        "void PHXTEXTUREFILTER::Render( CphxTexturePoolTexture *&Target, CphxTexturePoolTexture *&SwapBuffer, CphxTexturePoolTexture *Inputs[ TEXGEN_MAX_PARENTS ], unsigned char RandSeed, unsigned char Parameters[ TEXGEN_MAX_PARAMS ], void *ExtraData, int ExtraDataSize )",
        "void PHXTEXTUREFILTER::Render( CphxTexturePoolTexture *&Target, CphxTexturePoolTexture *&SwapBuffer, CphxTexturePoolTexture *Inputs[ TEXGEN_MAX_PARENTS ], unsigned char RandSeed, unsigned char Parameters[ TEXGEN_MAX_PARAMS ], void *ExtraData, intptr_t ExtraDataSize )",
        "match the declaration.",
    ),
    (
        "Phoenix/Texgen.cpp",
        "CphxTexturePoolTexture * PHXTEXTUREFILTER::GetLookupTexture( unsigned char Res, void *ExtraData, int ExtraDataSize )",
        "CphxTexturePoolTexture * PHXTEXTUREFILTER::GetLookupTexture( unsigned char Res, void *ExtraData, intptr_t ExtraDataSize )",
        "match the declaration.",
    ),
    (
        "Phoenix/Project.cpp",
        "        o.minimportData2 = (int)skiptext( Stream_ASCIIZ );",
        "        // intptr_t, not int: this slot carries the text pointer for text operators.\n"
        "        o.minimportData2 = (intptr_t)skiptext( Stream_ASCIIZ );",
        "64-bit pointer truncation.",
    ),
    (
        "Phoenix_Tool/Texgen_tool.cpp",
        "    Filter->Filter.Render( Target, SwapBuffer, Inputs, OpData.RandSeed, OpData.Parameters, &TextData, (int)Text );",
        "    // intptr_t, not int: `Text` is a pointer and the slot is a pointer for\n"
        "    // LookupType 2 (this is the call site that crashed on Clean Slate).\n"
        "    Filter->Filter.Render( Target, SwapBuffer, Inputs, OpData.RandSeed, OpData.Parameters, &TextData, (intptr_t)Text );",
        "64-bit pointer truncation.",
    ),
    # --- generator queue: missing `return` caused an infinite loop --------------
    # StepGeneratorQueue() deletes a resource that can never be generated
    # (CanBeGenerated() false) but does not return, so control falls into the
    # ParentContentReady() branch, which calls f->RequestContent() and puts the
    # very same resource straight back on the queue.  The player then spins
    # forever and never reaches its first frame.  Report it once per resource so
    # the reason is visible instead of silent.
    (
        "Phoenix_Tool/phxResource.cpp",
        """  if ( !f->CanBeGenerated() )
  {
    //the conditions have been changed while the item was waiting in queue and this item can't be generated anymore
    UpdateQueue.Delete( f );
  }
""",
        """  if ( !f->CanBeGenerated() )
  {
    //the conditions have been changed while the item was waiting in queue and this item can't be generated anymore
    // [port] Report it: without the `return` below this resource is immediately
    // re-queued by the ParentContentReady() branch and the generator never
    // drains.  The resource is dropped instead, which is what deleting it from
    // the queue means.
    LOG_ERR( "[resource] cannot be generated (type %d); dropping it from the queue", (TS32)f->GetType() );
    UpdateQueue.Delete( f );
    return;
  }
""",
        "upstream missing `return` turned a permanently-ungeneratable resource into an\n"
        "infinite generation loop.",
    ),
    # --- generator queue: diagnose the re-queue ping-pong -----------------------
    # A resource whose parent never becomes ContentReady() is deleted from the
    # queue and immediately re-requested, forever.  Log the first occurrences so
    # the culprit is identifiable instead of the player silently spinning.
    (
        "Phoenix_Tool/phxResource.cpp",
        """  if ( !f->ParentContentReady() )
  {
    //some parent got invalidated while this item waited to be processed, move to the end of the queue
    UpdateQueue.Delete( f );
    f->RequestContent();
    return;
  }
""",
        """  if ( !f->ParentContentReady() )
  {
    //some parent got invalidated while this item waited to be processed, move to the end of the queue
    // [port] diagnostic: this branch can loop forever if a parent never becomes
    // ready, so say which child and which parent are stuck (first 40 only).
    {
      static TS32 reported = 0;
      if ( reported < 40 )
      {
        reported++;
        LOG_ERR( "[resource] re-queueing type %d because a parent is not ready", (TS32)f->GetType() );
        for ( TS32 pc = 0; pc < f->GetParentCount(); pc++ )
        {
          CphxResource *par = f->GetParent( pc );
          if ( par && !par->ContentReady() )
            LOG_ERR( "[resource]    parent type %d not ready (uptodate %d allocated %d)",
                     (TS32)par->GetType(), (TS32)par->ContentReady(), (TS32)par->IsAllocated() );
        }
      }
    }
    UpdateQueue.Delete( f );
    f->RequestContent();
    return;
  }
""",
        "diagnostic for an otherwise silent infinite generator loop.",
    ),
    (
        "Phoenix_Tool/apxProject.cpp",
        """    CphxEvent_Tool *e = Timeline->CreateEvent( EventType, 0, 0, 0, NULL );
    e->Import( &n );""",
        """    CphxEvent_Tool *e = Timeline->CreateEvent( EventType, 0, 0, 0, NULL );
    // [port] CreateEvent returns NULL for a type it does not know (anything
    // outside PHXEVENTTYPE), and this imported straight through the null
    // pointer: an <event><Type>7</Type></event> in a project file killed the
    // player with SIGSEGV while importing.  An event type this build cannot
    // render is a reason to skip that event, not to die.
    if ( !e )
    {
      LOG_ERR( "[import] event %d has type %d, which this build does not know; skipping it",
               x, (TS32)EventType );
      continue;
    }
    e->Import( &n );""",
        "a hostile .apx with an out of range <event><Type> dereferenced NULL.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        """    CphxObject_Tool *t = AddObject( type, Data );
    t->Import( &n );""",
        """    CphxObject_Tool *t = AddObject( type, Data );
    // [port] AddObject returns NULL for a primitive id it does not know, and
    // for an Object_Model whose <model> GUID resolved to nothing (it bails on
    // a null Data).  This imported straight through the null pointer, so
    // <Object Type="99"> or a stale model GUID killed the player during import.
    if ( !t )
    {
      LOG_ERR( "[import] scene object %d has type %d and could not be created; skipping it",
               x, (TS32)type );
      continue;
    }
    t->Import( &n );""",
        "a hostile .apx with an unknown scene object type or model GUID "
        "dereferenced NULL.",
    ),
    (
        "Phoenix_Tool/Scene_tool.cpp",
        """    PHXSPLINETYPE type = Spline_Position_x;
    n.GetAttributeAsInteger( _T( "type" ), (TS32*)&type );

    CphxClipSpline_Tool *s = new CphxClipSpline_Tool( type );""",
        """    PHXSPLINETYPE type = Spline_Position_x;
    n.GetAttributeAsInteger( _T( "type" ), (TS32*)&type );

    // [port] The type attribute is a PHXSPLINETYPE used as an index into
    // CphxObject::SplineResults[Spline_Count].  It was read with no range check
    // at all, so <clipspline type="66"> wrote past the end of every object that
    // used the clip and killed the player with SIGBUS on every frame.  Reject
    // it here, where there is something to log with, and keep the check on the
    // read side in Scene.cpp as well.
    if ( (TS32)type < 0 || (TS32)type >= (TS32)Spline_Count )
    {
      LOG_ERR( "[import] clip spline type %d is outside 0..%d; skipping it",
               (TS32)type, (TS32)Spline_Count - 1 );
      continue;
    }

    CphxClipSpline_Tool *s = new CphxClipSpline_Tool( type );""",
        "a hostile .apx with an out of range <clipspline type> indexed "
        "SplineResults[] out of bounds.",
    ),
    (
        "Phoenix/Scene.cpp",
        """    CphxClipSpline *s = Clips[ Clip ]->Splines[ x ];
    s->Spline->CalculateValue( t );
    SplineResults[ s->Type ] = s->Spline->Value[ 0 ];""",
        """    CphxClipSpline *s = Clips[ Clip ]->Splines[ x ];
    s->Spline->CalculateValue( t );
    // [port] Belt and braces for the check Scene_tool.cpp does at import: this
    // writes SplineResults[] with an index that came out of the file, and a
    // wrong one corrupts the object rather than failing.  No logging macro is
    // reachable from this translation unit, so it is silent.
    if ( (int)s->Type < 0 || (int)s->Type >= Spline_Count ) continue;
    SplineResults[ s->Type ] = s->Spline->Value[ 0 ];""",
        "guard the SplineResults[] index on the read side too.",
    ),
    (
        "Phoenix/Timeline.cpp",
        """    //render here
    phxContext->Draw( 6, 0 );""",
        """    // [port] diagnostic: a shader-toy pass with nowhere to draw, or with no
    // pixel shader, renders nothing and says nothing about it.  The loading
    // screen is exactly this path, and it came out blank without a single log
    // line pointing at it, so report the first few passes' state -- including
    // the four floats that become cbuffer b0, whose XRes/YRes entries are only
    // written when Target is non-null.
    {
      static int reportedShadertoy = 0;
      if ( reportedShadertoy < 8 )
      {
        reportedShadertoy++;
        // No logging macro reaches this translation unit, so write it directly.
        printf( "[shadertoy] pass %d of %d: rendertarget %p, ps %p, target %p, b0 = %.4f %.4f %.4f %.4f\\n",
                (int)x, (int)Tech->PassCount, (void*)MaterialState[ x ]->RenderTarget,
                (void*)Tech->RenderPasses[ x ]->PS, (void*)Target,
                ShaderData[ 0 ], ShaderData[ 1 ], ShaderData[ 2 ], ShaderData[ 3 ] );
        fflush( stdout );
      }
    }

    //render here (after the [shadertoy] diagnostic above)
    phxContext->Draw( 6, 0 );""",
        "a shader-toy pass with no render target renders nothing silently; this is "
        "the loading-screen path.",
    ),
    (
        "Phoenix/Timeline.cpp",
        """    //display frame
    Prepare2dRender();""",
        """    // [port] diagnostic: the shader-toy pass above draws into
    // MaterialState[x]'s render target, and this blit reads the EVENT's Target.
    // When those are two different objects the pass renders into an image
    // nobody looks at and the screen stays blank with nothing in the log to say
    // so.
    {
      static int reportedBlit = 0;
      if ( reportedBlit < 8 )
      {
        reportedBlit++;
        printf( "[blit] event target %p view %p %dx%d\\n", (void*)Target, (void*)Target->View,
                (int)Target->XRes, (int)Target->YRes );
        fflush( stdout );
      }
    }

    //display frame (after the [blit] diagnostic above)
    Prepare2dRender();""",
        "a blit that reads a different render target than the pass drew into leaves "
        "a blank screen and says nothing.",
    ),
]


def apply(rel, old, new, note, replace_all=False):
    path = os.path.join(TP, rel)
    if not os.path.exists(path):
        return "missing", path
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if old not in src:
        if new == "" or new in src:
            return "already", path
        return "NOTFOUND", path
    src = src.replace(old, new, -1 if replace_all else 1)
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(src)
    return "patched", path


def main():
    bad = 0
    for entry in PATCHES:
        rel, old, new, note = entry[0], entry[1], entry[2], entry[3]
        replace_all = len(entry) > 4 and entry[4]
        status, path = apply(rel, old, new, note, replace_all)
        print("%-9s %s" % (status, os.path.relpath(path, ROOT)))
        if status == "NOTFOUND":
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
