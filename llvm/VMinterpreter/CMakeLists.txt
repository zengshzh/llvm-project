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
# Clears stale cache entry before find_path (in case previous search failed)
if(DEFINED CACHE{FFI_INCLUDE_DIR} AND NOT FFI_INCLUDE_DIR)
  unset(FFI_INCLUDE_DIR CACHE)
endif()

# Priority 1: no hints — works when vcpkg's toolchain file is active, or
#   when libffi is in compiler's default search paths.
# Priority 2: fallback using VCPKG_ROOT env var and common triplets.
find_path(FFI_INCLUDE_DIR ffi.h)
if(NOT FFI_INCLUDE_DIR AND DEFINED ENV{VCPKG_ROOT})
  foreach(_TRIPLET x64-windows-static x64-windows x64-windows-static-md)
    find_path(FFI_INCLUDE_DIR ffi.h
      PATHS "$ENV{VCPKG_ROOT}/installed/${_TRIPLET}/include"
      NO_DEFAULT_PATH)
    if(FFI_INCLUDE_DIR)
      break()
    endif()
  endforeach()
endif()

find_library(FFI_LIBRARY ffi)
if(NOT FFI_LIBRARY AND DEFINED ENV{VCPKG_ROOT})
  foreach(_TRIPLET x64-windows-static x64-windows x64-windows-static-md)
    find_library(FFI_LIBRARY ffi
      PATHS "$ENV{VCPKG_ROOT}/installed/${_TRIPLET}/lib"
      NO_DEFAULT_PATH)
    if(FFI_LIBRARY)
      break()
    endif()
  endforeach()
endif()

if(FFI_INCLUDE_DIR)
  target_include_directories(vminterpreter PRIVATE ${FFI_INCLUDE_DIR})
else()
  message(WARNING "libffi header not found; mixed-mode calls will fail to compile. "
                  "Set FFI_INCLUDE_DIR or install via: vcpkg install libffi")
endif()

if(FFI_LIBRARY)
  target_link_libraries(vminterpreter PRIVATE ${FFI_LIBRARY})
  # Merge libffi into vminterpreter.lib so consumers are self-contained.
  get_filename_component(_TOOLCHAIN_DIR "${CMAKE_AR}" DIRECTORY)
  find_program(LLVM_LIB llvm-lib PATHS "${_TOOLCHAIN_DIR}" NO_DEFAULT_PATH)
  if(LLVM_LIB)
    add_custom_command(TARGET vminterpreter POST_BUILD
      COMMAND "${LLVM_LIB}" /OUT:$<TARGET_FILE:vminterpreter>
              $<TARGET_FILE:vminterpreter> "${FFI_LIBRARY}"
      COMMENT "Merging libffi into vminterpreter"
      VERBATIM
    )
  else()
    message(WARNING "llvm-lib not found; ffi.lib not merged. "
                    "Consumers must link ffi.lib separately.")
  endif()
else()
  message(WARNING "libffi library not found; mixed-mode calls will fail to link. "
                  "Set FFI_LIBRARY or install via: vcpkg install libffi")
endif()