cmake_minimum_required(VERSION 3.20)
project(AssaultCube3DS VERSION 1.3.0.2 LANGUAGES C CXX ASM)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/arm-3ds.cmake")
endif()

include(FetchContent)
set(AC3DS_ASSET_ARCHIVE "" CACHE FILEPATH "Supplied AssaultCube 1.3.0.2 RC1 Linux release archive")
set(AC3DS_ICON "" CACHE FILEPATH "Optional 48x48 PNG for Nintendo 3DS application metadata")
option(AC3DS_EMBED_ASSETS "Embed release assets in 3DSX RomFS" ON)
option(AC3DS_PRODUCTION "Use release product version and master port" ON)
option(AC3DS_LTO "Enable link-time optimization for the native game" ON)
option(AC3DS_HEAPTRACK "Development: track heap blocks by caller (ac3dsheap command)" OFF)
option(AC3DS_FIRSTRUN "First-run download of the official game data when none is found (links mbedTLS and bzip2)" ON)
option(AC3DS_FIRSTRUN_TEST "Development: let a file on the SD card point the first-run download at a test server" OFF)
if(AC3DS_FIRSTRUN_TEST AND (AC3DS_PRODUCTION OR NOT AC3DS_FIRSTRUN))
    message(FATAL_ERROR "AC3DS_FIRSTRUN_TEST is for development builds only: it needs AC3DS_FIRSTRUN=ON and AC3DS_PRODUCTION=OFF")
endif()
if(AC3DS_EMBED_ASSETS AND NOT EXISTS "${AC3DS_ASSET_ARCHIVE}")
    message(FATAL_ERROR "Set AC3DS_ASSET_ARCHIVE to the supplied RC1 tar.bz2 or disable AC3DS_EMBED_ASSETS")
endif()

set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_SYSTEM_ICONV OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_LIBC ON CACHE BOOL "" FORCE)
# libctru handles quit through APT. A static try-compile can see newlib's
# sigaction declaration, but the Nintendo 3DS runtime does not provide it.
set(HAVE_SIGACTION OFF CACHE INTERNAL "No POSIX sigaction on Nintendo 3DS" FORCE)
# A 3DSX is a fixed-layout static link. SDL_image's option() would otherwise
# turn position-independent code on for every target; 3dsxtool cannot relocate
# GOT-relative references, so such a binary only runs when the loader happens
# to reproduce the linked text-to-data distance.
set(CMAKE_POSITION_INDEPENDENT_CODE OFF CACHE BOOL "3DSX is a fixed-layout static link: no PIC/PIE" FORCE)
FetchContent_Declare(sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG 5d249570393f7a37e037abf22cd6012a4cc56a71)
FetchContent_MakeAvailable(sdl2)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_INSTALL OFF CACHE BOOL "" FORCE)
foreach(format AVIF JXL TIF WEBP)
    set(SDL2IMAGE_${format} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(sdl2_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG c1bf2245b0ba63a25afe2f8574d305feca25af77
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(sdl2_image)

set(sdk_tool_paths "${DEVKITPRO}/tools/bin" "${DEVKITPRO}/portlibs/3ds/bin" "${DEVKITARM}/bin")
find_program(PICASSO picasso HINTS ${sdk_tool_paths} REQUIRED)
find_program(THREEDSXTOOL 3dsxtool HINTS ${sdk_tool_paths} REQUIRED)
foreach(lib citro3d ctru z vorbisidec ogg)
    find_library(ac3ds_lib_${lib} NAMES ${lib}
        HINTS "${DEVKITPRO}/libctru/lib" "${DEVKITPRO}/portlibs/3ds/lib" REQUIRED)
endforeach()

set(shader "${CMAKE_CURRENT_BINARY_DIR}/cube_vertex.shbin")
set(shader_asm "${CMAKE_CURRENT_BINARY_DIR}/cube_vertex.S")
# One DVLB with three programs: default, projection texture, startup probe.
set(shader_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/source/3ds/cube_vertex.v.pica"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/3ds/cube_projective.v.pica"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/3ds/probe_vertex.v.pica")
add_custom_command(OUTPUT "${shader}"
    COMMAND "${PICASSO}" -o "${shader}" ${shader_sources}
    DEPENDS ${shader_sources} VERBATIM)
file(GENERATE OUTPUT "${shader_asm}" CONTENT ".section .rodata\n.balign 4\n.global cube_vertex_shbin\n.global cube_vertex_shbin_size\ncube_vertex_shbin:\n.incbin \"${shader}\"\n.Lcube_vertex_shbin_end:\n.balign 4\ncube_vertex_shbin_size:\n.word .Lcube_vertex_shbin_end - cube_vertex_shbin\n")
set_source_files_properties("${shader_asm}" PROPERTIES OBJECT_DEPENDS "${shader}")

set(cube_units
    crypto audiomanager autodownload client clientgame clients2c command console docs
    editing entities http log main menus oggstream openal packetqueue physics protocol
    rendercubes rendergl renderhud rendermodel renderparticles rendertext rndmap scoreboard
    serverms server serverbrowser shadow soundlocation soundscheduler stream texture
    tools water weapon world worldio worldlight worldocull worldrender zip
    bot/bot bot/botmanager bot/bot_ai bot/bot_util bot/bot_waypoint bot/ac_bot bot/ac_bot_ai)
list(TRANSFORM cube_units PREPEND "source/src/")
list(TRANSFORM cube_units APPEND ".cpp")
set(enet_units callbacks compress host list packet peer protocol unix)
list(TRANSFORM enet_units PREPEND "source/enet/")
list(TRANSFORM enet_units APPEND ".c")
set(platform_units ac3ds_gl audio_3ds crash_3ds dedicated romfs_image smallpool_3ds system texprefetch_3ds)
if(AC3DS_HEAPTRACK)
    list(APPEND platform_units heaptrack_3ds)
endif()
# The first-run download speaks TLS 1.2 through mbedTLS (3ds-mbedtls in
# portlibs): the console's own TLS stops at 1.1, which GitHub refuses. The
# release is a .tar.bz2, unpacked with libbz2 (3ds-bzip2).
set(ac3ds_firstrun_libs "")
if(AC3DS_FIRSTRUN)
    list(APPEND platform_units firstrun_3ds firstrun_core https_3ds)
    foreach(lib mbedtls mbedx509 mbedcrypto bz2)
        find_library(ac3ds_lib_${lib} NAMES ${lib} HINTS "${DEVKITPRO}/portlibs/3ds/lib" REQUIRED)
        list(APPEND ac3ds_firstrun_libs "${ac3ds_lib_${lib}}")
    endforeach()
    # Written against the 2.28 interface; 3.x renamed parts of it.
    file(STRINGS "${DEVKITPRO}/portlibs/3ds/include/mbedtls/version.h" ac3ds_mbedtls_version
        REGEX "#define MBEDTLS_VERSION_STRING " LIMIT_COUNT 1)
    if(NOT ac3ds_mbedtls_version MATCHES "\"2\\.28\\.")
        message(FATAL_ERROR "AC3DS_FIRSTRUN needs mbedTLS 2.28 in portlibs (3ds-mbedtls); found: ${ac3ds_mbedtls_version}. "
            "Configure with -DAC3DS_FIRSTRUN=OFF to build without the first-run download.")
    endif()
endif()
list(TRANSFORM platform_units PREPEND "source/3ds/")
list(TRANSFORM platform_units APPEND ".cpp")
# Build number for the SD install: bumped in BUILD_NUMBER for every build put
# on the handheld. The installed file stays /3ds/assaultcube/assaultcube.3dsx
# (the Homebrew Launcher needs <folder>/<folder>.3dsx); the launcher shows the
# SMDH title: the release name in RELEASE_NAME, or AC3DSv<n> without one.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER" AC3DS_BUILD_NUMBER LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER")
set(AC3DS_LAUNCHER_TITLE "AC3DSv${AC3DS_BUILD_NUMBER}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NAME")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NAME" ac3ds_release_name LIMIT_COUNT 1)
    if(ac3ds_release_name)
        set(AC3DS_LAUNCHER_TITLE "${ac3ds_release_name}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NAME")
endif()
message(STATUS "AC3DS build number: ${AC3DS_BUILD_NUMBER}, launcher title: ${AC3DS_LAUNCHER_TITLE}")
add_executable(assaultcube ${cube_units} ${enet_units} ${platform_units} "${shader_asm}")
set_target_properties(assaultcube PROPERTIES SUFFIX ".elf" CXX_STANDARD 17 CXX_STANDARD_REQUIRED TRUE CXX_EXTENSIONS TRUE
    POSITION_INDEPENDENT_CODE OFF)
target_include_directories(assaultcube PRIVATE source/src source/src/bot source/enet/include source/3ds source/3ds/include
    "${DEVKITPRO}/portlibs/3ds/include" "${DEVKITPRO}/libctru/include")
target_compile_definitions(assaultcube PRIVATE $<$<BOOL:${AC3DS_PRODUCTION}>:PRODUCTION> AC3DS_BUILD_NUMBER=${AC3DS_BUILD_NUMBER} "AC3DS_RELEASE_NAME=\"${AC3DS_LAUNCHER_TITLE}\"")
target_compile_options(assaultcube PRIVATE -O3 -fsigned-char -ffunction-sections -fdata-sections -fno-strict-aliasing)
target_link_options(assaultcube PRIVATE -Wl,--gc-sections -Wl,--wrap=GX_ProcessCommandList -Wl,--wrap=gspSubmitGxCommand -Wl,--wrap=GX_BindQueue -Wl,--wrap=abort -Wl,--wrap=svcBreak "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/assaultcube.map")
# The small-block allocator (source/3ds/smallpool_3ds.cpp) sits in front of newlib's malloc family.
target_link_options(assaultcube PRIVATE -Wl,--wrap=_malloc_r -Wl,--wrap=_free_r -Wl,--wrap=_realloc_r
    -Wl,--wrap=_calloc_r -Wl,--wrap=_memalign_r)
if(AC3DS_FIRSTRUN)
    target_compile_definitions(assaultcube PRIVATE AC3DS_FIRSTRUN $<$<BOOL:${AC3DS_FIRSTRUN_TEST}>:AC3DS_FIRSTRUN_TEST>)
endif()
if(AC3DS_HEAPTRACK)
    target_compile_definitions(assaultcube PRIVATE AC3DS_HEAPTRACK)
    target_link_options(assaultcube PRIVATE -Wl,--wrap=_Znwj -Wl,--wrap=_Znaj)
endif()
if(AC3DS_LTO)
    target_compile_options(assaultcube PRIVATE -flto)
    target_link_options(assaultcube PRIVATE -flto=4)
endif()
target_link_libraries(assaultcube PRIVATE
    SDL2_image::SDL2_image-static SDL2::SDL2-static
    "${ac3ds_lib_vorbisidec}" "${ac3ds_lib_ogg}" "${ac3ds_lib_z}" ${ac3ds_firstrun_libs}
    "${ac3ds_lib_citro3d}" "${ac3ds_lib_ctru}" m)

# 3dsxtool silently drops GOT-relative relocations; fail the build instead.
find_program(AC3DS_READELF arm-none-eabi-readelf HINTS "${DEVKITARM}/bin" REQUIRED)
add_custom_command(TARGET assaultcube POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DREADELF=${AC3DS_READELF}" "-DELF=$<TARGET_FILE:assaultcube>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-relocs.cmake"
    VERBATIM)

set(romfs "${CMAKE_CURRENT_BINARY_DIR}/romfs")
set(icon "")
set(romfs_args "")
set(romfs_deps "")
set(smdh_args "")
set(smdh "")
if(AC3DS_EMBED_ASSETS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(stock_icon "${romfs}/packages/misc/icon.png")
    add_custom_command(OUTPUT "${romfs}/.assaultcube-assets"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stage-assets.py"
            "${AC3DS_ASSET_ARCHIVE}" "${romfs}" --stb-image "${sdl2_image_SOURCE_DIR}/src/stb_image.h"
        DEPENDS "${AC3DS_ASSET_ARCHIVE}" scripts/stage-assets.py VERBATIM)
    set(romfs_args "--romfs=${romfs}")
    set(romfs_deps "${romfs}/.assaultcube-assets")
endif()
if(AC3DS_ICON)
    get_filename_component(icon "${AC3DS_ICON}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${icon}" OR IS_DIRECTORY "${icon}")
        message(FATAL_ERROR "AC3DS_ICON must point to a 48x48 PNG")
    endif()
elseif(AC3DS_EMBED_ASSETS)
    set(icon "${CMAKE_CURRENT_BINARY_DIR}/assaultcube-icon.png")
    add_custom_command(OUTPUT "${icon}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/resize-icon.py" "${stock_icon}" "${icon}"
        DEPENDS ${romfs_deps} scripts/resize-icon.py VERBATIM)
else()
    # Engine-only builds carry no game art; use the port's own launcher icon.
    set(icon "${CMAKE_CURRENT_SOURCE_DIR}/source/3ds/icon.png")
endif()
set(package_config "${CMAKE_CURRENT_BINARY_DIR}/package-3dsx.options")
file(GENERATE OUTPUT "${package_config}" CONTENT "romfs=${romfs_args}\nicon=${icon}\n")
if(icon)
    find_program(SMDHTOOL smdhtool HINTS ${sdk_tool_paths} REQUIRED)
    set(smdh "${CMAKE_CURRENT_BINARY_DIR}/assaultcube.smdh")
    add_custom_command(OUTPUT "${smdh}"
        COMMAND "${SMDHTOOL}" --create "${AC3DS_LAUNCHER_TITLE}" "AssaultCube for New 3DS, build ${AC3DS_BUILD_NUMBER}" "rzkowskii" "${icon}" "${smdh}"
        DEPENDS "${icon}" "${package_config}" VERBATIM)
    set(smdh_args "--smdh=${smdh}")
endif()
set(three_dsx "${CMAKE_CURRENT_BINARY_DIR}/assaultcube.3dsx")
add_custom_command(OUTPUT "${three_dsx}"
    COMMAND "${THREEDSXTOOL}" "$<TARGET_FILE:assaultcube>" "${three_dsx}" ${smdh_args} ${romfs_args}
    DEPENDS assaultcube "${package_config}" ${smdh} ${romfs_deps} VERBATIM)
add_custom_target(package_3dsx ALL DEPENDS "${three_dsx}")
