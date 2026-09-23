# OpenPak: an OpenSSL for the Android build of openpak-client.
#
# The NDK ships no OpenSSL, and the library speaks HTTPS through cpp-httplib's OpenSSL backend.
# This takes the same prebuilt static OpenSSL Eden's Android build uses (crueter-ci/OpenSSL),
# pinned by version and SHA-512 so a CI run and a local build get the same bytes. The package
# also carries <openssl/cert.h>, the public root store the library loads on Android (there is no
# CA bundle file to point OpenSSL at on a phone).

set(OPENPAK_OPENSSL_VERSION "4.0.1-1788234794-b64f68a94e")

if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(_openpak_openssl_arch "aarch64")
  set(_openpak_openssl_sha512 "beda26d4151196a980da67bc595be7b538dead4d243f97288fec4e04204dc17566608ed5db1a48c95b6fe140a627a906926667c9479db6f0f2d66a7a6e6cad2a")
elseif(ANDROID_ABI STREQUAL "x86_64")
  set(_openpak_openssl_arch "amd64")
  set(_openpak_openssl_sha512 "e01b72f9e2f72b1b91027d1e2ac1e81d755f0058d151d00e4fa51165e10fdc92725adf5036b6a943e3694c5c0d032819c3ef8b68bc35cec82f60440eb29fe1d9")
else()
  message(FATAL_ERROR "OpenPak: no prebuilt OpenSSL for ANDROID_ABI ${ANDROID_ABI}")
endif()

set(_openpak_openssl_name "openssl-android-${_openpak_openssl_arch}-${OPENPAK_OPENSSL_VERSION}")
set(_openpak_openssl_dir "${CMAKE_BINARY_DIR}/openpak-openssl/${_openpak_openssl_name}")
set(_openpak_openssl_archive "${CMAKE_BINARY_DIR}/openpak-openssl/${_openpak_openssl_name}.tar.zst")

if(NOT EXISTS "${_openpak_openssl_dir}/lib/libssl.a")
  file(DOWNLOAD
    "https://github.com/crueter-ci/OpenSSL/releases/download/${OPENPAK_OPENSSL_VERSION}/${_openpak_openssl_name}.tar.zst"
    "${_openpak_openssl_archive}"
    EXPECTED_HASH SHA512=${_openpak_openssl_sha512}
    STATUS _openpak_openssl_status)
  list(GET _openpak_openssl_status 0 _openpak_openssl_error)
  if(_openpak_openssl_error)
    message(FATAL_ERROR "OpenPak: could not download ${_openpak_openssl_name}: ${_openpak_openssl_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_openpak_openssl_archive}" DESTINATION "${_openpak_openssl_dir}")
  file(REMOVE "${_openpak_openssl_archive}")
endif()

# FindOpenSSL takes these as given; the NDK toolchain's re-rooted search would not find them.
set(OPENSSL_USE_STATIC_LIBS ON)
set(OPENSSL_ROOT_DIR "${_openpak_openssl_dir}" CACHE PATH "" FORCE)
set(OPENSSL_INCLUDE_DIR "${_openpak_openssl_dir}/include" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${_openpak_openssl_dir}/lib/libcrypto.a" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${_openpak_openssl_dir}/lib/libssl.a" CACHE FILEPATH "" FORCE)

# The library's default cpp-httplib (0.18) predates OpenSSL 4; take the release Eden's Android
# build pairs with this OpenSSL. The first FetchContent declaration of a name wins, so the
# library's own declaration below is then a no-op.
include(FetchContent)
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.46.0 GIT_SHALLOW TRUE)
