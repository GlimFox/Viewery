cmake_minimum_required(VERSION 3.25)
project(Viewery VERSION 0.2.0 LANGUAGES CXX)
set(VIEWERY_VERSION "0.2.0-beta.1" CACHE STRING "Semantic version published by Viewery")
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
    message(FATAL_ERROR "Viewery requires Windows x64 and the Microsoft C++ toolchain.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
file(TO_CMAKE_PATH "$ENV{INCLUDE}" viewery_windows_includes)
foreach(include_dir IN LISTS viewery_windows_includes)
    if(EXISTS "${include_dir}/../cppwinrt/winrt/base.h")
        list(APPEND viewery_winrt_includes "${include_dir}/../cppwinrt")
    endif()
endforeach()

function(viewery_target name)
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${name} PRIVATE src ${viewery_winrt_includes})
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 VIEWERY_VERSION="${VIEWERY_VERSION}")
    target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8 /EHsc)
endfunction()

add_executable(Viewery WIN32 src/main.cpp src/catalog.cpp src/media.cpp src/explorer.cpp
    src/converter.cpp src/frame_image.cpp src/library.cpp src/rename_template.cpp src/rename_studio.cpp
    src/updater.cpp src/runtime_paths.cpp src/collection_view.cpp src/Viewery.rc)
set_source_files_properties(src/Viewery.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Viewery.manifest;${CMAKE_CURRENT_SOURCE_DIR}/src/Viewery.ico")
viewery_target(Viewery)
target_link_options(Viewery PRIVATE /MANIFEST:NO)
target_link_libraries(Viewery PRIVATE advapi32 comctl32 dwmapi gdi32 ole32 oleaut32
    shell32 shlwapi user32 uuid windowscodecs bcrypt winhttp windowsapp runtimeobject uxtheme)
add_custom_command(TARGET Viewery POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Get-MediaTools.ps1" "$<TARGET_FILE_DIR:Viewery>/Get-MediaTools.ps1")

add_library(VieweryShell SHARED src/shell_command.cpp)
viewery_target(VieweryShell)
target_link_libraries(VieweryShell PRIVATE ole32 shell32 shlwapi)
set_target_properties(VieweryShell PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CTest)
if(BUILD_TESTING)
    add_executable(viewery-library-tests tests/library_smoke.cpp src/library.cpp)
    viewery_target(viewery-library-tests)
    target_link_libraries(viewery-library-tests PRIVATE shell32 ole32 bcrypt winsqlite3)
    add_test(NAME collection-and-rename COMMAND viewery-library-tests "${CMAKE_BINARY_DIR}/library-test-data")
    set_tests_properties(collection-and-rename PROPERTIES TIMEOUT 120)
    add_executable(viewery-template-tests tests/template_smoke.cpp src/rename_template.cpp src/library.cpp)
    viewery_target(viewery-template-tests)
    target_link_libraries(viewery-template-tests PRIVATE shell32 ole32 bcrypt)
    add_test(NAME rename-templates COMMAND viewery-template-tests)
    add_executable(viewery-collection-tests tests/collection_view_smoke.cpp src/collection_view.cpp)
    viewery_target(viewery-collection-tests)
    target_link_libraries(viewery-collection-tests PRIVATE shlwapi)
    add_test(NAME collection-view COMMAND viewery-collection-tests)
    add_executable(viewery-updater-tests tests/updater_tests.cpp src/runtime_paths.cpp)
    viewery_target(viewery-updater-tests)
    target_link_libraries(viewery-updater-tests PRIVATE winhttp windowsapp runtimeobject shell32 ole32 advapi32 user32)
    add_test(NAME update-validation COMMAND viewery-updater-tests)
endif()
option(VIEWERY_MEDIA_TESTS "Enable integration tests with local libmpv and FFmpeg binaries" OFF)
if(BUILD_TESTING AND VIEWERY_MEDIA_TESTS)
    add_executable(viewery-playback-smoke tests/playback_smoke.cpp src/media.cpp src/runtime_paths.cpp)
    viewery_target(viewery-playback-smoke)
    target_link_libraries(viewery-playback-smoke PRIVATE user32 shell32 ole32)
    add_executable(viewery-converter-smoke tests/converter_smoke.cpp src/converter.cpp src/runtime_paths.cpp)
    viewery_target(viewery-converter-smoke)
    target_link_libraries(viewery-converter-smoke PRIVATE shell32 ole32)
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

option(VIEWERY_GUI_TESTS "Enable isolated native Rename Studio GUI checks" OFF)
if(BUILD_TESTING AND VIEWERY_GUI_TESTS)
    add_executable(viewery-rename-studio-tests tests/rename_studio_smoke.cpp
        src/rename_studio.cpp src/rename_template.cpp src/library.cpp src/frame_image.cpp)
    viewery_target(viewery-rename-studio-tests)
    target_link_libraries(viewery-rename-studio-tests PRIVATE
        comctl32 uxtheme user32 gdi32 ole32 shell32 windowscodecs bcrypt)
    add_test(NAME rename-studio-gui COMMAND viewery-rename-studio-tests
        "${CMAKE_BINARY_DIR}/studio-gui-test-data")
    set_tests_properties(rename-studio-gui PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
