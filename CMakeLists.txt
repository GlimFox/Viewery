cmake_minimum_required(VERSION 3.25)
project(Viewery VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
    message(FATAL_ERROR "Viewery requires Windows x64 and the Microsoft C++ toolchain.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

function(viewery_target name)
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${name} PRIVATE src)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8 /EHsc)
endfunction()

add_executable(Viewery WIN32 src/main.cpp src/catalog.cpp src/media.cpp src/explorer.cpp
    src/converter.cpp src/frame_image.cpp src/Viewery.rc)
set_source_files_properties(src/Viewery.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Viewery.manifest;${CMAKE_CURRENT_SOURCE_DIR}/src/Viewery.ico")
viewery_target(Viewery)
target_link_options(Viewery PRIVATE /MANIFEST:NO)
target_link_libraries(Viewery PRIVATE advapi32 comctl32 dwmapi gdi32 ole32 oleaut32
    shell32 shlwapi user32 uuid windowscodecs)

add_library(VieweryShell SHARED src/shell_command.cpp)
viewery_target(VieweryShell)
target_link_libraries(VieweryShell PRIVATE ole32 shell32 shlwapi)
set_target_properties(VieweryShell PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CTest)
option(VIEWERY_MEDIA_TESTS "Enable integration tests with local libmpv and FFmpeg binaries" OFF)
if(BUILD_TESTING AND VIEWERY_MEDIA_TESTS)
    add_executable(viewery-playback-smoke tests/playback_smoke.cpp src/media.cpp)
    viewery_target(viewery-playback-smoke)
    target_link_libraries(viewery-playback-smoke PRIVATE user32)
    add_executable(viewery-converter-smoke tests/converter_smoke.cpp src/converter.cpp)
    viewery_target(viewery-converter-smoke)
    find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)
    set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    add_test(NAME create-media-fixtures COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/New-Fixtures.ps1"
        -Ffmpeg "$<TARGET_FILE_DIR:Viewery>/ffmpeg.exe" -Destination "${fixtures}")
    set_tests_properties(create-media-fixtures PROPERTIES FIXTURES_SETUP media TIMEOUT 30)
    add_test(NAME playback COMMAND viewery-playback-smoke
        "${fixtures}/photo.png" "${fixtures}/animation.gif" "${fixtures}/sample.mp4")
    add_test(NAME gif-export COMMAND viewery-converter-smoke
        "${fixtures}/sample.mp4" "${fixtures}")
    set_tests_properties(playback gif-export PROPERTIES FIXTURES_REQUIRED media TIMEOUT 60)
endif()
