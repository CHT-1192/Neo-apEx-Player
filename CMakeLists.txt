cmake_minimum_required(VERSION 3.20)
project(neo_apex_player CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
endif()

# Declared here, before anything tests it: the standalone test targets are
# guarded with `if(NOT APEX_BUILD_PLAYER)` so that enabling the player does not
# create them twice (cmake/engine.cmake owns them in that configuration).
option(APEX_BUILD_PLAYER "Build the apEx player (needs the full engine port)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(SHADERC REQUIRED shaderc)
pkg_check_modules(SDL3 REQUIRED sdl3)

# ---------------------------------------------------------------------------
# The vendored apEx code targets MSVC.  These flags make clang accept it:
#   -fms-extensions             __int64, __declspec, forward-declared enums, ...
#   -fdelayed-template-parsing  MSVC-style two-phase lookup in BaseLib templates
add_library(apex_compat_flags INTERFACE)

# The standard headers must be parsed before compat/Windows.h defines min/max as
# macros: libstdc++ uses those names unqualified inside its own headers and the
# macros turn its definitions into syntax errors.  See the header for the full
# explanation.
target_compile_options(apex_compat_flags INTERFACE
  "-include" "${CMAKE_CURRENT_SOURCE_DIR}/compat/std_before_windows.h")
target_compile_options(apex_compat_flags INTERFACE
  -fms-extensions
  -fdelayed-template-parsing
  -Wno-everything
  -Wno-gnu-zero-variadic-macro-arguments
)
target_compile_definitions(apex_compat_flags INTERFACE
  "_WINSOCKAPI_"
  "NOMINMAX"
  # The engine's own PhoenixConfig.h is the authoritative feature set.
)

# compat/ must precede the system headers so our Windows.h/D3D11 shims win.
set(APEX_COMPAT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# The compat headers are capitalised where Windows capitalises them (Windows.h,
# MMSystem.h), and the vendored engine includes them the way MSVC does -- which
# is case-insensitively, so <windows.h> and <mmsystem.h> appear as often as the
# capitalised spelling.  That works on a case-insensitive filesystem (macOS) and
# fails on Linux, where the include simply is not found.
#
# Generate lowercase aliases into the build tree rather than shipping a second
# copy of every header in the repository -- or a stack of symlinks that some
# checkouts (Windows, archive exports) turn into text files.
set(APEX_COMPAT_LOWERCASE ${CMAKE_BINARY_DIR}/compat-lowercase)
file(MAKE_DIRECTORY ${APEX_COMPAT_LOWERCASE})
file(GLOB APEX_COMPAT_HEADERS RELATIVE ${APEX_COMPAT_INCLUDE} ${APEX_COMPAT_INCLUDE}/*.h)
foreach(_header ${APEX_COMPAT_HEADERS})
  string(TOLOWER ${_header} _lower)
  if(NOT _lower STREQUAL _header)
    file(WRITE ${APEX_COMPAT_LOWERCASE}/${_lower} "#include \"${_header}\"\n")
  endif()
endforeach()
set(APEX_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx)

# ---------------------------------------------------------------------------
# Renderer core (Vulkan bring-up + HLSL->SPIR-V compiler)

add_library(phx_vk STATIC
  src/rhi/vk_core.cpp
  src/rhi/shader_compiler.cpp
  src/rhi/gs_emulation.cpp
  src/rhi/shader_interface.cpp
)
target_include_directories(phx_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(phx_vk SYSTEM PUBLIC ${VULKAN_INCLUDE_DIRS} ${SHADERC_INCLUDE_DIRS})
# *_LINK_LIBRARIES are absolute paths; plain -lvulkan is not on the default
# search path on a Homebrew-only setup.
target_link_directories(phx_vk PUBLIC ${VULKAN_LIBRARY_DIRS} ${SHADERC_LIBRARY_DIRS})
target_link_libraries(phx_vk PUBLIC ${VULKAN_LINK_LIBRARIES} ${SHADERC_LINK_LIBRARIES})
target_link_libraries(phx_vk PUBLIC apex_compat_flags)

# ---------------------------------------------------------------------------
# Vendored apEx sources that must NOT be compiled.
#
#   Bedrock/CoRE2/WindowHandler.cpp
#     The Win32 window/message implementation (RegisterClass/CreateWindow/
#     WndProc/PeekMessage/LoadCursor).  It is replaced by
#     src/platform/window_sdl.cpp, which implements the same
#     CCoreWindowParameters / CCoreWindowHandler / CCoreWindowHandlerWin
#     declarations on SDL3.  Compiling both would define those classes twice,
#     and compat/Windows.h has no window-class or message-queue API at all.
#
# Keep this list in sync with docs/spec/platform-port.md 7.4 ("Files that must
# be excluded from the build").  Sources added through phx_apex_sources() are
# checked against it, so accidentally compiling one is a configure error rather
# than a pile of link errors.

set(APEX_EXCLUDED_SOURCES
  "${APEX_THIRD_PARTY}/Bedrock/CoRE2/WindowHandler.cpp"
)

function(phx_apex_sources target)
  foreach(src IN LISTS ARGN)
    list(FIND APEX_EXCLUDED_SOURCES "${src}" excluded_index)
    if(NOT excluded_index EQUAL -1)
      message(FATAL_ERROR
        "${src} is listed in APEX_EXCLUDED_SOURCES and must not be compiled (see the comment there).")
    endif()
    target_sources(${target} PRIVATE "${src}")
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Developer tool: compile the shaders extracted from a .apx and print
# reflection.  Used to validate the D3D11 -> Vulkan binding mapping.

add_executable(phx_shadertool tools/shadertool.cpp)
target_link_libraries(phx_shadertool PRIVATE phx_vk)

# ---------------------------------------------------------------------------
# Window/platform layer test: opens a real SDL3 Vulkan window through
# CCoreWindowHandlerWin, pumps the message loop for ~1 s and prints what it saw.
# The window layer is deliberately independent of the rest of the engine: it
# needs only BaseLib's container/geometry bits, and the engine symbols the test
# would otherwise drag in are doubled in tests/window_sdl_test_link_doubles.cpp.
#
# NOTE: with -DAPEX_BUILD_PLAYER=ON, cmake/engine.cmake creates its own
# phx_window_test that links the full engine target (real Device.cpp/Logger.cpp/
# String.cpp; src/platform/window_sdl.cpp is part of apex_engine there).  The two
# definitions are mutually exclusive - this standalone one only exists when the
# player is off.

if(NOT APEX_BUILD_PLAYER)

add_executable(phx_window_test
  tests/window_sdl_test.cpp
  tests/window_sdl_test_link_doubles.cpp
  src/platform/window_sdl.cpp
  # window_sdl.cpp registers the primary window through this module; it also
  # implements ShowWindow/SetFocus/SetForegroundWindow for the engine handle.
  src/platform/window_handle.cpp
)
# BaseLib's geometry classes are mutually recursive: the CPoint/CRect
# constructors that CCoreWindowHandler's members need pull the whole
# Vector/Matrix/Quaternion/PRS cluster in with them, so all of it is linked
# rather than faked.  SpecMath.cpp provides InvSqrt(), Dictionary.cpp the two
# DictionaryHash overloads that CCoreDevice's CDictionaryEnumerable members need.
phx_apex_sources(phx_window_test
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Vector.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Matrix.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Quaternion.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/PRS.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Rectangle.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/SpecMath.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Dictionary.cpp
)
target_include_directories(phx_window_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${APEX_COMPAT_INCLUDE}
  ${APEX_COMPAT_LOWERCASE}
  ${APEX_THIRD_PARTY}/Bedrock/CoRE2
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib/RapidXML
)
target_include_directories(phx_window_test SYSTEM PRIVATE ${SDL3_INCLUDE_DIRS})
target_link_directories(phx_window_test PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(phx_window_test PRIVATE ${SDL3_LINK_LIBRARIES} apex_compat_flags)

endif() # NOT APEX_BUILD_PLAYER

# ---------------------------------------------------------------------------
# D3D11-on-Vulkan shim: the compat/ D3D11 API implemented over phx_vk.
# The vendored CoRE2 DX11 backend and the Phoenix engine compile on top of it.

add_library(phx_d3d11 STATIC
  src/d3d11/d3d11_blob.cpp
  src/d3d11/d3d11_context.cpp
  src/d3d11/d3d11_convert.cpp
  src/d3d11/d3d11_device.cpp
  src/d3d11/d3d11_reflection.cpp
  src/d3d11/d3d11_resource.cpp
  src/d3d11/d3d11_shader.cpp
  src/d3d11/d3d11_state.cpp
  src/d3d11/d3d11_swapchain.cpp
  src/d3d11/d3dcompiler_shim.cpp
  src/d3d11/dxgi_formats.cpp
  src/d3d11/vk_swapchain.cpp
  # The swapchain resolves the engine's TU32 window handle through
  # PhxWindowFromEngineHandle().  That resolver lives in its own
  # dependency-free module - linking the whole SDL window layer here would drag
  # BaseLib/Logger into every consumer of the shim.
  src/platform/window_handle.cpp
)
# compat/ first so <d3d11.h>/<dxgi.h> resolve to the shim headers.
# compat/ first so <d3d11.h>/<dxgi.h> resolve to the shim headers; src/ so
# src/platform/window_sdl.h is reachable; the CoRE2/UtilLib paths because
# window_sdl.h includes the vendored WindowHandler.h and XMLDocument.h.
target_include_directories(phx_d3d11 PRIVATE
  ${APEX_COMPAT_INCLUDE}
  ${APEX_COMPAT_LOWERCASE}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${APEX_THIRD_PARTY}/Bedrock/CoRE2
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib/RapidXML
)
target_include_directories(phx_d3d11 SYSTEM PUBLIC ${SDL3_INCLUDE_DIRS})
target_link_directories(phx_d3d11 PUBLIC ${SDL3_LIBRARY_DIRS})
target_link_libraries(phx_d3d11 PUBLIC phx_vk ${SDL3_LINK_LIBRARIES})
target_compile_options(phx_d3d11 PRIVATE -Wno-everything)

# ---------------------------------------------------------------------------
# Headless render smoke test: device + shader compile + draw + readback.
#
# phx_d3d11 depends on SDL only through src/platform/window_handle.cpp (the
# engine-handle resolver), which needs neither BaseLib nor CLogger, so this test
# links the shim and nothing else.  It used to need
# tests/window_sdl_test_link_doubles.cpp + BaseLib/Dictionary.cpp while
# window_sdl.cpp was part of the shim; keep them out, or they will mask a real
# link error the next time the shim grows an engine dependency.

add_executable(phx_smoke tests/vk_d3d11_smoke.cpp)
target_include_directories(phx_smoke PRIVATE
  ${APEX_COMPAT_INCLUDE}
  ${APEX_COMPAT_LOWERCASE}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
  ${APEX_THIRD_PARTY}/Bedrock/CoRE2
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib
)
target_link_libraries(phx_smoke PRIVATE phx_d3d11)

# ---------------------------------------------------------------------------
# Geometry shader emulation (src/rhi/gs_emulation.cpp).
#
#   ./build/phx_gs_test
#
# Runs the real "Wireframe" geometry shader from Clean Slate -- the one behind
# the missing background grid -- through the shape reader, the analysis, the
# GS->VS translation and the fat-geometry builder, and checks the strip layout
# the emulation hands to the fixed-function assembler.  Exit status == failures.
add_executable(phx_gs_test
  tests/gs_emulation_test.cpp
)
target_link_libraries(phx_gs_test PRIVATE phx_vk)
target_compile_options(phx_gs_test PRIVATE -Wno-everything)

# ---------------------------------------------------------------------------
# Shader interface normalisation (src/rhi/shader_interface.cpp).
#
#   ./build/phx_interface_test
#
# Compiles the engine's own 2D quad vertex shader and the loading-screen
# project's logo-filter pixel shader -- the pair that made
# vkCreateGraphicsPipelines fail for 10 of the 11 bundled projects -- and checks
# that D3D11's signature linkage is lowered to Vulkan's location linkage: the
# vertex shader's float2 TEXCOORD0 and the pixel shader's float4 both come back
# four components wide, a shader that needs nothing is untouched byte for byte,
# and a stage with no varying interface is left alone.
# Exit status == failed checks.
add_executable(phx_interface_test
  tests/shader_interface_test.cpp
)
target_link_libraries(phx_interface_test PRIVATE phx_vk)
target_compile_options(phx_interface_test PRIVATE -Wno-everything)

# ---------------------------------------------------------------------------
# GDI-on-CoreText test (src/platform/gdi_posix.cpp).
#
#   ./build/phx_gdi_test
#
# Walks real GGO_NATIVE glyph outlines exactly the way Phoenix/Mesh.cpp:1874-2012
# does (CreateFontA -> SelectObject -> GetGlyphOutline -> the engine's own
# TTPOLYGONHEADER/TTPOLYCURVE pointer arithmetic and TOFLOAT 16.16 decode),
# checks that decoding the stream with Mesh.cpp's TT_PRIM_QSPLINE reconstruction
# rule reproduces CoreText's outline for the glyph, renders text through the
# Texgen.cpp:257-298 DC/bitmap/GetDIBits path, and confirms the refusals
# (GGO_BITMAP/GGO_GRAY8_BITMAP) fail loudly.  Exit status == failed checks.
#
# Apple-only: the file it tests is the macOS GDI implementation, and the test
# links the same frameworks as apex_engine.
if(APPLE)
  add_executable(phx_gdi_test
    tests/gdi_glyph_test.cpp
    src/platform/gdi_posix.cpp
  )
  target_include_directories(phx_gdi_test PRIVATE
    ${APEX_COMPAT_INCLUDE}
  ${APEX_COMPAT_LOWERCASE}          # must come first: it shadows <Windows.h>
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_options(phx_gdi_test PRIVATE
    -fms-extensions
    -fdelayed-template-parsing
    -Wno-everything
  )
  target_link_libraries(phx_gdi_test PRIVATE
    "-framework CoreText"
    "-framework CoreGraphics"
    "-framework CoreFoundation"
    "-framework ImageIO"
  )
endif()

# ---------------------------------------------------------------------------
# RapidXML XML layer.
#
# Bedrock/UtilLib/XMLDocument.cpp + XMLNode.cpp implement the engine's
# CXMLDocument / CXMLNode on top of the vendored RapidXML instead of MSXML, so
# that the XML headers no longer pull in msxml3.tlh (which is Windows/COM only).
# The static library exists so the round-trip test can link the real
# implementation without dragging in the whole engine.
#
# The XML sources themselves only need CString and the CLogger global, but
# CString's base64 code references CStreamWriterMemory, so String.cpp comes with
# StreamWriter.cpp.  src/platform/win32_compat.cpp supplies the handful of Win32
# entry points those BaseLib sources call on the POSIX side.

add_library(phx_xml STATIC
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib/XMLDocument.cpp
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib/XMLNode.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/String.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Logger.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/Hash.cpp
  ${APEX_THIRD_PARTY}/Bedrock/BaseLib/StreamWriter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32_compat.cpp
)
target_include_directories(phx_xml PUBLIC
  ${APEX_COMPAT_INCLUDE}
  ${APEX_COMPAT_LOWERCASE}
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib
  ${APEX_THIRD_PARTY}/Bedrock/UtilLib/RapidXML
)
target_link_libraries(phx_xml PUBLIC apex_compat_flags)

# ---------------------------------------------------------------------------
# XML round-trip test:  ./build/phx_xml_test [path-to.apx]
#
# Parses a real project file, walks it with the exact access pattern the .apx
# loader uses, and asserts that parse -> serialise -> re-parse preserves both the
# bytes and every name-indexed child count.

add_executable(phx_xml_test tests/xml_roundtrip_test.cpp)
target_link_libraries(phx_xml_test PRIVATE phx_xml)

# ---------------------------------------------------------------------------
# Pre-rendered song reader:  ./build/phx_song_pcm_test [real.apxsndcache]
#
# The MVX synthesiser cannot be ported, so music playback depends entirely on
# reading the container the Windows tool writes.  A wrong sample count or marker
# table would desynchronise the demo, so the format is pinned by a test.
add_executable(phx_song_pcm_test
  tests/song_pcm_test.cpp
  src/player/song_pcm.cpp
)
target_include_directories(phx_song_pcm_test PRIVATE src/player)

# ---------------------------------------------------------------------------
# The full player: vendored apEx engine + platform layer + D3D11 shim.
# Off by default while the engine port is still being completed, so that the
# renderer/shim/window targets above can be built and tested independently.
# Turn on with -DAPEX_BUILD_PLAYER=ON.
if(APEX_BUILD_PLAYER)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine.cmake)
endif()
