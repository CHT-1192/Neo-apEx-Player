# The player: vendored apEx engine + platform layer + D3D11-on-Vulkan shim.
#
# Included from CMakeLists.txt with a single `include(cmake/engine.cmake)`.
# The source list is generated (tools/gen_engine_sources.py) so that the
# Windows-only translation units we drop stay auditable, and the shim/player
# directories are globbed so that adding a file does not require touching this
# file.

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_sources.cmake)

file(GLOB APEX_D3D11_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/d3d11/*.cpp)
# The portable replacement for d3dx10.lib (declarations are in
# compat/D3DX10Math.h, definitions here).
file(GLOB APEX_D3DX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/d3dx/*.cpp)
file(GLOB APEX_PLAYER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/window_sdl.cpp
)

add_library(apex_engine STATIC
  ${APEX_ENGINE_SOURCES}
  ${APEX_PLATFORM_SOURCES}
  ${APEX_D3D11_SOURCES}
  ${APEX_D3DX_SOURCES}
)

target_include_directories(apex_engine PUBLIC
  ${APEX_COMPAT_INCLUDE}          # must come first: it shadows <Windows.h> etc.
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/Bedrock/UtilLib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/Bedrock/UtilLib/RapidXML
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/Bedrock/CoRE2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/Phoenix
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/Phoenix_Tool
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apEx/LibCTiny
)
target_include_directories(apex_engine SYSTEM PUBLIC ${SDL3_INCLUDE_DIRS})
target_compile_options(apex_engine PRIVATE
  -fms-extensions -fdelayed-template-parsing -Wno-everything
  -Wno-gnu-zero-variadic-macro-arguments
  -DGL_SILENCE_DEPRECATION      # the GLU tessellator is used by the text mesh generator
)
target_link_libraries(apex_engine PUBLIC phx_vk apex_compat_flags)
target_link_directories(apex_engine PUBLIC ${SDL3_LIBRARY_DIRS})
target_link_libraries(apex_engine PUBLIC ${SDL3_LINK_LIBRARIES})
if(APPLE)
  # GLU lives in the OpenGL framework; deprecated since 10.9 but functional and
  # required by Phoenix's text mesh tessellation.
  target_link_libraries(apex_engine PUBLIC "-framework OpenGL")
  # src/platform/gdi_posix.cpp implements the Win32 GDI subset on CoreText /
  # CoreGraphics / ImageIO (glyph outlines for the text meshes, and the texgen
  # picture path).
  target_link_libraries(apex_engine PUBLIC
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework ImageIO"
  )
endif()

# ---------------------------------------------------------------------------
# Dear ImGui, vendored in third_party/imgui (v1.91.8, MIT).
#
# The DX11 backend is the right one here, not the Vulkan backend: this project's
# D3D11 is a real implementation on Vulkan, the engine already draws through it,
# and ImGui's DX11 shaders therefore go through the same HLSL -> SPIR-V path and
# into the same frame as everything else.  Using the Vulkan backend would mean
# standing up a second render path that has to agree with the shim about render
# passes, dynamic rendering and image layouts.
add_library(phx_imgui STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_dx11.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(phx_imgui SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
  ${APEX_COMPAT_INCLUDE}
  ${SDL3_INCLUDE_DIRS}
)
# imgui_impl_dx11 includes <d3d11.h>, which compat/ shadows with the subset this
# port implements, so it needs the same flags the rest of the port uses.
target_link_libraries(phx_imgui PUBLIC apex_compat_flags)
target_link_directories(phx_imgui PUBLIC ${SDL3_LIBRARY_DIRS})
target_link_libraries(phx_imgui PUBLIC ${SDL3_LINK_LIBRARIES})
# ImGui's sources are third-party and warn freely; the project builds with
# -Wno-everything for the vendored tree, and the backends should not be an
# exception that drowns out our own diagnostics.
target_compile_options(phx_imgui PRIVATE -w)

add_executable(neo_apex_player ${APEX_PLAYER_SOURCES})
target_link_libraries(neo_apex_player PRIVATE apex_engine phx_imgui)

# Headless render smoke test (owned by the D3D11 shim work).
#
# CMakeLists.txt also declares this target for the renderer-only configuration;
# guard on the name so that including this file never creates a duplicate
# target (CMP0002 is a hard error).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/vk_d3d11_smoke.cpp AND NOT TARGET phx_smoke)
  add_executable(phx_smoke tests/vk_d3d11_smoke.cpp)
  target_link_libraries(phx_smoke PRIVATE apex_engine)
endif()
# (phx_window_test is defined in CMakeLists.txt for the renderer-only build;
#  with the player enabled, neo_apex_player exercises the window layer itself.)
