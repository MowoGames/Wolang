# Shared compiler configuration for every Wolang target.
#
# The project is built with clang -- see the check in the top-level
# CMakeLists -- and follows the Chromium C++ style guide, which bans
# exceptions and RTTI, so both are disabled globally. Code must use
# status/error-code returns instead of throwing, and must not rely on
# dynamic_cast or typeid.
#
# Two spellings of the same thing: clang-cl takes Microsoft's flags and
# clang everywhere else takes the usual ones, so the `MSVC` branch below
# is the *driver*, not the compiler (D184).

function(wolang_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  if(MSVC)
    # clang-cl: Microsoft's flag spelling, clang's compiler.
    target_compile_options(${target} PRIVATE
        /W4 /permissive- /EHs-c- /GR- /utf-8)
    target_compile_definitions(${target} PRIVATE
        _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN
        # The C library's own functions are not deprecated here; this
        # project uses them deliberately and checks what they answer.
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    if(WOLANG_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wno-unused-parameter
        -fno-exceptions
        -fno-rtti)
    if(WOLANG_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(WOLANG_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE
          -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
