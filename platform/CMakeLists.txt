# The window, input, pixels, GPU and sound layer.
#
# Its own shared library rather than part of the runtime: a program that
# draws nothing should link nothing, and a compiler that pulled in Cocoa
# to print a number would be paying for a window it never opens (D155).
#
# One backend per system, chosen here rather than by `#ifdef` inside one
# file: a file that is three systems at once is one nobody can read.

set(WOLANG_PLATFORM_SOURCES common.cc)
set(WOLANG_PLATFORM_LIBS "")
set(WOLANG_PLATFORM_BACKEND "none")

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND WOLANG_PLATFORM_SOURCES cocoa.mm)
  set(WOLANG_PLATFORM_BACKEND "cocoa")
  list(APPEND WOLANG_PLATFORM_LIBS
       "-framework AppKit"
       "-framework CoreGraphics"
       "-framework AudioToolbox"
       "-framework AudioUnit"
       "-framework OpenGL")
elseif(WIN32)
  list(APPEND WOLANG_PLATFORM_SOURCES win32.cc)
  set(WOLANG_PLATFORM_BACKEND "win32")
  list(APPEND WOLANG_PLATFORM_LIBS user32 gdi32 opengl32 winmm)
else()
  # X11 and ALSA are looked for rather than required: a container with no
  # display still builds the language, and `Available()` answers false.
  find_path(WOLANG_X11_INCLUDE X11/Xlib.h)
  find_library(WOLANG_X11_LIB X11)
  find_library(WOLANG_ASOUND_LIB asound)
  if(WOLANG_X11_INCLUDE AND WOLANG_X11_LIB)
    list(APPEND WOLANG_PLATFORM_SOURCES x11.cc)
    set(WOLANG_PLATFORM_BACKEND "x11")
    list(APPEND WOLANG_PLATFORM_LIBS ${WOLANG_X11_LIB} dl)
    if(WOLANG_ASOUND_LIB)
      list(APPEND WOLANG_PLATFORM_LIBS ${WOLANG_ASOUND_LIB})
    endif()
  else()
    list(APPEND WOLANG_PLATFORM_SOURCES none.cc)
  endif()
endif()

if(WOLANG_PLATFORM_BACKEND STREQUAL "none" AND NOT APPLE AND NOT WIN32)
  message(STATUS "wolang: no window backend here; `gfx` will say so")
endif()

add_library(wolang_platform SHARED ${WOLANG_PLATFORM_SOURCES})
target_include_directories(wolang_platform
                           PUBLIC ${PROJECT_SOURCE_DIR}/include
                           PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(wolang_platform PRIVATE ${WOLANG_PLATFORM_LIBS})
set_target_properties(wolang_platform PROPERTIES
                      CXX_VISIBILITY_PRESET hidden
                      OUTPUT_NAME wolang_platform)
if(WOLANG_X11_INCLUDE AND WOLANG_PLATFORM_BACKEND STREQUAL "x11")
  target_include_directories(wolang_platform PRIVATE ${WOLANG_X11_INCLUDE})
  if(WOLANG_ASOUND_LIB)
    target_compile_definitions(wolang_platform PRIVATE WOLANG_HAVE_ALSA=1)
  endif()
endif()

install(TARGETS wolang_platform
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/wolang/platform
        DESTINATION include/wolang)
