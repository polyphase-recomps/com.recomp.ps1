# Wii / GameCube toolchain (devkitPPC + libogc) for a Windows CMake.
# devkitPro's own toolchain files only run under its msys2 CMake; this one does the
# same job for the CMake that comes with Visual Studio (Ninja, Windows paths).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<this> -DPS1_OGC_PLATFORM=wii|gamecube [-DDEVKITPRO=C:/devkitPro]
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

if(NOT PS1_OGC_PLATFORM)
    set(PS1_OGC_PLATFORM "$ENV{PS1_OGC_PLATFORM}")
endif()
if(NOT DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO} AND EXISTS "$ENV{DEVKITPRO}")
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        set(DEVKITPRO "C:/devkitPro")
    endif()
endif()
file(TO_CMAKE_PATH "${DEVKITPRO}" DEVKITPRO)
# try_compile projects see only these variables
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS1_OGC_PLATFORM DEVKITPRO)

if(PS1_OGC_PLATFORM STREQUAL "wii")
    set(NINTENDO_WII TRUE)
    set(machine rvl)
    set(libdir wii)
    set(extra_libs wiiuse bte)
    set(console_define __wii__)
elseif(PS1_OGC_PLATFORM STREQUAL "gamecube")
    set(NINTENDO_GAMECUBE TRUE)
    set(machine ogc)
    set(libdir cube)
    set(extra_libs "")
    set(console_define __gamecube__)
else()
    message(FATAL_ERROR "PS1_OGC_PLATFORM must be wii or gamecube")
endif()

set(PPC_BIN "${DEVKITPRO}/devkitPPC/bin")
set(CMAKE_C_COMPILER "${PPC_BIN}/powerpc-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "${PPC_BIN}/powerpc-eabi-g++.exe")
set(CMAKE_ASM_COMPILER "${PPC_BIN}/powerpc-eabi-gcc.exe")
set(CMAKE_AR "${PPC_BIN}/powerpc-eabi-gcc-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PPC_BIN}/powerpc-eabi-gcc-ranlib.exe" CACHE FILEPATH "")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(arch "-m${machine} -mcpu=750 -meabi -mhard-float")
set(CMAKE_C_FLAGS_INIT "${arch} -ffunction-sections -fdata-sections -D${console_define} -DGEKKO")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${arch} -Wl,--gc-sections -L${DEVKITPRO}/libogc/lib/${libdir}")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${DEVKITPRO}/libogc/include")
set(CMAKE_C_STANDARD_LIBRARIES "")
foreach(lib IN LISTS extra_libs ITEMS ogc m)
    string(APPEND CMAKE_C_STANDARD_LIBRARIES " -l${lib}")
endforeach()

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/devkitPPC" "${DEVKITPRO}/libogc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# <target>.elf -> <target>.dol next to it
function(ogc_create_dol target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${DEVKITPRO}/tools/bin/elf2dol.exe" "$<TARGET_FILE:${target}>"
            "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.dol"
        COMMENT "elf2dol ${target}"
        VERBATIM)
endfunction()
