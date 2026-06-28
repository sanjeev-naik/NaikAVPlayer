cmake_minimum_required(VERSION 3.16)
project(NaikAVPlayer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------
# Compiler & Linker Hardening Options
# -------------------------------------------------------------
if(MSVC)
    add_compile_options(/GS /sdl /W3)
    add_link_options(/NXCOMPAT /DYNAMICBASE)
else()
    # GCC/MinGW hardening flags
    add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security)
    add_link_options(-Wl,--dynamicbase -Wl,--nxcompat)
endif()

# -------------------------------------------------------------
# Fetch SDL2
# -------------------------------------------------------------
include(FetchContent)

# Optimize SDL2 build time: build shared only, disable tests/docs
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

message(STATUS "Fetching SDL2...")
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.3
)
FetchContent_MakeAvailable(SDL2)

# -------------------------------------------------------------
# Fetch Dear ImGui
# -------------------------------------------------------------
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Create a static library for ImGui
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

# ImGui needs SDL headers
target_link_libraries(imgui PRIVATE SDL2::SDL2)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# -------------------------------------------------------------
# Configure FFmpeg (Download automatically if missing)
# -------------------------------------------------------------
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")

if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h" OR NOT EXISTS "${FFMPEG_DIR}/lib/avcodec.lib")
    message(STATUS "FFmpeg headers/libraries not found in thirdparty/ffmpeg. Setting up automatically...")
    
    set(FFMPEG_ZIP_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip")
    set(FFMPEG_ZIP_PATH "${CMAKE_BINARY_DIR}/ffmpeg_download.zip")
    set(FFMPEG_EXTRACT_DIR "${CMAKE_BINARY_DIR}/ffmpeg_extracted")

    message(STATUS "Downloading FFmpeg shared binaries from ${FFMPEG_ZIP_URL}...")
    file(DOWNLOAD "${FFMPEG_ZIP_URL}" "${FFMPEG_ZIP_PATH}" 
         SHOW_PROGRESS 
         STATUS download_status)

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download FFmpeg (Code: ${status_code}): ${status_msg}")
    endif()

    message(STATUS "Extracting FFmpeg package...")
    file(ARCHIVE_EXTRACT INPUT "${FFMPEG_ZIP_PATH}" DESTINATION "${FFMPEG_EXTRACT_DIR}")

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
    
    file(COPY "${FFMPEG_SRC_DIR}/bin" DESTINATION "${FFMPEG_DIR}")
    file(COPY "${FFMPEG_SRC_DIR}/include" DESTINATION "${FFMPEG_DIR}")
    file(COPY "${FFMPEG_SRC_DIR}/lib" DESTINATION "${FFMPEG_DIR}")
    
    # Copy LICENSE and README if present
    if(EXISTS "${FFMPEG_SRC_DIR}/LICENSE.txt")
        file(COPY "${FFMPEG_SRC_DIR}/LICENSE.txt" DESTINATION "${FFMPEG_DIR}")
    endif()
    if(EXISTS "${FFMPEG_SRC_DIR}/README.txt")
        file(COPY "${FFMPEG_SRC_DIR}/README.txt" DESTINATION "${FFMPEG_DIR}")
    endif()

    file(REMOVE "${FFMPEG_ZIP_PATH}")
    file(REMOVE_RECURSE "${FFMPEG_EXTRACT_DIR}")
    message(STATUS "FFmpeg setup complete!")
endif()

include_directories(SYSTEM "${FFMPEG_DIR}/include")
link_directories("${FFMPEG_DIR}/lib")

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

add_executable(NaikAVPlayer 
    src/main.cpp 
    src/PlayerUI.cpp
    ${PLAYER_LOGIC_SOURCES}
)

# Link libraries to our executable
target_link_libraries(NaikAVPlayer PRIVATE
    imgui
    SDL2::SDL2
    avcodec
    avformat
    avutil
    swscale
    swresample
    comdlg32
)

# Ensure standard Win32 subsystems link correctly
target_link_libraries(NaikAVPlayer PRIVATE mingw32)

# Copy FFmpeg and SDL DLLs to executable output directory so it runs smoothly
add_custom_command(TARGET NaikAVPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:NaikAVPlayer>
    COMMENT "Copying FFmpeg DLLs to output directory..."
)

add_custom_command(TARGET NaikAVPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:NaikAVPlayer>
    COMMENT "Copying SDL2.dll to output directory..."
)

# -------------------------------------------------------------
# Test Target with Code Coverage
# -------------------------------------------------------------
add_executable(NaikAVPlayer_tests 
    tests/tests.cpp
)

# Enable GCC code coverage flags for tests
target_compile_options(NaikAVPlayer_tests PRIVATE --coverage -O0 -g)
target_link_options(NaikAVPlayer_tests PRIVATE --coverage)

target_link_libraries(NaikAVPlayer_tests PRIVATE
    SDL2::SDL2
    avcodec
    avformat
    avutil
    swscale
    swresample
)

# Copy FFmpeg and SDL DLLs to tests output directory
add_custom_command(TARGET NaikAVPlayer_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:NaikAVPlayer_tests>
    COMMENT "Copying FFmpeg DLLs to tests output directory..."
)

add_custom_command(TARGET NaikAVPlayer_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:NaikAVPlayer_tests>
    COMMENT "Copying SDL2.dll to tests output directory..."
)
