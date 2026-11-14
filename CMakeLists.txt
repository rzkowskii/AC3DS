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
set(platform_units ac3ds_gl audio_3ds crash_3ds dedicated system texprefetch_3ds)
list(TRANSFORM platform_units PREPEND "source/3ds/")
list(TRANSFORM platform_units APPEND ".cpp")
add_executable(assaultcube ${cube_units} ${enet_units} ${platform_units} "${shader_asm}")
set_target_properties(assaultcube PROPERTIES SUFFIX ".elf" CXX_STANDARD 17 CXX_STANDARD_REQUIRED TRUE CXX_EXTENSIONS TRUE)
target_include_directories(assaultcube PRIVATE source/src source/src/bot source/enet/include source/3ds source/3ds/include
    "${DEVKITPRO}/portlibs/3ds/include" "${DEVKITPRO}/libctru/include")
target_compile_definitions(assaultcube PRIVATE $<$<BOOL:${AC3DS_PRODUCTION}>:PRODUCTION>)
target_compile_options(assaultcube PRIVATE -O3 -fsigned-char -ffunction-sections -fdata-sections -fno-strict-aliasing)
target_link_options(assaultcube PRIVATE -Wl,--gc-sections -Wl,--wrap=GX_ProcessCommandList -Wl,--wrap=gspSubmitGxCommand "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/assaultcube.map")
if(AC3DS_LTO)
    target_compile_options(assaultcube PRIVATE -flto)
    target_link_options(assaultcube PRIVATE -flto=4)
endif()
target_link_libraries(assaultcube PRIVATE
    SDL2_image::SDL2_image-static SDL2::SDL2-static
    "${ac3ds_lib_vorbisidec}" "${ac3ds_lib_ogg}" "${ac3ds_lib_z}"
    "${ac3ds_lib_citro3d}" "${ac3ds_lib_ctru}" m)

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
            "${AC3DS_ASSET_ARCHIVE}" "${romfs}"
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
elseif(EXISTS "${DEVKITPRO}/libctru/default_icon.png")
    set(icon "${DEVKITPRO}/libctru/default_icon.png")
endif()
set(package_config "${CMAKE_CURRENT_BINARY_DIR}/package-3dsx.options")
file(GENERATE OUTPUT "${package_config}" CONTENT "romfs=${romfs_args}\nicon=${icon}\n")
if(icon)
    find_program(SMDHTOOL smdhtool HINTS ${sdk_tool_paths} REQUIRED)
    set(smdh "${CMAKE_CURRENT_BINARY_DIR}/assaultcube.smdh")
    add_custom_command(OUTPUT "${smdh}"
        COMMAND "${SMDHTOOL}" --create "AssaultCube" "Lockdown Edition for New Nintendo 3DS" "AssaultCube Team" "${icon}" "${smdh}"
        DEPENDS "${icon}" "${package_config}" VERBATIM)
    set(smdh_args "--smdh=${smdh}")
endif()
set(three_dsx "${CMAKE_CURRENT_BINARY_DIR}/assaultcube.3dsx")
add_custom_command(OUTPUT "${three_dsx}"
    COMMAND "${THREEDSXTOOL}" "$<TARGET_FILE:assaultcube>" "${three_dsx}" ${smdh_args} ${romfs_args}
    DEPENDS assaultcube "${package_config}" ${smdh} ${romfs_deps} VERBATIM)
add_custom_target(package_3dsx ALL DEPENDS "${three_dsx}")
