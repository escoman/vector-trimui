cmake_minimum_required(VERSION 2.8.12)

PROJECT(v06x)

# TrimUI Brick Pro / CrossMix-OS standalone profile (aarch64).
option(TRIMUI "Build standalone v06x for TrimUI Brick Pro / CrossMix-OS (aarch64)" OFF)
if(TRIMUI)
    option(VECTOR06_ENABLE_GDB "Build the GDB stub server" OFF)
    option(VECTOR06_ENABLE_SCRIPTING "Build ChaiScript scripting support" OFF)
else()
    option(VECTOR06_ENABLE_GDB "Build the GDB stub server" ON)
    option(VECTOR06_ENABLE_SCRIPTING "Build ChaiScript scripting support" ON)
endif()

set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release Debug NoConfig)

message("CMAKE_SYSTEM_NAME is ${CMAKE_SYSTEM_NAME}")

add_definitions(-Wall)
add_definitions(-std=gnu++14)
add_definitions(-fpermissive)
add_definitions(-O3)
#add_definitions(-Rpass=loop-vectorize)

include_directories(fast-filters/sources)
include_directories(coreutil/sources)
include_directories(chaiscript/include)

add_definitions(-ffunction-sections -fdata-sections 
    -Wl,--gc-sections)

# about ChaiScript crash see 
# http://discourse.chaiscript.com/t/crash-when-using-chaiscript-from-plugins/89/3
set(EXTRA_DEFS CHAISCRIPT_NO_THREADS=1)

if(TRIMUI)
    message("Building for TrimUI Brick Pro / CrossMix-OS (aarch64)")
    # The codebase is C++17 (see common.mk); the generic -std=gnu++14 added
    # above is too old for e.g. std::clamp in sound.cpp. The later flag wins.
    add_definitions(-std=gnu++17)
    # Service GUI ported from the PSP build: overlay windows, Main Menu,
    # About and the on-screen keyboard, drawn above the picture by TV.
    # Guarded so the desktop Makefile/CMake builds stay GUI-free.
    add_definitions(-DVECTOR06_GUI=1)
    include_directories(gui)
    # No ARM32 flags and no -mcpu: rely on the aarch64 toolchain defaults.
    set(BFDNAME -O elf64-littleaarch64 -B aarch64)
    # The embedded objcopy blobs expose their size as an absolute symbol
    # whose *address* is taken (boots_bin_len = &_binary_boots_bin_size).
    # Under AArch64 PIE that address is relocated by the load base at run
    # time and turns into garbage, making boot.resize() throw std::bad_alloc.
    # Linking non-PIE keeps the absolute symbol value intact.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
    # Let SDL2 pull in only the runtime deps the target actually needs.
    set(EXTRA_LIBS ${EXTRA_LIBS} -lm -ldl -lpthread -lrt)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(EXTRA_LIBS -limm32 -lwinmm -lversion -lws2_32 -lmswsock)
    if (${MSYS})
        message("Windows+MSYS, assuming mingw32 i686 (32-bit) build")
        set(BFDNAME -O pe-i386 -B i386)
        set(EXTRA_DEFS "${EXTRA_DEFS}" MSYS_MINGW32=1)
        set(EXTRA_LIBS "${EXTRA_LIBS}" -lsetupapi -static-libgcc -static-libstdc++)
        add_definitions(-msse)
    else()
        message("Windows but not MSYS. Assuming 64-bit MinGW from https://nuwen.net/mingw.html")
        set(EXTRA_LIBS "${EXTRA_LIBS}" -lsetupapi)
        set(BFDNAME -O pe-x86-64 -B i386:x86-64)
    endif()
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    message("Must be a macOS, will use xxd")
    set(USE_XXD true)
    add_definitions(-ffast-math -msse2 -ffinite-math-only -ftree-vectorize)
else()
    if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
        message("Assuming this is Linux on Raspberry Pi")
        set(BFDNAME -O elf32-littlearm -B arm)
        add_definitions(-mfpu=neon-vfpv4 -mfloat-abi=hard -mcpu=cortex-a53 -funsafe-math-optimizations -ftree-vectorize -ftree-vectorizer-verbose=2 -fopt-info-vec)
    else()
        message("Looks like it's a desktop Linux on amd64, will use xxd")
        set(USE_XXD true) 

        # default ubuntu SDL comes with a load of deps
        set(EXTRA_LIBS ${EXTRA_LIBS} -Wl,--no-undefined -lm -ldl -lasound -lm -ldl -lpthread -lpulse-simple -lpulse -lsndio -lX11 -lXext -lXcursor -lXinerama -lXi -lXrandr -lXss -lXxf86vm -lwayland-egl -lwayland-client -lwayland-cursor -lxkbcommon -lpthread -lrt -lpng -ljpeg -ltiff -lwebp)
    endif()	
endif() 

set(SOURCE_FILES src/main.cpp src/memory.cpp src/tv.cpp src/board.cpp src/emulator.cpp
    src/hal.cpp src/i8080.cpp src/options.cpp src/sound.cpp src/filler.cpp
    src/resampler.cpp # src/biquad.cpp
    src/cadence.cpp
    src/icon.cpp
    src/fsimage.cpp
    src/glextns.cpp src/shaders.cpp
    src/util.cpp)
if(VECTOR06_ENABLE_SCRIPTING)
    list(APPEND SOURCE_FILES src/scriptnik.cpp)
endif()
if(VECTOR06_ENABLE_GDB)
    list(APPEND SOURCE_FILES src/server.cpp)
endif()

if(TRIMUI)
    # Service GUI (gui/) and its PNG/asset support, TRIMUI-only. The
    # desktop Makefile globs src/ and never sees gui/, so these stay out
    # of the non-TRIMUI builds (which also leave VECTOR06_GUI undefined).
    list(APPEND SOURCE_FILES
        gui/layer_draw.cpp
        gui/popup.cpp
        gui/mainmenu.cpp
        gui/about.cpp
        gui/vkbd.cpp
        gui/statewindow.cpp
        gui/rombrowser.cpp
        gui/mapwindow.cpp
        gui/message.cpp
        gui/gamecenter.cpp
        gui/netman.cpp
        src/imgload.cpp
        src/statefile.cpp
        src/filelist.cpp
        src/keymap.cpp
        src/stb_impl.cpp
        src/logo_data.cpp)
endif()

# OpenGL shader blobs are only referenced when HAVE_OPENGL is set.
set(EMBED_OBJS boots.o icon64.o)
if(NOT TRIMUI)
    list(APPEND EMBED_OBJS singlepass_vsh.o singlepass_fsh.o)
endif()

add_executable(v06x ${SOURCE_FILES} ${EMBED_OBJS})

target_compile_definitions(v06x PRIVATE ${EXTRA_DEFS})
if(VECTOR06_ENABLE_GDB)
    target_compile_definitions(v06x PRIVATE VECTOR06_ENABLE_GDB=1)
else()
    target_compile_definitions(v06x PRIVATE VECTOR06_ENABLE_GDB=0)
endif()
if(VECTOR06_ENABLE_SCRIPTING)
    target_compile_definitions(v06x PRIVATE VECTOR06_ENABLE_SCRIPTING=1)
else()
    target_compile_definitions(v06x PRIVATE VECTOR06_ENABLE_SCRIPTING=0)
endif()


if(TRIMUI)
    # Brick ships the required Boost shared libraries next to v06x (rpath $ORIGIN/lib)
    set(Boost_USE_STATIC_LIBS OFF)
else()
    set(Boost_USE_STATIC_LIBS ON)
endif()
find_package(Boost COMPONENTS thread system filesystem program_options chrono REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# prefer static linkage of SDL2_image, but on macos all hell breaks lose 
# so screw it
set(OLD_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(NOT (${CMAKE_SYSTEM_NAME} MATCHES "Darwin") AND NOT TRIMUI)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()

message("Library suffixes are: ${CMAKE_FIND_LIBRARY_SUFFIXES}")
find_package(SDL2 REQUIRED)
message("SDL2 libs are: ${SDL2_LIBRARY}")

if(NOT TRIMUI)
    find_package(SDL2_image)
endif()
set(CMAKE_FIND_LIBRARY_SUFFIXES ${OLD_CMAKE_FIND_LIBRARY_SUFFIXES})

if (${SDL2_image_FOUND}) 
    add_definitions(-DHAS_IMAGE=1)
else()
    message("SDL2_image not found, saving frames will not work")
    set(SDL2_IMAGE_INCLUDE_DIRS "")
    set(SDL2_IMAGE_LIBRARIES "")
endif()
include_directories(${SDL2_INCLUDE_DIR} ${SDL2_IMAGE_INCLUDE_DIRS})

if(NOT TRIMUI)
    find_package(OpenGL)
    message("OPENGL_FOUND: ${OPENGL_FOUND}")
    if (${OPENGL_FOUND})
        add_definitions(-DHAVE_OPENGL=1)
    endif()
endif()

target_link_libraries(v06x ${SDL2_LIBRARY} ${SDL2_IMAGE_LIBRARIES} ${Boost_LIBRARIES} ${OPENGL_LIBRARIES})

if(NOT TRIMUI)
    get_directory_property(include_dirs INCLUDE_DIRECTORIES)
    find_path(PERFTOOLS_H gperftools/profiler.h ${include_dirs})
    if (PERFTOOLS_H)
        message("Found gperftools/profiler.h")
        add_definitions(-DHAVE_GPERFTOOLS=1)
        set(EXTRA_LIBS ${EXTRA_LIBS} -lprofiler)
    endif()
endif()

# On Linux and Windows we have objcopy...
if(TRIMUI)
    find_program(OBJCOPY aarch64-linux-gnu-objcopy)
else()
    find_program(OBJCOPY objcopy)
endif()

macro(BIN2OBJ infile basename)
    if(USE_XXD OR (${OBJCOPY} STREQUAL "OBJCOPY-NOTFOUND")) 
        # ...but Darwin is retarded so we must make do with something else
        add_custom_command(OUTPUT ${basename}.c COMMAND xxd -i ${infile} ${basename}.c
            DEPENDS ${infile})
        add_custom_command(OUTPUT ${basename}.o COMMAND cc -c ${basename}.c DEPENDS ${basename}.c)
        add_definitions(-DUSED_XXD=1)
    else()
        add_custom_command(
            OUTPUT ${basename}.o
            COMMAND ${OBJCOPY} -I binary ${BFDNAME} ${infile} ${basename}.o
            DEPENDS ${infile}
            )
    endif()
endmacro()

add_custom_command(OUTPUT boots.bin
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/boot/boots.bin ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/boot/boots.bin)

add_custom_command(OUTPUT singlepass.vsh
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/shaders/singlepass.vsh ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/singlepass.vsh )

add_custom_command(OUTPUT singlepass.fsh
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/shaders/singlepass.fsh ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/singlepass.fsh)

add_custom_command(OUTPUT icon64.rgba
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/res/icon64.rgba ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/icon64.rgba)

    

BIN2OBJ(boots.bin boots)
BIN2OBJ(singlepass.vsh singlepass_vsh)
BIN2OBJ(singlepass.fsh singlepass_fsh)
BIN2OBJ(icon64.rgba icon64)

message("objcopy is ${OBJCOPY}")

target_link_libraries(v06x ${BOOT_O} ${EXTRA_LIBS})

if(TRIMUI)
    # Bundle Boost/SDL deps next to the binary in the SDCARD package.
    set_target_properties(v06x PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
endif()

include_directories(src)
if(NOT TRIMUI)
    set(TEST_SOURCE_FILES test/tests.cpp)
    add_executable(tests ${TEST_SOURCE_FILES} boots.o)
endif()

