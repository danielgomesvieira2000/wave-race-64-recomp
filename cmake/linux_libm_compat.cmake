# Keep Linux x86-64 releases usable on glibc 2.42. This must precede the
# subdirectories: RT64 and the runtime also emit calls to these libm symbols.
include_guard(GLOBAL)
option(WR64_LINUX_MATH_COMPAT "Use the pre-2.43 Linux x86-64 float libm ABI" ON)

if(WR64_LINUX_MATH_COMPAT AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$"
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$"
   AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    # Detect libc here so the forced header does not include features.h before
    # a translation unit can select its own feature-test macros.
    include(CheckCSourceCompiles)
    check_c_source_compiles("#include <features.h>
        #ifndef __GLIBC__
        #error glibc required
        #endif
        int main(void) { return 0; }" WR64_HAS_GLIBC)
    if(WR64_HAS_GLIBC)
        get_filename_component(WR64_LIBM_COMPAT_HEADER
            "${CMAKE_CURRENT_LIST_DIR}/../include/wr64/linux_math_compat.h" ABSOLUTE)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C,CXX>:-DWR64_LIBM_COMPAT_GLIBC=1>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-include>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:${WR64_LIBM_COMPAT_HEADER}>")
    endif()
endif()
