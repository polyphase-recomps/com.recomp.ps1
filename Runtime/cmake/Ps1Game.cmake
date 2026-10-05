# ps1_add_game(): builds a 100%-decompiled PS1 game as a native 32-bit Windows program
# on top of the com.recomp.ps1 runtime (PsyQ replacement + Windows host).
#
# The program is linked at 0x80800000 and maps PS1 main RAM at 0x80000000 and the
# scratchpad at 0x1F800000, so PS1 addresses used by the game (fixed buffers, 24-bit
# ordering-table links, overlay load addresses) are valid as they are. This needs a
# 32-bit, large-address-aware, fixed-base executable: configure from an x86 Visual
# Studio environment with clang (Runtime/tools/build_game.ps1 does this).
#
#   ps1_add_game(
#       NAME dw                              # executable name (dw.exe)
#       TITLE "Digimon World"                # window title
#       DECOMP <dir>                         # the decomp checkout (splat layout: config/<ver>, src, include)
#       VERSION us                           # config/<VERSION>/ and the version define
#       DEFINES LANGUAGE_C VERSION_US        # game compile definitions
#       INCLUDES <dir>...                    # game include dirs (after the runtime's own)
#       SOURCES_SCRIPT <py>                  # prints the game's C sources, one per line (args: decomp version)
#       EXCLUDE_REGEX "/_psstart[.]c$"       # sources replaced by the runtime
#       BOOT_EXE SLUS_010.32                 # executable on the disc
#       BSS_END 0x80090C68                   # end of the executable's BSS, when its header has no BSS size
#       RAM_SIZE 0x400000                    # PS1 RAM the game may use (default 2 MB, at most 4 MB): more
#                                            # than the console's gives a game's heap room (optional)
#                                            # (the startup code clears it and starts the heap there; optional)
#       DISC <path.bin>                      # default disc image (absolute or relative to the game package)
#       MOVIES MOVIE/OP1.STR ...             # STR files on the disc, by movie id (startMovie ids)
#       MOVIE_NOSKIP 3                       # movie ids Start cannot skip
#       OVERLAYS btl std ...                 # overlay source dirs (src/<ovl>), in the game's Overlay enum order (1-based)
#       PATCHES <file.patch>...              # unified diffs against the decomp, applied to copies
#       EXTRA_SOURCES <c>...                 # game-specific port code (test hooks...)
#       PUBLISH_DIR <dir>                    # where the built exe is copied (optional; usually <pkg>/Assets/Bin)
#       EXTRACT_TO <dir>)                    # extracted disc for packaged games and mods (optional; usually <pkg>/Assets/Disc)
cmake_minimum_required(VERSION 3.20)

set(PS1_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "")
set(PS1_GUEST "native" CACHE STRING "How the game code is built: native (32-bit x86, Windows) or wasm (wasm2c, any host)")
set_property(CACHE PS1_GUEST PROPERTY STRINGS native wasm)
get_filename_component(PS1_RUNTIME_DIR "${PS1_RUNTIME_DIR}" ABSOLUTE)

function(_ps1_run_map script outvar)
    # Runs a "original|generated" mapping tool and rewrites the source list.
    execute_process(COMMAND ${script} OUTPUT_VARIABLE map RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${script} failed")
    endif()
    string(STRIP "${map}" map)
    if(map STREQUAL "")
        return()
    endif()
    string(REPLACE "\n" ";" map "${map}")
    set(list ${${outvar}})
    foreach(pair IN LISTS map)
        string(REPLACE "|" ";" pair_list "${pair}")
        list(GET pair_list 0 original)
        list(GET pair_list 1 generated)
        list(TRANSFORM list REPLACE "^${original}$" "${generated}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    endforeach()
    set(${outvar} ${list} PARENT_SCOPE)
endfunction()

function(ps1_add_game)
    cmake_parse_arguments(G "" "NAME;TITLE;DECOMP;VERSION;SOURCES_SCRIPT;EXCLUDE_REGEX;BOOT_EXE;BSS_END;RAM_SIZE;DISC;PUBLISH_DIR;PACKAGE;EXTRACT_TO"
        "DEFINES;INCLUDES;MOVIES;MOVIE_NOSKIP;OVERLAYS;PATCHES;EXTRA_SOURCES" ${ARGN})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(G_DECOMP "${G_DECOMP}" ABSOLUTE)
    set(PORT_DIR "${PS1_RUNTIME_DIR}/port")
    set(TOOLS "${PS1_RUNTIME_DIR}/tools")
    set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
    file(MAKE_DIRECTORY "${GEN_DIR}")
    set(PY "${Python3_EXECUTABLE}")

    # ---- game sources -------------------------------------------------------------
    execute_process(COMMAND "${PY}" "${G_SOURCES_SCRIPT}" "${G_DECOMP}" "${G_VERSION}"
        OUTPUT_VARIABLE raw RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${G_SOURCES_SCRIPT} failed")
    endif()
    string(STRIP "${raw}" raw)
    string(REPLACE "\n" ";" GAME_SOURCES "${raw}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${G_SOURCES_SCRIPT}")
    if(G_EXCLUDE_REGEX)
        list(FILTER GAME_SOURCES EXCLUDE REGEX "${G_EXCLUDE_REGEX}")
    endif()

    # Game fixes: applied to copies of the decomp files (the checkout stays untouched).
    if(G_PATCHES)
        _ps1_run_map("${PY};${TOOLS}/apply_patches.py;${G_DECOMP};${GEN_DIR}/patched;${G_PATCHES};${GAME_SOURCES}"
            GAME_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${G_PATCHES})
    endif()
    # Japanese text: the original build encodes literals as CP932, which clang cannot do,
    # so sources with non-ASCII literals are compiled from escaped copies.
    _ps1_run_map("${PY};${TOOLS}/sjis_sources.py;${G_DECOMP};${GEN_DIR}/sjis;${GAME_SOURCES}" GAME_SOURCES)

    # Symbols the original link provided by address (overlay bounds, data the decomp
    # leaves to the linker), from the decomp's symbol files.
    if(NOT G_RAM_SIZE)
        set(G_RAM_SIZE 0x200000)
    endif()
    execute_process(COMMAND "${PY}" "${TOOLS}/gen_symbols.py" "${G_DECOMP}" "${G_VERSION}" "${GEN_DIR}"
        "--ram-size=${G_RAM_SIZE}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "gen_symbols.py failed")
    endif()

    # ---- generated game configuration (boot exe, movies, overlays, disc) -------------
    # Overlay data: on the PS1 an overlay's globals come back to their initial values
    # every time its file is loaded. tools/ovl_launcher.py moves each overlay object's
    # data into its own section group (.ovNN$d between .ovNN$a/.ovNN$z markers) so the
    # runtime can snapshot it at boot and restore it (port/guest/overlays.c).
    # (wasm: the data goes to segment ovNN, bounded by wasm-ld's __start_ovNN/__stop_ovNN.)
    list(LENGTH G_OVERLAYS ovl_count)
    set(markers "/* Generated by Ps1Game.cmake - overlay data section markers. */\n")
    set(wmarkers "/* Generated by Ps1Game.cmake - overlay data segments (wasm). */\n")
    set(begins "")
    set(ends "")
    set(wbegins "")
    set(wends "")
    set(index 1)
    foreach(ovl IN LISTS G_OVERLAYS)
        if(index LESS 10)
            set(sec "ov0${index}")
        else()
            set(sec "ov${index}")
        endif()
        string(APPEND markers "__attribute__((section(\".${sec}$a\"))) char port_ovl_begin_${index}[4] = {1};\n")
        string(APPEND markers "__attribute__((section(\".${sec}$z\"))) char port_ovl_end_${index}[4] = {1};\n")
        # the data starts after the 4-byte begin marker
        string(APPEND begins "port_ovl_begin_${index} + 4, ")
        string(APPEND ends "port_ovl_end_${index}, ")
        string(APPEND wmarkers "extern char __start_${sec}[] __attribute__((weak));\n")
        string(APPEND wmarkers "extern char __stop_${sec}[] __attribute__((weak));\n")
        string(APPEND wbegins "__start_${sec}, ")
        string(APPEND wends "__stop_${sec}, ")
        math(EXPR index "${index} + 1")
    endforeach()
    foreach(var markers wmarkers)
        if(var STREQUAL "markers")
            set(b "${begins}")
            set(e "${ends}")
            set(file "ovl_markers.c")
        else()
            set(b "${wbegins}")
            set(e "${wends}")
            set(file "ovl_markers_wasm.c")
        endif()
        set(text "${${var}}")
        string(APPEND text "char *const port_ovl_begins[] = { ${b}0 };\n")
        string(APPEND text "char *const port_ovl_ends[] = { ${e}0 };\n")
        string(APPEND text "const int port_ovl_count = ${ovl_count};\n")
        file(WRITE "${GEN_DIR}/${file}.new" "${text}")
        file(COPY_FILE "${GEN_DIR}/${file}.new" "${GEN_DIR}/${file}" ONLY_IF_DIFFERENT)
    endforeach()
    string(REPLACE ";" "," ovl_csv "${G_OVERLAYS}")

    if(G_DISC)
        get_filename_component(disc "${G_DISC}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        set(disc "")
    endif()
    get_filename_component(disc_name "${disc}" NAME)
    set(movies "")
    foreach(m IN LISTS G_MOVIES)
        string(APPEND movies "\"/${m};1\", ")
    endforeach()
    list(LENGTH G_MOVIES movie_count)
    set(noskip 0)
    foreach(id IN LISTS G_MOVIE_NOSKIP)
        math(EXPR noskip "${noskip} | (1 << ${id})")
    endforeach()
    string(REPLACE "/" "\\\\" disc_c "${disc}")
    string(REPLACE "/" "\\\\" extract_c "${G_EXTRACT_TO}")
    if(NOT G_BSS_END)
        set(G_BSS_END 0)
    endif()
    set(cfg "/* Generated by Ps1Game.cmake for ${G_NAME} - do not edit. */
#ifndef PS1_GAME_CONFIG_H
#define PS1_GAME_CONFIG_H
#define PS1_GAME_TITLE \"${G_TITLE}\"
#define PS1_BOOT_EXE \"/${G_BOOT_EXE};1\"
#define PS1_BOOT_EXE_NAME \"${G_BOOT_EXE}\"
#define PS1_BSS_END ${G_BSS_END}u
#define PS1_RAM_SIZE ${G_RAM_SIZE}u
#define PS1_DEFAULT_DISC \"${disc_c}\"
#define PS1_DEFAULT_DISC_NAME \"${disc_name}\"
#define PS1_EXTRACTED_DISC \"${extract_c}\"
#define PS1_MOVIE_COUNT ${movie_count}
#define PS1_MOVIES { ${movies}0 }
#define PS1_MOVIE_NOSKIP_MASK ${noskip}u
#endif
")
    file(WRITE "${GEN_DIR}/ps1_game_config.h.new" "${cfg}")
    file(COPY_FILE "${GEN_DIR}/ps1_game_config.h.new" "${GEN_DIR}/ps1_game_config.h" ONLY_IF_DIFFERENT)

    # Packaged games read the disc as extracted files (mods replace them): the editor
    # ships files under a package's Assets/ as they are, and Ps1Player looks there.
    # Extracted again when the image changes; files already there (mods) are kept.
    if(G_EXTRACT_TO AND disc AND EXISTS "${disc}")
        add_custom_command(OUTPUT "${G_EXTRACT_TO}/disc.idx"
            COMMAND "${PY}" "${TOOLS}/extract_disc.py" "${disc}" "${G_EXTRACT_TO}"
            DEPENDS "${disc}" "${TOOLS}/extract_disc.py"
            COMMENT "Extracting the disc to ${G_EXTRACT_TO}"
            VERBATIM)
        add_custom_target(${G_NAME}_disc ALL DEPENDS "${G_EXTRACT_TO}/disc.idx")
    endif()

    # ---- targets ----------------------------------------------------------------------
    file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS "${PORT_DIR}/guest/*.c")
    if(PS1_GUEST STREQUAL "wasm")
        _ps1_wasm_game()
        return()
    endif()
    file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${PORT_DIR}/host/win32/*.c" "${PORT_DIR}/host/native/*.c")
    list(APPEND HOST_SOURCES "${PS1_RUNTIME_DIR}/../Source/Wasm/ps1w_disc.c")

    set(game "${G_NAME}_game")
    add_library(${game} OBJECT ${GAME_SOURCES} ${GUEST_SOURCES} ${G_EXTRA_SOURCES}
        "${GEN_DIR}/ps1_symbols.c" "${GEN_DIR}/ovl_markers.c")
    target_compile_definitions(${game} PRIVATE ${G_DEFINES} PORT=1 main=ps1_game_main)
    target_include_directories(${game} PRIVATE "${PORT_DIR}/include" "${GEN_DIR}" ${G_INCLUDES})
    # The GNU target gives GCC struct layout for bitfields (PsyQ GsOT_TAG and friends are
    # 4 bytes on the PS1; MSVC layout makes them 8). No stack probes: the game stack is
    # committed memory in the PS1 window. NULL is a valid address on the PS1 (RAM mirror)
    # and games read through it; keep those accesses (the fault handler serves them)
    # instead of letting the optimiser treat them as unreachable.
    # Decompiled code passes uninitialised locals around (matching code: the callee
    # ignores them); with clang's noundef argument attributes that is undefined
    # behaviour and whole functions get optimised away, so the attributes are off.
    target_compile_options(${game} PRIVATE --target=i686-pc-windows-gnu -mno-ms-bitfields
        "SHELL:-Xclang -no-enable-noundef-analysis"
        -mno-stack-arg-probe -fno-delete-null-pointer-checks -std=gnu99 -ffreestanding -nostdlibinc
        -fno-builtin -fno-strict-aliasing -fwrapv -funsigned-char -Wno-everything -g -gcodeview
        -fno-omit-frame-pointer "SHELL:-include \"${PORT_DIR}/include/port_prelude.h\"")
    set_target_properties(${game} PROPERTIES C_COMPILER_LAUNCHER
        "${PY};${TOOLS}/ovl_launcher.py;--overlays=${ovl_csv}")
    option(PS1_GAME_OPTIMIZE "Optimise the game code" ON)
    if(PS1_GAME_OPTIMIZE)
        target_compile_options(${game} PRIVATE -O2)
    else()
        target_compile_options(${game} PRIVATE -O0)
    endif()

    # Host side: Windows APIs, no PsyQ headers.
    set(host "${G_NAME}_host")
    add_library(${host} OBJECT ${HOST_SOURCES})
    target_include_directories(${host} PRIVATE "${PORT_DIR}/include" "${GEN_DIR}")
    target_compile_options(${host} PRIVATE -O2 -g -gcodeview -Wall -Wno-unused-function)

    add_executable(${G_NAME} $<TARGET_OBJECTS:${game}> $<TARGET_OBJECTS:${host}>)
    target_link_libraries(${G_NAME} PRIVATE kernel32 user32 gdi32 winmm dbghelp xinput)
    target_link_options(${G_NAME} PRIVATE -fuse-ld=lld -Wl,/BASE:0x80800000 -Wl,/FIXED
        -Wl,/DYNAMICBASE:NO -Wl,/LARGEADDRESSAWARE -Wl,/STACK:0x800000 -Wl,/debug -Wl,/errorlimit:0
        # The PS1 has no memory protection and games write to some of their const data.
        "SHELL:-Xlinker /SECTION:.rdata,RW")

    _ps1_publish(${G_NAME})
endfunction()

# Copies the program to PUBLISH_DIR (normally the game package's Assets/Bin, which the
# editor ships as raw assets) with a .meta sidecar that limits it to Windows packages.
# The .pdb stays in the build folder.
function(_ps1_publish target)
    if(G_PUBLISH_DIR)
        set(meta "${CMAKE_BINARY_DIR}/${target}.exe.meta")
        file(WRITE "${meta}" "{\"platforms\": [\"Windows\"]}\n")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${G_PUBLISH_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>" "${G_PUBLISH_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${meta}" "${G_PUBLISH_DIR}"
            VERBATIM)
    endif()
endfunction()

# Finds <Tools>/<pattern> (newest match) in a folder named Tools next to one of the
# parent folders of the game package, unless the cache variable is set.
function(_ps1_find_tool var pattern)
    if(${var} AND EXISTS "${${var}}")
        return()
    endif()
    set(dir "${CMAKE_SOURCE_DIR}")
    foreach(i RANGE 10)
        file(GLOB found LIST_DIRECTORIES true "${dir}/Tools/${pattern}")
        if(found)
            list(SORT found)
            list(GET found -1 found)
            set(${var} "${found}" CACHE PATH "${pattern}" FORCE)
            return()
        endif()
        get_filename_component(dir "${dir}" DIRECTORY)
    endforeach()
    message(FATAL_ERROR "${pattern} not found: set ${var}")
endfunction()

# ---- wasm guest -------------------------------------------------------------------------
# The game and the PsyQ replacement are compiled to WebAssembly (wasi-sdk), linked into
# one module and translated to C by wasm2c (WABT); the host program then compiles that
# C with the host compiler. Nothing in the result depends on the host's address space,
# word size or byte order (port/host/wasm/ps1w.h), so the same guest runs on 64-bit PCs
# and big-endian consoles. Runs in ps1_add_game's scope (its variables are visible).
macro(_ps1_wasm_game)
    _ps1_find_tool(PS1_WASI_SDK "wasi-sdk-*")
    _ps1_find_tool(PS1_WABT "wabt-*")
    set(WASM_DIR "${CMAKE_BINARY_DIR}/wasm")
    # host tools (CMAKE_EXECUTABLE_SUFFIX is the target's: .elf for the consoles)
    set(WASM_CC "${PS1_WASI_SDK}/bin/clang${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(WASM_LD "${PS1_WASI_SDK}/bin/wasm-ld${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(WASI_LIB "${PS1_WASI_SDK}/share/wasi-sysroot/lib/wasm32-wasip1")
    file(GLOB WASM_RT_BUILTINS "${PS1_WASI_SDK}/lib/clang/*/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a")
    set(WASM2C "${PS1_WABT}/bin/wasm2c${CMAKE_HOST_EXECUTABLE_SUFFIX}")

    # Same language settings as the native game build (GCC struct layout is the wasm32
    # default). setjmp/longjmp (some game code uses them) need wasm exception handling.
    # tools/wasm_cc.py gives globals the MIPS GCC alignment and puts overlay data in the
    # ovNN segments.
    set(cflags --target=wasm32 -O2 -mllvm -wasm-enable-sjlj -mexception-handling
        -Xclang -no-enable-noundef-analysis -fno-delete-null-pointer-checks -std=gnu99 -ffreestanding -nostdlibinc -fno-builtin
        -fno-strict-aliasing -fwrapv -funsigned-char -Wno-everything
        -include "${PORT_DIR}/include/port_prelude.h" -I "${PORT_DIR}/include" -I "${GEN_DIR}")
    # PORT_HOST_GPU: the GPU (port/guest/gpu.c) runs in the host (Source/Wasm/ps1w_gpu.c)
    foreach(d IN LISTS G_DEFINES ITEMS PORT=1 main=ps1_game_main PORT_HOST_GPU=1)
        list(APPEND cflags "-D${d}")
    endforeach()
    foreach(i IN LISTS G_INCLUDES)
        list(APPEND cflags -I "${i}")
    endforeach()

    file(GLOB WASM_GUEST_SOURCES CONFIGURE_DEPENDS "${PORT_DIR}/guest/wasm/*.c")
    list(FILTER GUEST_SOURCES EXCLUDE REGEX "/gpu[.]c$")
    set(objs "")
    set(index 0)
    foreach(src IN LISTS GAME_SOURCES GUEST_SOURCES WASM_GUEST_SOURCES G_EXTRA_SOURCES ITEMS "${GEN_DIR}/ovl_markers_wasm.c")
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${WASM_DIR}/obj/${index}_${stem}.o")
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${PY}" "${TOOLS}/wasm_cc.py" "--overlays=${ovl_csv}" "${WASM_CC}" ${cflags}
                -MD -MF "${obj}.d" -c "${src}" -o "${obj}"
            DEPENDS "${src}" "${TOOLS}/wasm_cc.py"
            DEPFILE "${obj}.d"
            COMMENT "wasm ${stem}.c"
            VERBATIM)
        list(APPEND objs "${obj}")
        math(EXPR index "${index} + 1")
    endforeach()
    set(sym_obj "${WASM_DIR}/obj/ps1_symbols.o")
    add_custom_command(OUTPUT "${sym_obj}"
        COMMAND "${WASM_CC}" --target=wasm32 -c "${GEN_DIR}/ps1_symbols.s" -o "${sym_obj}"
        DEPENDS "${GEN_DIR}/ps1_symbols.s" VERBATIM)

    # Memory layout: see Source/Wasm/ps1w.h (initial memory = PS1W_HEAP_END, 0x807F0000).
    # ps1_symbols.o (PS1 RAM) must come first.
    set(wasm "${WASM_DIR}/${G_NAME}.wasm")
    add_custom_command(OUTPUT "${wasm}"
        COMMAND "${PY}" "${TOOLS}/wasm_link.py" "${WASM_LD}" --no-entry --export=port_game_entry
            --no-stack-first -z stack-size=524288 --global-base=2147483648
            --initial-memory=2155806720 --max-memory=2155806720
            "--allow-undefined-file=${TOOLS}/wasm_imports.txt" --error-limit=0
            "${sym_obj}" ${objs} "${WASI_LIB}/libsetjmp.a" "${WASI_LIB}/libc.a" ${WASM_RT_BUILTINS}
            -o "${wasm}"
        DEPENDS "${sym_obj}" ${objs} "${TOOLS}/wasm_imports.txt" "${TOOLS}/wasm_link.py"
        COMMENT "wasm-ld ${G_NAME}.wasm"
        VERBATIM)

    # The translated guest goes straight into the addon (com.recomp.ps1/Source/Guest/<name>):
    # the editor then compiles it into the com.recomp.ps1 addon for every platform, and
    # Ps1Player runs it in-process (it registers itself under the game's package id).
    # The standalone programs below compile the same files.
    get_filename_component(SOURCE_WASM_DIR "${PS1_RUNTIME_DIR}/../Source/Wasm" ABSOLUTE)
    get_filename_component(W2C_DIR "${PS1_RUNTIME_DIR}/../Source/Guest/${G_NAME}" ABSOLUTE)
    if(G_PACKAGE)
        set(package "${G_PACKAGE}")
    else()
        # <package>/Native/CMakeLists.txt
        get_filename_component(package "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
        get_filename_component(package "${package}" NAME)
    endif()
    set(w2c_count 8)
    set(stem "${G_NAME}_guest")
    set(w2c_c "")
    math(EXPR last "${w2c_count} - 1")
    foreach(i RANGE ${last})
        list(APPEND w2c_c "${W2C_DIR}/${stem}_${i}.c")
    endforeach()
    add_custom_command(OUTPUT ${w2c_c} "${W2C_DIR}/${stem}.h" "${W2C_DIR}/${stem}-impl.h"
            "${W2C_DIR}/${stem}_module.c" "${W2C_DIR}/${stem}_register.cpp"
        COMMAND "${PY}" "${TOOLS}/wasm_to_c.py" "${WASM2C}" "${wasm}" "${W2C_DIR}" ${G_NAME} ${w2c_count}
            "--package=${package}" "--title=${G_TITLE}" "--disc=${disc_name}" "--runtime-include=../../Wasm/"
            --register
        DEPENDS "${wasm}" "${TOOLS}/wasm_to_c.py"
        COMMENT "wasm2c ${G_NAME}.wasm -> Source/Guest/${G_NAME}"
        VERBATIM)

    # What the editor needs (Setup Dependencies, Source/Ps1Dependencies.cpp, runs this target): the translated game in
    # the addon and the extracted disc, without the standalone program below.
    add_custom_target(ps1_addon DEPENDS ${w2c_c} "${W2C_DIR}/${stem}_module.c" "${W2C_DIR}/${stem}_register.cpp")
    if(TARGET ${G_NAME}_disc)
        add_dependencies(ps1_addon ${G_NAME}_disc)
    endif()

    # Host program: platform host + wasm2c runtime/backend + the translated guest.
    if(NINTENDO_WII OR NINTENDO_GAMECUBE)
        set(platform ogc)
    else()
        set(platform win32)
    endif()
    set(disc_reader "${SOURCE_WASM_DIR}/ps1w_disc.c")
    file(WRITE "${GEN_DIR}/ps1w_default_module.c.new"
        "/* Generated by Ps1Game.cmake - the game the standalone program runs. */\n#include \"ps1w_module.h\"\nextern const Ps1wModule ps1w_module_${G_NAME};\nconst Ps1wModule *const ps1w_default_module = &ps1w_module_${G_NAME};\n")
    file(COPY_FILE "${GEN_DIR}/ps1w_default_module.c.new" "${GEN_DIR}/ps1w_default_module.c" ONLY_IF_DIFFERENT)
    file(GLOB host_sources CONFIGURE_DEPENDS "${PORT_DIR}/host/${platform}/*.c")
    add_executable(${G_NAME} ${host_sources} "${PORT_DIR}/host/wasm_standalone/ps1w_standalone.c"
        "${SOURCE_WASM_DIR}/ps1w_rt.c" "${SOURCE_WASM_DIR}/ps1w_backend.c" "${SOURCE_WASM_DIR}/ps1w_gpu.c" ${disc_reader}
        "${GEN_DIR}/ps1w_default_module.c" "${W2C_DIR}/${stem}_module.c" ${w2c_c})
    target_include_directories(${G_NAME} PRIVATE "${PORT_DIR}/include" "${GEN_DIR}" "${SOURCE_WASM_DIR}")
    target_compile_options(${G_NAME} PRIVATE -O2 -g -w)
    if(platform STREQUAL "ogc")
        target_link_libraries(${G_NAME} PRIVATE fat asnd)
        ogc_create_dol(${G_NAME})
    elseif(WIN32)
        target_link_libraries(${G_NAME} PRIVATE kernel32 user32 gdi32 winmm xinput)
        target_link_options(${G_NAME} PRIVATE -fuse-ld=lld -Wl,/debug)
    endif()
    # not published yet: Bin/ keeps the native build until the wasm one replaces it
endmacro()
