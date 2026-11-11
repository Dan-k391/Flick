cmake_minimum_required(VERSION 3.25)
project(Flick C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no redist needed, slightly faster process start.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /EHsc /MP)

file(GLOB SCI_SRC third_party/scintilla/src/*.cxx third_party/scintilla/win32/*.cxx)
list(FILTER SCI_SRC EXCLUDE REGEX "ScintillaDLL")
add_library(scintilla STATIC ${SCI_SRC})
target_include_directories(scintilla PUBLIC third_party/scintilla/include PRIVATE third_party/scintilla/src)

file(GLOB LEX_SRC third_party/lexilla/src/Lexilla.cxx third_party/lexilla/lexlib/*.cxx third_party/lexilla/lexers/*.cxx)
add_library(lexilla STATIC ${LEX_SRC})
target_include_directories(lexilla PUBLIC third_party/lexilla/include PRIVATE third_party/lexilla/lexlib)
target_link_libraries(lexilla PUBLIC scintilla)
target_compile_definitions(lexilla PRIVATE LEXILLA_NO_EXPORT)

add_library(md4c STATIC third_party/md4c/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c)
target_compile_definitions(md4c PUBLIC MD4C_USE_UTF16)  # parse wchar_t text directly

file(GLOB PLUTOVG_SRC third_party/lunasvg/plutovg/source/*.c)
add_library(plutovg STATIC ${PLUTOVG_SRC})
target_include_directories(plutovg PUBLIC third_party/lunasvg/plutovg/include)
target_compile_definitions(plutovg PUBLIC PLUTOVG_BUILD_STATIC PRIVATE PLUTOVG_BUILD PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD)
file(GLOB LUNASVG_SRC third_party/lunasvg/source/*.cpp)
add_library(lunasvg STATIC ${LUNASVG_SRC})
target_include_directories(lunasvg PUBLIC third_party/lunasvg/include)
target_compile_definitions(lunasvg PUBLIC LUNASVG_BUILD_STATIC PRIVATE LUNASVG_BUILD LUNASVG_DISABLE_LOAD_SYSTEM_FONTS)
target_link_libraries(lunasvg PRIVATE plutovg)

add_executable(Flick WIN32 src/main.cpp src/window.cpp src/lang.cpp src/markdown.cpp src/pdf.cpp src/grid.cpp src/font.cpp src/svg.cpp src/fileinfo.cpp src/highlight.cpp src/convert.cpp src/media.cpp src/flick.manifest src/flick.rc)
set_source_files_properties(src/flick.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/flick.ico)  # rebuild when the icon changes
target_compile_definitions(Flick PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(Flick PRIVATE lexilla scintilla md4c lunasvg windowsapp windows.data.pdf d3d11 version msi crypt32 mfplat mfuuid mfreadwrite propsys
  d2d1 dwrite windowscodecs dwmapi shlwapi uxtheme shcore imm32 msimg32 ole32 oleaut32 uuid psapi)
