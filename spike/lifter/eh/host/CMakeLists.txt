# P1-1 host: a lifted VC6 program + pcrecomp win32hle (with patches/) + the
# guest EH runtime, native arm64 macOS. One build directory per lifted program:
#
#   cmake -S spike/lifter/eh/host -B $LIFTER/build-game -G Ninja -DLIFTER=$LIFTER
#   cmake -S spike/lifter/eh/host -B $LIFTER/build-ehtest -G Ninja -DLIFTER=$LIFTER \
#         -DGEN_DIR=$LIFTER/ehtest/out/gen
#   cmake --build <dir> -j 4
cmake_minimum_required(VERSION 3.20)
project(sfcop_eh C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(LIFTER "$SFCOP_ROOT/work/lifter-eh" CACHE PATH "work/lifter-eh")
set(GEN_DIR "${LIFTER}/out/gen" CACHE PATH "the lifted C (run_lift_eh.py --out)")
set(PCRECOMP "${LIFTER}/pcrecomp")
set(EH_DIR "${CMAKE_SOURCE_DIR}/..")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(sfcop main.c ${EH_DIR}/runtime/sfcop_eh.c ${HLE_SOURCES} ${GEN_SOURCES})
# The spike's include/ first: its SDL2/SDL_ttf.h stands in for the missing SDL2_ttf.
target_include_directories(sfcop BEFORE PRIVATE ${EH_DIR}/../host/include)
target_include_directories(sfcop PRIVATE ${EH_DIR}/runtime ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(sfcop PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS
                                         RECOMP_MEM_BASE=0x100000000000ull)
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
target_compile_options(sfcop PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(sfcop PRIVATE PkgConfig::SDL2 pthread m)
