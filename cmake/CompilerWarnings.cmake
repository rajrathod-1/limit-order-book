# Warning and tuning helpers, kept out of the top-level file.

function(lob_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow                  # a shadowed variable in matching logic is a real bug
            -Wnon-virtual-dtor
            -Wcast-align              # matters: we decode unaligned wire data deliberately
            -Wunused
            -Woverloaded-virtual
            -Wconversion              # silent narrowing of a quantity is a correctness issue
            -Wsign-conversion
            -Wdouble-promotion
            -Wformat=2)
    endif()
endfunction()

# Tune for the host. The book's hot loops depend on count-leading-zeros and on
# the cache line size, both of which the compiler picks better when it knows the
# target. Kept optional so CI can build portable binaries.
function(lob_tune target)
    if(NOT LOB_NATIVE_ARCH)
        return()
    endif()
    include(CheckCXXCompilerFlag)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        check_cxx_compiler_flag("-mcpu=native" LOB_HAS_MCPU_NATIVE)
        if(LOB_HAS_MCPU_NATIVE)
            target_compile_options(${target} PRIVATE -mcpu=native)
        endif()
    else()
        check_cxx_compiler_flag("-march=native" LOB_HAS_MARCH_NATIVE)
        if(LOB_HAS_MARCH_NATIVE)
            target_compile_options(${target} PRIVATE -march=native)
        endif()
    endif()
endfunction()

if(LOB_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
