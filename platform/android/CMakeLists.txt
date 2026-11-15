# libferret.so for the Android APK; configured by build.sh with the NDK toolchain file, once per ABI.
cmake_minimum_required(VERSION 3.20)
project(ferret_android C)

set(FR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# fercore, FR_VERSION and fr_embed_assets come from the desktop build; its programs and tests are left out.
set(FR_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(${FR_ROOT} native EXCLUDE_FROM_ALL)
# fr_embed_assets reads FR_ASSETS from the caller's scope
set(FR_ASSETS ${FR_ROOT}/assets)
get_target_property(FR_DEFS fercore INTERFACE_COMPILE_DEFINITIONS)
string(REGEX REPLACE ".*FR_VERSION=\"?([^\";]*)\"?.*" "\\1" FR_VERSION_STRING "${FR_DEFS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt "${FR_VERSION_STRING}\n")

# raylib 6.2 for PLATFORM_ANDROID, as its own CMake builds it
set(RAYLIB_SRC ${FR_ROOT}/thirdparty/raylib/src)
add_library(raylib STATIC ${RAYLIB_SRC}/rcore.c ${RAYLIB_SRC}/rshapes.c ${RAYLIB_SRC}/rtext.c ${RAYLIB_SRC}/rtextures.c
  ${RAYLIB_SRC}/raudio.c ${RAYLIB_SRC}/rmodels.c ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
set_target_properties(raylib PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_definitions(raylib PUBLIC PLATFORM_ANDROID GRAPHICS_API_OPENGL_ES2)
target_include_directories(raylib PUBLIC ${RAYLIB_SRC} ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(raylib PRIVATE -fno-strict-aliasing -Werror=implicit-function-declaration -Werror=pointer-arith)
# rcore_android.c defines __wrap_fopen (APK assets) on top of __real_fopen
target_link_options(raylib PUBLIC -Wl,--wrap=fopen)
target_link_libraries(raylib PUBLIC log android EGL GLESv2 OpenSLES m)

fr_embed_assets(${CMAKE_CURRENT_BINARY_DIR}/generated)
file(GLOB FR_UI_SOURCES CONFIGURE_DEPENDS ${FR_ROOT}/src/ui/*.c ${FR_ROOT}/src/app/*.c)
add_library(ferret SHARED ${FR_UI_SOURCES} android_main.c ${FR_ASSET_SOURCES})
target_include_directories(ferret PRIVATE ${FR_ROOT}/src/ui ${FR_ROOT}/src/app ${FR_ROOT}/thirdparty/nanosvg)
target_compile_options(ferret PRIVATE -Wall -Wextra -Wno-unused-parameter)
# -u keeps the glue's entry point, which nothing in the library references
target_link_options(ferret PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,--gc-sections)
# libarchive and its codecs (fercore's, static) stay internal to the library
target_link_options(ferret PRIVATE -Wl,--exclude-libs,libarchive.a:libzstd.a:liblzma.a:libbz2.a)
target_link_libraries(ferret PRIVATE fercore raylib android log m)
