cmake_minimum_required(VERSION 3.16)
project(NaikAVPlayer C CXX)

# Enforce C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------
# Platform Configuration
# -------------------------------------------------------------
set(PLATFORM "AUTO" CACHE STRING "Target platform (WINDOWS, LINUX, or AUTO)")
set_property(CACHE PLATFORM PROPERTY STRINGS "WINDOWS" "LINUX" "AUTO")

# Auto-detect or use explicit target platform
if(PLATFORM STREQUAL "AUTO")
    if(WIN32)
        set(TARGET_PLATFORM_WINDOWS ON)
    elseif(UNIX)
        set(TARGET_PLATFORM_LINUX ON)
    else()
        message(FATAL_ERROR "Unsupported host platform. Please explicitly specify -DPLATFORM=WINDOWS or -DPLATFORM=LINUX")
    endif()
elseif(PLATFORM STREQUAL "WINDOWS")
    set(TARGET_PLATFORM_WINDOWS ON)
elseif(PLATFORM STREQUAL "LINUX")
    set(TARGET_PLATFORM_LINUX ON)
else()
    message(FATAL_ERROR "Invalid PLATFORM option specified. Options: WINDOWS, LINUX, AUTO")
endif()

# Single-config generators default to an empty CMAKE_BUILD_TYPE, which means
# no optimization flags at all. _FORTIFY_SOURCE requires -O1+ on glibc, and an
# unoptimized "default" build is never what we want anyway - so default to
# Release when the user hasn't chosen.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# -------------------------------------------------------------
# Compiler & Linker Hardening Options
# -------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/MinGW/Clang hardening flags
    add_compile_options(
        -fstack-protector-strong -Wformat -Wformat-security
        "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>"
    )
elseif(MSVC)
    # MSVC uses its own security flags instead of GCC/Clang warnings
    add_compile_options(/GS)
endif()
if(TARGET_PLATFORM_WINDOWS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_link_options("-Wl,--dynamicbase" "-Wl,--nxcompat")
    elseif(MSVC)
        add_link_options(/DYNAMICBASE /NXCOMPAT)
    endif()
endif()

# -------------------------------------------------------------
# C++ Compiler Warnings & Sanitizer Settings
# -------------------------------------------------------------
option(TREAT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR "Address/Undefined Sanitizers and ThreadSanitizer are mutually exclusive. Please enable only one.")
endif()

set(PROJECT_WARNING_FLAGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(PROJECT_WARNING_FLAGS "-Wall" "-Wextra" "-Wpedantic")
    if(TREAT_WARNINGS_AS_ERRORS)
        list(APPEND PROJECT_WARNING_FLAGS "-Werror")
    endif()
endif()

if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        include(CMakePushCheckState)
        include(CheckCXXSourceCompiles)
        cmake_push_check_state()
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
        check_cxx_source_compiles("int main() { return 0; }" COMPILER_SUPPORTS_SANITIZERS)
        cmake_pop_check_state()

        if(NOT COMPILER_SUPPORTS_SANITIZERS)
            message(FATAL_ERROR "ENABLE_SANITIZERS is ON, but the compiler/linker does not support or cannot find the required sanitizer libraries (e.g., -lasan or -lubsan).\n"
                                "If you are on Windows using MinGW, please make sure you have the necessary sanitizer packages installed, or disable sanitizers by running:\n"
                                "  cmake -B build -DENABLE_SANITIZERS=OFF")
        endif()

        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        include(CMakePushCheckState)
        include(CheckCXXSourceCompiles)
        cmake_push_check_state()
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=thread")
        check_cxx_source_compiles("int main() { return 0; }" COMPILER_SUPPORTS_TSAN)
        cmake_pop_check_state()

        if(NOT COMPILER_SUPPORTS_TSAN)
            message(FATAL_ERROR "ENABLE_TSAN is ON, but the compiler/linker does not support or cannot find the required ThreadSanitizer libraries.\n"
                                "Please disable ThreadSanitizer by running:\n"
                                "  cmake -B build -DENABLE_TSAN=OFF")
        endif()

        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# -------------------------------------------------------------
# Fetch SDL3
# -------------------------------------------------------------
include(FetchContent)

# Optimize SDL3 build time: build shared only, disable unused subsystems
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

# Enable only required subsystems for NaikAVPlayer
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_EVENTS ON CACHE BOOL "" FORCE)
set(SDL_THREADS ON CACHE BOOL "" FORCE)
set(SDL_RENDER ON CACHE BOOL "" FORCE)

# Disable all other unused subsystems
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
set(SDL_MISC OFF CACHE BOOL "" FORCE)
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)

message(STATUS "Fetching SDL3...")
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG a962f40bbba175e9716557a25d5d7965f134a3d3  # release-3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# Link libssp (Stack Smashing Protector) to SDL3-shared on Windows MinGW/Clang builds
if(TARGET_PLATFORM_WINDOWS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(TARGET SDL3-shared)
            target_link_libraries(SDL3-shared PRIVATE ssp)
        endif()
    endif()
endif()

# -------------------------------------------------------------
# Fetch Dear ImGui
# -------------------------------------------------------------
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 97428e8ac99e339ce05eee531cf55b77b29ea709  # v1.91.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# Create a static library for ImGui
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

# ImGui needs SDL headers
target_link_libraries(imgui PRIVATE SDL3::SDL3-shared)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# -------------------------------------------------------------
# Fetch Native File Dialog Extended (NFD)
# -------------------------------------------------------------
message(STATUS "Fetching Native File Dialog Extended (NFD)...")
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nativefiledialog-extended
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        86d5f2005fe1c00747348a12070fec493ea2407e  # v1.2.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nativefiledialog-extended)


# -------------------------------------------------------------
# Configure FFmpeg
# -------------------------------------------------------------
# Two sources are supported:
#
#   1. SYSTEM (via pkg-config) - REQUIRED on Linux ARM64 (Raspberry Pi).
#      The distro package (e.g. Raspberry Pi OS's libavcodec-dev) is built
#      against the running kernel's headers and validated against the Pi's
#      bcm2835-codec V4L2 M2M driver. BtbN's generic linuxarm64 build
#      programs V4L2 buffer formats that the Pi driver does not accept the
#      same way, causing hardware-decoded playback to hang inside a blocking
#      VIDIOC_DQBUF / poll() in kernel-space. Do NOT bundle BtbN FFmpeg on
#      ARM64 Linux.
#
#   2. BUNDLED (auto-downloaded from BtbN) - used on Windows and Linux x86_64,
#      where V4L2 M2M is not the primary hardware acceleration path.
#
# Advanced override: -DNAIKAV_FORCE_BUNDLED_FFMPEG=ON forces the bundled
# path even on ARM64 (only useful on non-Pi aarch64 boards where V4L2 M2M
# isn't in play - do NOT set this on a Raspberry Pi).

option(NAIKAV_FORCE_BUNDLED_FFMPEG
    "Force auto-download of BtbN FFmpeg even on ARM64 Linux (breaks V4L2 M2M on Raspberry Pi)"
    OFF)

set(FFMPEG_USE_SYSTEM OFF)
if(TARGET_PLATFORM_LINUX
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64"
   AND NOT NAIKAV_FORCE_BUNDLED_FFMPEG)
    set(FFMPEG_USE_SYSTEM ON)
endif()

set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")

if(FFMPEG_USE_SYSTEM)
    message(STATUS "Linux ARM64 detected: using system FFmpeg via pkg-config "
                   "(required for Raspberry Pi V4L2 M2M hardware decoding).")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswscale
        libswresample)
    message(STATUS "  libavcodec version:    ${FFMPEG_libavcodec_VERSION}")
    message(STATUS "  libavformat version:   ${FFMPEG_libavformat_VERSION}")
    message(STATUS "  libavutil version:     ${FFMPEG_libavutil_VERSION}")
    message(STATUS "  libswscale version:    ${FFMPEG_libswscale_VERSION}")
    message(STATUS "  libswresample version: ${FFMPEG_libswresample_VERSION}")
else()
    # ---------- Bundled FFmpeg (Windows / Linux x86_64) ----------
    # Determine if we need to download FFmpeg
    set(FFMPEG_NEEDS_DOWNLOAD OFF)
    if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
        set(FFMPEG_NEEDS_DOWNLOAD ON)
    elseif(TARGET_PLATFORM_WINDOWS AND NOT EXISTS "${FFMPEG_DIR}/lib/avcodec.lib")
        set(FFMPEG_NEEDS_DOWNLOAD ON)
    elseif(TARGET_PLATFORM_LINUX AND NOT EXISTS "${FFMPEG_DIR}/lib/libavcodec.so")
        set(FFMPEG_NEEDS_DOWNLOAD ON)
    endif()

    if(FFMPEG_NEEDS_DOWNLOAD)
        message(STATUS "FFmpeg headers/libraries not found in thirdparty/ffmpeg for this platform. Setting up automatically...")

        set(FFMPEG_BASE_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-06-30-13-34")

        if(TARGET_PLATFORM_WINDOWS)
            set(FFMPEG_FILENAME "ffmpeg-n8.1.2-21-gce3c09c101-win64-lgpl-shared-8.1.zip")
            set(FFMPEG_SHA256 "27bcaf58b5140171dfe838a0b365d12c60607d71fc168424456410bad6a834da")
        elseif(TARGET_PLATFORM_LINUX)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
                # Only reachable when NAIKAV_FORCE_BUNDLED_FFMPEG=ON (advanced override).
                set(FFMPEG_FILENAME "ffmpeg-n8.1.2-21-gce3c09c101-linuxarm64-lgpl-shared-8.1.tar.xz")
                set(FFMPEG_SHA256 "08a9639de779e0d024af5190ad4d0463c6c91b42ba79f00a51359fb4645e8ce7")
            else()
                set(FFMPEG_FILENAME "ffmpeg-n8.1.2-21-gce3c09c101-linux64-lgpl-shared-8.1.tar.xz")
                set(FFMPEG_SHA256 "bc60572d89533c7987e7cfbb654eba4d35122a77885c6938483dd7ef8219cde9")
            endif()
        endif()

        set(FFMPEG_ARCHIVE_URL "${FFMPEG_BASE_URL}/${FFMPEG_FILENAME}")
        set(FFMPEG_ARCHIVE_PATH "${CMAKE_BINARY_DIR}/${FFMPEG_FILENAME}")
        set(FFMPEG_EXTRACT_DIR "${CMAKE_BINARY_DIR}/ffmpeg_extracted")

        message(STATUS "Downloading FFmpeg shared binaries from ${FFMPEG_ARCHIVE_URL}...")
        if(FFMPEG_SHA256)
            file(DOWNLOAD "${FFMPEG_ARCHIVE_URL}" "${FFMPEG_ARCHIVE_PATH}"
                 SHOW_PROGRESS
                 EXPECTED_HASH SHA256=${FFMPEG_SHA256}
                 STATUS download_status)
        else()
            message(WARNING "FFMPEG_SHA256 is not set for ${FFMPEG_FILENAME} - downloaded FFmpeg archive will not be checksum-verified.")
            file(DOWNLOAD "${FFMPEG_ARCHIVE_URL}" "${FFMPEG_ARCHIVE_PATH}"
                 SHOW_PROGRESS
                 STATUS download_status)
        endif()

        list(GET download_status 0 status_code)
        list(GET download_status 1 status_msg)
        if(NOT status_code EQUAL 0)
            message(FATAL_ERROR "Failed to download FFmpeg (Code: ${status_code}): ${status_msg}")
        endif()

        message(STATUS "Extracting FFmpeg package...")
        file(ARCHIVE_EXTRACT INPUT "${FFMPEG_ARCHIVE_PATH}" DESTINATION "${FFMPEG_EXTRACT_DIR}")

        # Find the extracted folder name
        file(GLOB EXTRACTED_FOLDERS "${FFMPEG_EXTRACT_DIR}/ffmpeg-*")
        list(LENGTH EXTRACTED_FOLDERS folders_len)
        if(folders_len EQUAL 0)
            message(FATAL_ERROR "No extracted FFmpeg folder found. Extraction may have failed.")
        endif()
        list(GET EXTRACTED_FOLDERS 0 FFMPEG_SRC_DIR)

        message(STATUS "Installing FFmpeg to thirdparty/ffmpeg...")
        file(REMOVE_RECURSE "${FFMPEG_DIR}") # Clear clean install
        file(MAKE_DIRECTORY "${FFMPEG_DIR}")

        if(EXISTS "${FFMPEG_SRC_DIR}/bin")
            file(COPY "${FFMPEG_SRC_DIR}/bin" DESTINATION "${FFMPEG_DIR}")
        endif()
        if(EXISTS "${FFMPEG_SRC_DIR}/include")
            file(COPY "${FFMPEG_SRC_DIR}/include" DESTINATION "${FFMPEG_DIR}")
        endif()
        if(EXISTS "${FFMPEG_SRC_DIR}/lib")
            file(COPY "${FFMPEG_SRC_DIR}/lib" DESTINATION "${FFMPEG_DIR}")
        endif()

        # Copy LICENSE and README if present
        if(EXISTS "${FFMPEG_SRC_DIR}/LICENSE.txt")
            file(COPY "${FFMPEG_SRC_DIR}/LICENSE.txt" DESTINATION "${FFMPEG_DIR}")
        endif()
        if(EXISTS "${FFMPEG_SRC_DIR}/README.txt")
            file(COPY "${FFMPEG_SRC_DIR}/README.txt" DESTINATION "${FFMPEG_DIR}")
        endif()

        file(REMOVE "${FFMPEG_ARCHIVE_PATH}")
        file(REMOVE_RECURSE "${FFMPEG_EXTRACT_DIR}")
        message(STATUS "FFmpeg setup complete!")
    endif()

endif()

# -------------------------------------------------------------
# Main Executable
# -------------------------------------------------------------
# Separate logic source files from main.cpp
set(PLAYER_LOGIC_SOURCES
    src/PlayerController.cpp
    src/Demuxer.cpp
    src/VideoDecoder.cpp
    src/AudioDecoder.cpp
)

set(WINDOWS_RESOURCE_FILE "")
if(TARGET_PLATFORM_WINDOWS)
    set(WINDOWS_RESOURCE_FILE src/resource.rc)
endif()

add_executable(NaikAVPlayer 
    src/main.cpp 
    src/PlayerUI.cpp
    ${PLAYER_LOGIC_SOURCES}
    ${WINDOWS_RESOURCE_FILE}
)

# Link libraries to our executable
target_link_libraries(NaikAVPlayer PRIVATE
    imgui
    SDL3::SDL3-shared
    nfd
)
if(FFMPEG_USE_SYSTEM)
    target_link_libraries(NaikAVPlayer PRIVATE PkgConfig::FFMPEG)
else()
    target_include_directories(NaikAVPlayer SYSTEM PRIVATE "${FFMPEG_DIR}/include")
    target_link_directories(NaikAVPlayer PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(NaikAVPlayer PRIVATE
        avcodec
        avformat
        avutil
        swscale
        swresample
    )
endif()

if(TARGET_PLATFORM_WINDOWS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(NaikAVPlayer PRIVATE -static-libgcc -static-libstdc++)
    set_target_properties(NaikAVPlayer PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Link Linux specific libraries
if(TARGET_PLATFORM_LINUX)
    target_link_libraries(NaikAVPlayer PRIVATE pthread dl)
endif()

# Apply compiler warning flags to main application target
target_compile_options(NaikAVPlayer PRIVATE ${PROJECT_WARNING_FLAGS})

# Helper function to configure target RPATH and copy runtime dependencies
function(setup_target_dependencies TARGET_NAME)
    if(TARGET_PLATFORM_WINDOWS)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${FFMPEG_DIR}/bin"
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying FFmpeg DLLs for ${TARGET_NAME}..."
        )

        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying SDL3.dll for ${TARGET_NAME}..."
        )
    elseif(TARGET_PLATFORM_LINUX)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_RPATH "$ORIGIN/lib"
            INSTALL_RPATH "$ORIGIN/lib"
        )
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TARGET_NAME}>/lib
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:${TARGET_NAME}>/lib
            COMMENT "Copying SDL3 .so to lib/ directory for ${TARGET_NAME}..."
        )
        if(NOT FFMPEG_USE_SYSTEM)
            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${FFMPEG_DIR}/lib"
                    $<TARGET_FILE_DIR:${TARGET_NAME}>/lib
                COMMENT "Copying FFmpeg .so files to lib/ directory for ${TARGET_NAME}..."
            )
        endif()
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/assets/fonts"
        COMMENT "Copying Noto Sans fonts for ${TARGET_NAME}..."
    )
endfunction()

setup_target_dependencies(NaikAVPlayer)

# -------------------------------------------------------------
# Test Target
# -------------------------------------------------------------
add_executable(NaikAVPlayer_tests 
    tests/tests.cpp
)

option(ENABLE_COVERAGE "Enable code coverage instrumentation" ON)

if(ENABLE_COVERAGE AND NOT ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(NaikAVPlayer_tests PRIVATE --coverage -O0 -g -U_FORTIFY_SOURCE)
        target_link_options(NaikAVPlayer_tests PRIVATE --coverage)
    elseif(MSVC)
        target_compile_options(NaikAVPlayer_tests PRIVATE /Zi /Od)
        target_link_options(NaikAVPlayer_tests PRIVATE /DEBUG)
    endif()
else()
    if(MSVC)
        target_compile_options(NaikAVPlayer_tests PRIVATE /Od)
    else()
        target_compile_options(NaikAVPlayer_tests PRIVATE -O0 -g -U_FORTIFY_SOURCE)
    endif()
endif()

target_link_libraries(NaikAVPlayer_tests PRIVATE
    SDL3::SDL3-shared
)
if(FFMPEG_USE_SYSTEM)
    target_link_libraries(NaikAVPlayer_tests PRIVATE PkgConfig::FFMPEG)
else()
    target_include_directories(NaikAVPlayer_tests SYSTEM PRIVATE "${FFMPEG_DIR}/include")
    target_link_directories(NaikAVPlayer_tests PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(NaikAVPlayer_tests PRIVATE
        avcodec
        avformat
        avutil
        swscale
        swresample
    )
endif()

if(TARGET_PLATFORM_LINUX)
    target_link_libraries(NaikAVPlayer_tests PRIVATE pthread dl)
endif()

if(TARGET_PLATFORM_WINDOWS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(NaikAVPlayer_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Apply compiler warning flags to test suite target
target_compile_options(NaikAVPlayer_tests PRIVATE ${PROJECT_WARNING_FLAGS})

setup_target_dependencies(NaikAVPlayer_tests)

# -------------------------------------------------------------
# Install Rules
# -------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS NaikAVPlayer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY assets/fonts/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/NaikAVPlayer/fonts
)

install(FILES assets/app_icon.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    RENAME naikavplayer.png
)

# Generate the .desktop launcher file at configure time so the repo doesn't
# need to carry a separate packaging/naikavplayer.desktop source file.
if(TARGET_PLATFORM_LINUX)
    set(DESKTOP_FILE "${CMAKE_BINARY_DIR}/naikavplayer.desktop")
    file(WRITE "${DESKTOP_FILE}"
"[Desktop Entry]
Type=Application
Name=NaikAVPlayer
Comment=Lightweight C++ media player
Exec=${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/NaikAVPlayer
Icon=naikavplayer
Terminal=false
Categories=AudioVideo;Player;
")

    install(FILES "${DESKTOP_FILE}"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
endif()

# Refresh desktop/icon caches after install so the launcher shows up
# immediately without a logout/login (best-effort; ignored if tools absent).
if(TARGET_PLATFORM_LINUX)
    install(CODE "
        find_program(UPDATE_DESKTOP_DB update-desktop-database)
        if(UPDATE_DESKTOP_DB)
            execute_process(COMMAND \${UPDATE_DESKTOP_DB} ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications)
        endif()
        find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
        if(GTK_UPDATE_ICON_CACHE)
            execute_process(COMMAND \${GTK_UPDATE_ICON_CACHE} -f ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor)
        endif()
    ")
endif()

# -------------------------------------------------------------
# Uninstall Target (Linux)
# -------------------------------------------------------------
# The uninstall script is generated inline at configure time (no external
# cmake_uninstall.cmake.in file needed). It reads install_manifest.txt,
# which CMake writes automatically after `cmake --install`, and removes
# exactly the files that were installed.
if(TARGET_PLATFORM_LINUX AND NOT TARGET uninstall)
    set(UNINSTALL_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")

    file(WRITE "${UNINSTALL_SCRIPT}"
"cmake_policy(SET CMP0007 NEW)

if(NOT EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt\")
    message(FATAL_ERROR \"Cannot find install manifest: ${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt\")
endif()

file(READ \"${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt\" files)
string(REGEX REPLACE \"\\n\" \";\" files \"\${files}\")
list(REVERSE files)

foreach(file \${files})
    message(STATUS \"Uninstalling: \$ENV{DESTDIR}\${file}\")
    if(IS_SYMLINK \"\$ENV{DESTDIR}\${file}\" OR EXISTS \"\$ENV{DESTDIR}\${file}\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" -E remove \"\$ENV{DESTDIR}\${file}\"
            RESULT_VARIABLE rm_retval
        )
        if(NOT \"\${rm_retval}\" STREQUAL 0)
            message(FATAL_ERROR \"Problem when removing \$ENV{DESTDIR}\${file}\")
        endif()
    else()
        message(STATUS \"File \$ENV{DESTDIR}\${file} does not exist.\")
    endif()
endforeach()
")

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${UNINSTALL_SCRIPT}"
        COMMENT "Uninstalling NaikAVPlayer..."
    )
endif()

include(CTest)

# Generate CTestCustom.cmake to prevent CTest from truncating test output logs in Test.xml
file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake" "
set(CTEST_CUSTOM_MAXIMUM_PASSED_TEST_OUTPUT_SIZE 262144)
set(CTEST_CUSTOM_MAXIMUM_FAILED_TEST_OUTPUT_SIZE 262144)
")

set(TEST_VIDEO_PATH "${CMAKE_SOURCE_DIR}/assets/hd_test_video_with_audio.mp4")

# Automatically generate a dummy test video using FFmpeg if it's not present
if(NOT EXISTS "${TEST_VIDEO_PATH}")
    if(CMAKE_HOST_WIN32)
        find_program(FFMPEG_EXE NAMES ffmpeg PATHS "${FFMPEG_DIR}/bin" NO_DEFAULT_PATH)
        if(NOT FFMPEG_EXE)
            find_program(FFMPEG_EXE ffmpeg)
        endif()
    else()
        find_program(FFMPEG_EXE ffmpeg)
    endif()

    if(FFMPEG_EXE)
        message(STATUS "Generating dummy test video for test suite: ${TEST_VIDEO_PATH}...")
        
        # Try libx264 (standard for GPL builds)
        execute_process(
            COMMAND ${FFMPEG_EXE} -y -f lavfi -i testsrc=duration=5:size=640x360:rate=30
                    -f lavfi -i sine=frequency=1000:duration=5 -c:v libx264 -c:a aac ${TEST_VIDEO_PATH}
            RESULT_VARIABLE FFMPEG_GEN_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )
        
        # Fallback to libopenh264 (standard for Cisco/LGPL builds)
        if(NOT FFMPEG_GEN_RESULT EQUAL 0)
            execute_process(
                COMMAND ${FFMPEG_EXE} -y -f lavfi -i testsrc=duration=5:size=640x360:rate=30
                        -f lavfi -i sine=frequency=1000:duration=5 -c:v libopenh264 -c:a aac ${TEST_VIDEO_PATH}
                RESULT_VARIABLE FFMPEG_GEN_RESULT
                OUTPUT_QUIET
                ERROR_QUIET
            )
        endif()

        # Fallback to h264_mf (Windows Media Foundation)
        if(NOT FFMPEG_GEN_RESULT EQUAL 0)
            execute_process(
                COMMAND ${FFMPEG_EXE} -y -f lavfi -i testsrc=duration=5:size=640x360:rate=30
                        -f lavfi -i sine=frequency=1000:duration=5 -c:v h264_mf -c:a aac ${TEST_VIDEO_PATH}
                RESULT_VARIABLE FFMPEG_GEN_RESULT
                OUTPUT_QUIET
                ERROR_QUIET
            )
        endif()

        if(NOT FFMPEG_GEN_RESULT EQUAL 0)
            message(WARNING "Failed to generate dummy test video. Unit tests might fail without test_video.mp4.")
        else()
            message(STATUS "Successfully generated dummy test video.")
        endif()
    else()
        message(WARNING "ffmpeg executable not found. Unable to automatically generate test_video.mp4.")
    endif()
endif()

add_test(NAME NaikAVPlayer_tests COMMAND NaikAVPlayer_tests "${TEST_VIDEO_PATH}")
