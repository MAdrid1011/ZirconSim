find_package(verilator CONFIG REQUIRED HINTS
    "$ENV{VERILATOR_ROOT}"
    "${VERILATOR_ROOT}"
)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPIKE REQUIRED IMPORTED_TARGET riscv-riscv)
find_program(VERILATOR_EXECUTABLE verilator REQUIRED)

option(ZIRCON_SIM_ENABLE_VCD "Build zircon-sim with bounded VCD tracing support" OFF)
set(ZIRCON_VERILATOR_BUILD_JOBS 4 CACHE STRING
    "Parallel jobs for compiling the Verilated model")

add_executable(zircon-sim
    src/AXIMemory.cc
    src/ElfImage.cc
    src/main.cc
    src/SpikeReference.cc
    src/Statistic.cc
)
target_compile_features(zircon-sim PRIVATE cxx_std_20)
target_compile_options(zircon-sim PRIVATE
    -Wall
    -Wextra
    -Werror
)
set(ZIRCON_VERILATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/rtl")
set(ZIRCON_VERILATED_MODEL "${ZIRCON_VERILATED_DIR}/libVZirconCore.a")
set(ZIRCON_VERILATED_RUNTIME "${ZIRCON_VERILATED_DIR}/libverilated.a")
set(ZIRCON_VERILATOR_TRACE_ARGS)
if(ZIRCON_SIM_ENABLE_VCD)
    target_compile_definitions(zircon-sim PRIVATE ZIRCON_ENABLE_VCD=1)
    list(APPEND ZIRCON_VERILATOR_TRACE_ARGS --trace-vcd)
endif()
find_program(CCACHE_EXECUTABLE ccache)
if(NOT CCACHE_EXECUTABLE)
    set(CCACHE_EXECUTABLE "")
endif()

add_custom_command(
    OUTPUT
        "${ZIRCON_VERILATED_MODEL}"
        "${ZIRCON_VERILATED_RUNTIME}"
        "${ZIRCON_VERILATED_DIR}/VZirconCore.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZIRCON_VERILATED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "VERILATOR_ROOT=${VERILATOR_ROOT}"
        "CXX=${CMAKE_CXX_COMPILER}"
        "OBJCACHE=${CCACHE_EXECUTABLE}"
        "MAKEFLAGS="
        "${VERILATOR_EXECUTABLE}"
        --cc
        --build
        --build-jobs "${ZIRCON_VERILATOR_BUILD_JOBS}"
        --prefix VZirconCore
        --top-module ZirconCore
        --Mdir "${ZIRCON_VERILATED_DIR}"
        -y "${ZIRCON_RTL_DIR}"
        -Wall
        --output-split 160000
        ${ZIRCON_VERILATOR_TRACE_ARGS}
        -CFLAGS "-std=c++20 -O3"
        "${ZIRCON_RTL_TOP}"
    DEPENDS
        "${ZIRCON_RTL_TOP}"
        "${VERILATOR_EXECUTABLE}"
    COMMENT "Building the Verilated ZirconCore model"
    VERBATIM
)
add_custom_target(zircon-verilated-model
    DEPENDS
        "${ZIRCON_VERILATED_MODEL}"
        "${ZIRCON_VERILATED_RUNTIME}"
        "${ZIRCON_VERILATED_DIR}/VZirconCore.h"
)
add_dependencies(zircon-verilated-model zircon-rtl)
add_dependencies(zircon-sim zircon-verilated-model)

target_include_directories(zircon-sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${ZIRCON_VERILATED_DIR}"
)
target_include_directories(zircon-sim SYSTEM PRIVATE
    "${VERILATOR_ROOT}/include"
    "${VERILATOR_ROOT}/include/vltstd"
)
target_link_libraries(zircon-sim PRIVATE
    "${ZIRCON_VERILATED_MODEL}"
    "${ZIRCON_VERILATED_RUNTIME}"
    Threads::Threads
    PkgConfig::SPIKE
)
set_target_properties(zircon-sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(zircon-sim-unit
    src/ElfImage.cc
    src/SpikeReference.cc
    src/Statistic.cc
    tests/UnitTests.cc
)
target_compile_features(zircon-sim-unit PRIVATE cxx_std_20)
target_compile_options(zircon-sim-unit PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_include_directories(zircon-sim-unit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(zircon-sim-unit PRIVATE PkgConfig::SPIKE)
set_target_properties(zircon-sim-unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set(ZIRCON_RV_SOFTWARE_DIR "${PROJECT_SOURCE_DIR}/RV-Software" CACHE PATH
    "Path to the Zircon RV-Software checkout"
)
set(ZIRCON_TEST_ELF "" CACHE FILEPATH
    "RV32 ELF used by the ZirconSim host-side unit test"
)

find_program(MAKE_EXECUTABLE make)
if(MAKE_EXECUTABLE AND EXISTS "${ZIRCON_RV_SOFTWARE_DIR}/coremark/Makefile")
    set(ZIRCON_COREMARK_BUILD_ELF
        "${ZIRCON_RV_SOFTWARE_DIR}/coremark/build/coremark-i30-rv32imaf_zicsr_zifencei-ilp32f.elf"
    )
    set(ZIRCON_COREMARK_ELF
        "${ZIRCON_RV_SOFTWARE_DIR}/coremark/build/coremark-rv32imaf_zicsr_zifencei-ilp32f.elf"
    )
    file(GLOB_RECURSE ZIRCON_COREMARK_SOURCES CONFIGURE_DEPENDS
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.c"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.h"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.S"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.ld"
        "${ZIRCON_RV_SOFTWARE_DIR}/coremark/*.c"
        "${ZIRCON_RV_SOFTWARE_DIR}/coremark/*.h"
        "${ZIRCON_RV_SOFTWARE_DIR}/coremark/Makefile"
    )
    add_custom_command(
        OUTPUT "${ZIRCON_COREMARK_ELF}"
        COMMAND "${MAKE_EXECUTABLE}" -C "${ZIRCON_RV_SOFTWARE_DIR}/coremark"
            "ITERATIONS=30"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ZIRCON_COREMARK_BUILD_ELF}"
            "${ZIRCON_COREMARK_ELF}"
        DEPENDS ${ZIRCON_COREMARK_SOURCES}
        COMMENT "Building the RV32 CoreMark benchmark"
        VERBATIM
    )
    add_custom_target(zircon-coremark-software DEPENDS "${ZIRCON_COREMARK_ELF}")
    add_custom_target(coremark
        COMMAND "$<TARGET_FILE:zircon-sim>"
            --elf "${ZIRCON_COREMARK_ELF}"
            --seed 1
            --max-cycles 10000000
            --stall-cycles 10000
        DEPENDS zircon-sim zircon-coremark-software
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Running CoreMark with Spike differential testing"
        USES_TERMINAL
        VERBATIM
    )
endif()

if(MAKE_EXECUTABLE AND EXISTS "${ZIRCON_RV_SOFTWARE_DIR}/functest/Makefile")
    file(GLOB_RECURSE ZIRCON_FUNCTEST_COMMON_SOURCES CONFIGURE_DEPENDS
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.c"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.h"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.S"
        "${ZIRCON_RV_SOFTWARE_DIR}/base-port/*.ld"
    )
    file(GLOB ZIRCON_FUNCTEST_SOURCES CONFIGURE_DEPENDS
        "${ZIRCON_RV_SOFTWARE_DIR}/functest/src/*.c"
    )
    foreach(ZIRCON_FUNCTEST_SOURCE IN LISTS ZIRCON_FUNCTEST_SOURCES)
        get_filename_component(ZIRCON_FUNCTEST_NAME "${ZIRCON_FUNCTEST_SOURCE}" NAME_WE)
        set(ZIRCON_FUNCTEST_ELF
            "${ZIRCON_RV_SOFTWARE_DIR}/functest/build/${ZIRCON_FUNCTEST_NAME}-rv32imaf_zicsr_zifencei-ilp32f.elf"
        )
        add_custom_command(
            OUTPUT "${ZIRCON_FUNCTEST_ELF}"
            COMMAND "${MAKE_EXECUTABLE}" -C "${ZIRCON_RV_SOFTWARE_DIR}/functest"
                "NAMES=${ZIRCON_FUNCTEST_NAME}"
            DEPENDS
                ${ZIRCON_FUNCTEST_COMMON_SOURCES}
                "${ZIRCON_FUNCTEST_SOURCE}"
                "${ZIRCON_RV_SOFTWARE_DIR}/functest/Makefile"
            COMMENT "Building RV32 functest ${ZIRCON_FUNCTEST_NAME}"
            VERBATIM
        )
        add_custom_target("zircon-functest-${ZIRCON_FUNCTEST_NAME}"
            DEPENDS "${ZIRCON_FUNCTEST_ELF}"
        )
        add_custom_target("functest-${ZIRCON_FUNCTEST_NAME}"
            COMMAND "$<TARGET_FILE:zircon-sim>"
                --elf "${ZIRCON_FUNCTEST_ELF}"
                --seed 1
                --max-cycles 1000000
                --stall-cycles 10000
            DEPENDS zircon-sim "zircon-functest-${ZIRCON_FUNCTEST_NAME}"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Running functest ${ZIRCON_FUNCTEST_NAME} with Spike differential testing"
            USES_TERMINAL
            VERBATIM
        )
    endforeach()
endif()

if(BUILD_TESTING AND ZIRCON_TEST_ELF)
    add_test(
        NAME zircon-sim-unit
        COMMAND zircon-sim-unit "${ZIRCON_TEST_ELF}"
    )
endif()
