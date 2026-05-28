cmake_minimum_required(VERSION 3.20.0)

project(VMinterpreter
    VERSION 1.0.0
    DESCRIPTION "VM interpreter standalone library"
    LANGUAGES C)

# Header-only include
set(VMINTERPRETER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(VMINTERPRETER_SOURCES
    src/vmprint.c
)

# Static library (no LLVM dependency)
add_library(vminterpreter STATIC
    ${VMINTERPRETER_SOURCES}
)

target_include_directories(vminterpreter
    PUBLIC
        $<BUILD_INTERFACE:${VMINTERPRETER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
