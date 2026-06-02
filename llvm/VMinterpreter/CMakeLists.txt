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
    src/vmexecute.c
)

# Platform-specific CALL trampoline
if(WIN32)
    list(APPEND VMINTERPRETER_SOURCES src/vmcall_win64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    list(APPEND VMINTERPRETER_SOURCES src/vmcall_linux_arm64.S)
else()
    list(APPEND VMINTERPRETER_SOURCES src/vmcall_linux64.S)
endif()

# Static library (no LLVM dependency)
add_library(vminterpreter STATIC
    ${VMINTERPRETER_SOURCES}
)

target_include_directories(vminterpreter
    PUBLIC
        $<BUILD_INTERFACE:${VMINTERPRETER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
