cmake_minimum_required(VERSION 3.10)
project(headfetch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(FetchContent)
FetchContent_Declare(libhat
    GIT_REPOSITORY https://github.com/BasedInc/libhat.git
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.1
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(libhat imgui)

# --- networking (avatar/head fetch) ---------------------------------------
# mbedTLS provides the TLS backend for curl, since the Android NDK does not
# expose OpenSSL/BoringSSL headers to apps.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.2
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(mbedtls)

# curl's bundled CMake/FindMbedTLS.cmake only looks for a *system-installed*
# mbedtls (via find_library/pkg-config). Since we're building mbedtls
# in-tree via FetchContent instead of installing it, short-circuit that
# search and point it straight at the targets we just defined.
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY "$<TARGET_FILE:mbedtls>" CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY "$<TARGET_FILE:mbedx509>" CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY "$<TARGET_FILE:mbedcrypto>" CACHE STRING "" FORCE)

# zlib: curl would otherwise use this for Content-Encoding: gzip/deflate
# support. Left enabled, curl's find_package(ZLIB) picks up the Android
# NDK's libz.so stub, which is a link-only shim -- the real libz.so is
# expected to be provided by the Android system at runtime. mcpelauncher's
# environment doesn't resolve that the way real Android does, so the mod
# ends up with an unresolved "zlibVersion" symbol at dlopen time and fails
# to load entirely. We only ever fetch PNG avatar images, which are
# already compressed and essentially never served gzip-encoded, so simply
# disabling zlib support in curl avoids the whole problem instead of
# fighting to statically link it in (IMPORTED_LOCATION doesn't support
# generator expressions the way MBEDTLS_LIBRARY's usage above does,
# which is what broke the generated Makefile with a stray "$<TARGET_FILE:
# zlibstatic>" leaking into a build rule).
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The Android NDK's headers declare CLOCK_MONOTONIC_RAW, so curl's
# configure-time check detects it and prefers clock_gettime(CLOCK_
# MONOTONIC_RAW, ...) for its internal timing (see lib/timeval.c) --
# this is a long-standing curl-on-Android footgun, see
# https://github.com/curl/curl/issues/13826. mcpelauncher's libc-shim
# (which intercepts clock_gettime in its "fake Android" environment)
# only implements CLOCK_REALTIME and CLOCK_MONOTONIC, so it throws
# "Unexpected clock type 4" and aborts (SIGABRT) the very first time
# curl times anything. Force this detection off so curl falls back to
# plain CLOCK_MONOTONIC, which the shim does support.
set(HAVE_CLOCK_GETTIME_MONOTONIC_RAW OFF CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
# We only ever hit hardcoded ASCII hostnames, and none of these are
# available/needed for an Android NDK cross-build -- disabling them avoids
# pulling in system libidn2/libssh2/wolfssh/nghttp2/brotli/zstd/libpsl,
# which is what caused the "-lidn2" link failure.
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(CURL_USE_WOLFSSH OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_10_1
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(curl)
if(NOT TARGET CURL::libcurl)
    # Older/newer curl CMake versions may not export this alias; fall back
    # to the raw target name curl's own CMakeLists defines.
    add_library(CURL::libcurl ALIAS libcurl)
endif()
# The MBEDTLS_*_LIBRARY variables above are TARGET_FILE generator
# expressions, not real filesystem paths, so CMake can't infer build order
# from them the way it would from linking a target by name. Force it.
# "libcurl" itself is commonly an ALIAS (e.g. for libcurl_static), and
# add_dependencies() can't target an alias directly, so resolve the real
# target first.
get_target_property(_curl_real_target libcurl ALIASED_TARGET)
if(NOT _curl_real_target)
    set(_curl_real_target libcurl)
endif()
add_dependencies(${_curl_real_target} mbedtls mbedx509 mbedcrypto)

# stb_image: header-only PNG decoder, no CMake target of its own.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(stb)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES2)
target_compile_options(imgui PRIVATE -Os -ffunction-sections -fdata-sections -fvisibility=hidden)
target_link_libraries(imgui PUBLIC GLESv2)

add_library(headfetch SHARED
    src/main.cpp
    src/net/stb_image_impl.cpp
    src/net/memrchr_shim.cpp
)
target_include_directories(headfetch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${stb_SOURCE_DIR})
target_compile_options(headfetch PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_libraries(headfetch PUBLIC libhat::libhat imgui GLESv2 android EGL log CURL::libcurl)
target_link_options(headfetch PRIVATE
    -static-libstdc++
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,-s
)
set_target_properties(headfetch PROPERTIES OUTPUT_NAME headfetch)
install(TARGETS headfetch LIBRARY DESTINATION mods)
