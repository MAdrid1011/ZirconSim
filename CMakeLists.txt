find_package(verilator CONFIG REQUIRED HINTS
    "$ENV{VERILATOR_ROOT}"
    "${VERILATOR_ROOT}"
)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
include(CheckCXXCompilerFlag)
pkg_check_modules(SPIKE REQUIRED IMPORTED_TARGET riscv-riscv)
find_program(VERILATOR_EXECUTABLE verilator REQUIRED)

option(ZIRCON_SIM_ENABLE_VCD "Build zircon-sim with bounded VCD tracing support" OFF)
option(ZIRCON_SIM_ENABLE_CHECKPOINTS "Build zircon-sim with Verilator save/restore support" OFF)
option(ZIRCON_SIM_PUBLIC_INTERNALS "Expose Verilator internal signals for simulator diagnostics" OFF)
include(ProcessorCount)
ProcessorCount(ZIRCON_HOST_PROCESSORS)
if(NOT ZIRCON_HOST_PROCESSORS OR ZIRCON_HOST_PROCESSORS GREATER 2)
    set(ZIRCON_DEFAULT_VERILATOR_BUILD_JOBS 2)
else()
    set(ZIRCON_DEFAULT_VERILATOR_BUILD_JOBS "${ZIRCON_HOST_PROCESSORS}")
endif()
set(ZIRCON_VERILATOR_BUILD_JOBS "${ZIRCON_DEFAULT_VERILATOR_BUILD_JOBS}" CACHE STRING
    "Parallel jobs for compiling the Verilated model")
set(ZIRCON_VERILATOR_THREADS 5 CACHE STRING
    "Runtime threads used by the Verilated model")
set(ZIRCON_VERILATOR_OUTPUT_SPLIT 20000 CACHE STRING
    "Verilator C++ output split threshold")
set(ZIRCON_VERILATOR_OUTPUT_SPLIT_CFUNCS 10000 CACHE STRING
    "Verilator generated-function split threshold")
option(ZIRCON_SIM_NATIVE_OPTIMIZATION
    "Compile the simulator and Verilated model for the current host CPU" ON)
option(ZIRCON_SIM_ENABLE_THIN_LTO
    "Enable ThinLTO for the simulator and Verilated model" OFF)
set(ZIRCON_SIM_PGO_MODE "OFF" CACHE STRING
    "Profile-guided optimization mode: OFF, GENERATE, or USE")
set_property(CACHE ZIRCON_SIM_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(ZIRCON_SIM_PGO_PROFILE "" CACHE FILEPATH
    "LLVM instrumentation profile used when ZIRCON_SIM_PGO_MODE is USE")
if(NOT ZIRCON_SIM_PGO_MODE STREQUAL "OFF" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ZirconSim instrumentation PGO requires Clang or AppleClang")
endif()

add_executable(zircon-sim
    src/AXIMemory.cc
    src/Checkpoint.cc
    src/ElfImage.cc
    src/main.cc
    src/PlatformDevices.cc
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
set(ZIRCON_VERILATOR_CHECKPOINT_ARGS)
set(ZIRCON_VERILATOR_DEBUG_ARGS)
set(ZIRCON_VERILATOR_CXX_FLAGS "-std=c++20")
set(ZIRCON_VERILATOR_OPT_FAST "-O3")
set(ZIRCON_VERILATOR_OPT_SLOW "-O0")
if(ZIRCON_SIM_NATIVE_OPTIMIZATION)
    check_cxx_compiler_flag("-march=native" ZIRCON_CXX_SUPPORTS_MARCH_NATIVE)
    if(ZIRCON_CXX_SUPPORTS_MARCH_NATIVE)
        set(ZIRCON_NATIVE_FLAG "-march=native")
    else()
        check_cxx_compiler_flag("-mcpu=native" ZIRCON_CXX_SUPPORTS_MCPU_NATIVE)
        if(NOT ZIRCON_CXX_SUPPORTS_MCPU_NATIVE)
            message(FATAL_ERROR "The selected C++ compiler has no supported native-CPU flag")
        endif()
        set(ZIRCON_NATIVE_FLAG "-mcpu=native")
    endif()
    string(APPEND ZIRCON_VERILATOR_CXX_FLAGS " ${ZIRCON_NATIVE_FLAG}")
    target_compile_options(zircon-sim PRIVATE "${ZIRCON_NATIVE_FLAG}")
endif()
if(ZIRCON_SIM_ENABLE_VCD)
    target_compile_definitions(zircon-sim PRIVATE ZIRCON_ENABLE_VCD=1)
    list(APPEND ZIRCON_VERILATOR_TRACE_ARGS
        --trace-vcd
        --output-split-ctrace 5000
    )
    set(ZIRCON_VERILATOR_OPT_FAST "-O1")
endif()
if(ZIRCON_SIM_ENABLE_CHECKPOINTS)
    target_compile_definitions(zircon-sim PRIVATE ZIRCON_ENABLE_CHECKPOINTS=1)
    list(APPEND ZIRCON_VERILATOR_CHECKPOINT_ARGS --savable)
endif()
if(ZIRCON_SIM_PUBLIC_INTERNALS)
    list(APPEND ZIRCON_VERILATOR_DEBUG_ARGS --public-flat-rw)
endif()
if(ZIRCON_SIM_ENABLE_THIN_LTO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(ZIRCON_LTO_FLAG "-flto=thin")
    else()
        set(ZIRCON_LTO_FLAG "-flto")
    endif()
    string(APPEND ZIRCON_VERILATOR_OPT_FAST " ${ZIRCON_LTO_FLAG}")
    target_compile_options(zircon-sim PRIVATE "${ZIRCON_LTO_FLAG}")
    target_link_options(zircon-sim PRIVATE "${ZIRCON_LTO_FLAG}")
endif()
if(ZIRCON_SIM_PGO_MODE STREQUAL "GENERATE")
    set(ZIRCON_SIM_PGO_RAW "${CMAKE_BINARY_DIR}/zircon-%p.profraw")
    set(ZIRCON_SIM_PGO_FLAG "-fprofile-instr-generate=${ZIRCON_SIM_PGO_RAW}")
elseif(ZIRCON_SIM_PGO_MODE STREQUAL "USE")
    if(NOT ZIRCON_SIM_PGO_PROFILE OR NOT EXISTS "${ZIRCON_SIM_PGO_PROFILE}")
        message(FATAL_ERROR "ZIRCON_SIM_PGO_PROFILE must name an existing .profdata file in USE mode")
    endif()
    set(ZIRCON_SIM_PGO_FLAG "-fprofile-instr-use=${ZIRCON_SIM_PGO_PROFILE}")
elseif(NOT ZIRCON_SIM_PGO_MODE STREQUAL "OFF")
    message(FATAL_ERROR "ZIRCON_SIM_PGO_MODE must be OFF, GENERATE, or USE")
endif()
if(DEFINED ZIRCON_SIM_PGO_FLAG)
    string(APPEND ZIRCON_VERILATOR_OPT_FAST " ${ZIRCON_SIM_PGO_FLAG}")
    target_compile_options(zircon-sim PRIVATE "${ZIRCON_SIM_PGO_FLAG}")
    target_link_options(zircon-sim PRIVATE "${ZIRCON_SIM_PGO_FLAG}")
endif()
if(ZIRCON_SIM_PGO_MODE STREQUAL "USE")
    target_compile_options(zircon-sim PRIVATE -Wno-profile-instr-out-of-date)
endif()
find_program(CCACHE_EXECUTABLE ccache)
if(NOT CCACHE_EXECUTABLE)
    set(CCACHE_EXECUTABLE "")
endif()
if(CMAKE_CXX_COMPILER_AR)
    set(ZIRCON_VERILATOR_AR "${CMAKE_CXX_COMPILER_AR}")
else()
    set(ZIRCON_VERILATOR_AR "${CMAKE_AR}")
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
        -MAKEFLAGS
            "CXX='${CMAKE_CXX_COMPILER}' LINK='${CMAKE_CXX_COMPILER}' AR='${ZIRCON_VERILATOR_AR}' OPT_FAST='${ZIRCON_VERILATOR_OPT_FAST}' OPT_SLOW='${ZIRCON_VERILATOR_OPT_SLOW}' OPT_GLOBAL='${ZIRCON_VERILATOR_OPT_SLOW}'"
        --prefix VZirconCore
        --top-module ZirconCore
        --Mdir "${ZIRCON_VERILATED_DIR}"
        -y "${ZIRCON_RTL_DIR}"
        -Wall
        # ICache refill RAMs intentionally leave their readback pins unused;
        # the lower address bits of an aligned line response are likewise
        # intentionally unused in the generated RTL.
        -Wno-UNUSEDSIGNAL
        -Wno-PINCONNECTEMPTY
        --threads "${ZIRCON_VERILATOR_THREADS}"
        --output-split "${ZIRCON_VERILATOR_OUTPUT_SPLIT}"
        --output-split-cfuncs "${ZIRCON_VERILATOR_OUTPUT_SPLIT_CFUNCS}"
        ${ZIRCON_VERILATOR_TRACE_ARGS}
        ${ZIRCON_VERILATOR_CHECKPOINT_ARGS}
        ${ZIRCON_VERILATOR_DEBUG_ARGS}
        -CFLAGS "${ZIRCON_VERILATOR_CXX_FLAGS}"
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
    src/Checkpoint.cc
    src/ElfImage.cc
    src/PlatformDevices.cc
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
