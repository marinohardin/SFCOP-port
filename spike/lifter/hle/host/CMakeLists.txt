# P1-2 build: the lifted StarFleetOP.exe + pcrecomp win32hle (with patches/
# applied) + the HLE modules in ../src + host/main.c, native arm64 macOS.
#
#   cmake -S spike/lifter/hle/host -B $WORK/build -G Ninja -DLIFTER=$WORK
#   cmake --build $WORK/build -j 4
#
# The generated C goes into its own static library so that editing the HLE or
# the host relinks without recompiling 191 MB of lifted code.
cmake_minimum_required(VERSION 3.20)
project(sfcop_hle C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(LIFTER "$SFCOP_ROOT/work/lifter-hle" CACHE PATH "work/lifter-hle")
set(PCRECOMP "${LIFTER}/pcrecomp")
set(GEN_DIR "${LIFTER}/out/gen")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
file(GLOB SFCOP_HLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/../src/*.c")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(COMMON_DEFS RECOMP_RETADDR=0u RECOMP_LOCAL_REGS RECOMP_MEM_BASE=0x100000000000ull)
set(COMMON_INCLUDES ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)

set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
add_library(sfcop_gen STATIC ${GEN_SOURCES})
target_include_directories(sfcop_gen PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(sfcop_gen PRIVATE ${COMMON_DEFS})
target_compile_options(sfcop_gen PRIVATE -w ${GEN_OPT} -fno-strict-aliasing -fwrapv)

add_executable(sfcop main.c ${SFCOP_HLE_SOURCES} ${HLE_SOURCES})
# include/ first: its SDL2/SDL_ttf.h stands in for the missing SDL2_ttf.
target_include_directories(sfcop BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(sfcop PRIVATE ${COMMON_INCLUDES} ${CMAKE_SOURCE_DIR}/../src)
target_compile_definitions(sfcop PRIVATE ${COMMON_DEFS})
target_compile_options(sfcop PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(sfcop PRIVATE sfcop_gen PkgConfig::SDL2 pthread m)

# The HLE modules' self-test (../tests): win32hle and the modules without the
# lifted game (sfcop_game.c needs it), run by the selftest step of ../scripts/run_all.sh.
set(SELFTEST_HLE_SOURCES ${SFCOP_HLE_SOURCES})
list(FILTER SELFTEST_HLE_SOURCES EXCLUDE REGEX "sfcop_game\\.c$")
add_executable(hle_selftest ${CMAKE_SOURCE_DIR}/../tests/hle_selftest.c ${SELFTEST_HLE_SOURCES} ${HLE_SOURCES})
target_include_directories(hle_selftest BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(hle_selftest PRIVATE ${HLE} ${PCRECOMP}/runtime/recomp32 ${CMAKE_SOURCE_DIR}/../src)
target_compile_definitions(hle_selftest PRIVATE RECOMP_RETADDR=0u RECOMP_MEM_BASE=0x100000000000ull)
target_compile_options(hle_selftest PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(hle_selftest PRIVATE PkgConfig::SDL2 pthread m)
