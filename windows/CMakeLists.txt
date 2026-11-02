cmake_minimum_required(VERSION 3.24)
project(TinycastWindows VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "The Windows port requires the Windows SDK and MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zi)
  add_link_options(/DEBUG:FULL)
endif()

add_library(tinycast_core STATIC src/core.cpp)
target_include_directories(tinycast_core PUBLIC src)

add_library(tinycast_platform STATIC src/platform.cpp)
target_link_libraries(tinycast_platform PUBLIC tinycast_core
  user32 gdi32 dwmapi shell32 ole32 advapi32 uuid windowsapp runtimeobject)

find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
set(ICON_FILE "${CMAKE_CURRENT_BINARY_DIR}/tinycast.ico")
set(MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")
execute_process(COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-icon.ps1" -OutputPath "${ICON_FILE}"
  COMMAND_ERROR_IS_FATAL ANY)
configure_file(resources/app.rc "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
add_executable(Tinycast WIN32 src/main.cpp src/palette.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
target_link_libraries(Tinycast PRIVATE tinycast_platform comctl32 uxtheme psapi)
target_include_directories(Tinycast PRIVATE resources)
if(MSVC)
  target_link_options(Tinycast PRIVATE /MANIFEST:NO)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE tinycast_core)
  add_test(NAME core COMMAND core_tests)

  add_executable(platform_tests tests/platform_tests.cpp)
  target_link_libraries(platform_tests PRIVATE tinycast_platform dbghelp)
  add_test(NAME platform COMMAND platform_tests)
  set_tests_properties(core platform PROPERTIES TIMEOUT 30)
endif()
