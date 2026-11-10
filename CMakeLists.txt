cmake_minimum_required(VERSION 3.25)
project(Flick CXX)

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

add_executable(Flick WIN32 src/main.cpp src/window.cpp src/lang.cpp src/flick.manifest)
target_compile_definitions(Flick PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(Flick PRIVATE lexilla scintilla
  d2d1 dwrite windowscodecs dwmapi shlwapi uxtheme shcore imm32 msimg32 ole32 oleaut32 uuid psapi)
