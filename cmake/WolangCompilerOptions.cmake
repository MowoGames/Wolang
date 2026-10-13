# Shared compiler configuration for every Wolang target.
#
# The project follows the Chromium C++ style guide, which bans exceptions and
# RTTI, so both are disabled globally. Code must use status/error-code returns
# instead of throwing, and must not rely on dynamic_cast or typeid.

function(wolang_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  if(MSVC)
    target_compile_options(${target} PRIVATE
        /W4 /permissive- /EHs-c- /GR- /utf-8 /Zc:preprocessor)
    target_compile_definitions(${target} PRIVATE
        _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN)
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
