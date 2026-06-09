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

# Static library (no LLVM dependency)
add_library(vminterpreter STATIC
    ${VMINTERPRETER_SOURCES}
)

target_include_directories(vminterpreter
    PUBLIC
        $<BUILD_INTERFACE:${VMINTERPRETER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# libffi for mixed-mode calls
find_path(FFI_INCLUDE_DIR ffi.h)
find_library(FFI_LIBRARY ffi)

if(FFI_INCLUDE_DIR)
  target_include_directories(vminterpreter PUBLIC ${FFI_INCLUDE_DIR})
else()
  message(WARNING "libffi header not found; mixed-mode calls will fail to compile")
endif()

if(FFI_LIBRARY)
  target_link_libraries(vminterpreter PUBLIC ${FFI_LIBRARY})
else()
  message(WARNING "libffi library not found; mixed-mode calls will fail to link")
endif()
