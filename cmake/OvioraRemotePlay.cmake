include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_library(oviora_remoteplay_core STATIC
    "${OVIORA_ROOT}/src/remoteplay/Validation.cpp"
    "${OVIORA_ROOT}/src/remoteplay/AnnexB.cpp"
    "${OVIORA_ROOT}/src/remoteplay/Timeline.cpp"
    "${OVIORA_ROOT}/src/remoteplay/VideoIngress.cpp"
    "${OVIORA_ROOT}/src/remoteplay/SessionInbox.cpp")
target_include_directories(oviora_remoteplay_core PUBLIC "${OVIORA_ROOT}/include")
target_compile_features(oviora_remoteplay_core PUBLIC cxx_std_20)
set_target_properties(oviora_remoteplay_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(oviora_remoteplay_core PUBLIC Threads::Threads)
# Shared pinned native dependency for the product and probes. No stub fallback.
enable_language(C)
# Oviora's own chiaki-ng fork, vendored in the repository.
set(OVIORA_RP_CHIAKI_SOURCE_DIR "${OVIORA_ROOT}/third_party/chiaki-ng")
if(NOT EXISTS "${OVIORA_RP_CHIAKI_SOURCE_DIR}/lib/include/chiaki/session.h")
    message(FATAL_ERROR "OVIORA_RP_BUILD_NATIVE needs the chiaki-ng source tree")
endif()
# Target C++20 requirements remain explicit despite upstream defaults.
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_ANDROID OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_RUDP OFF CACHE STRING "" FORCE)
set(CHIAKI_LIB_ENABLE_OPUS ON CACHE BOOL "" FORCE)
set(CHIAKI_LIB_ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_CURL OFF CACHE STRING "" FORCE)
# Disabling RUDP does NOT remove curl/json-c/libevent/miniupnpc in the pinned lib.
# Provision their REAL matching-toolchain dev libraries; never replace them with
# empty imported targets or remove FEC to force the native gate green.
add_subdirectory("${OVIORA_RP_CHIAKI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/chiaki" EXCLUDE_FROM_ALL)
if(NOT TARGET chiaki-lib)
    message(FATAL_ERROR "Upstream chiaki-lib target was not created")
endif()
# vcpkg's static miniupnpc headers default to dllimport on Windows unless the
# caller opts into the static ABI. Without this definition the static library
# is found correctly but Chiaki emits unresolved __imp_* references.
target_compile_definitions(chiaki-lib PRIVATE MINIUPNP_STATICLIB)
add_library(oviora_remoteplay_chiaki STATIC
    "${OVIORA_ROOT}/src/remoteplay/ChiakiBackend.cpp"
    "${OVIORA_ROOT}/src/remoteplay/ChiakiRegistration.cpp"
    "${OVIORA_ROOT}/src/remoteplay/ProfileStore.cpp"
    "${OVIORA_ROOT}/src/remoteplay/Discovery.cpp")
target_compile_features(oviora_remoteplay_chiaki PUBLIC cxx_std_20)
target_compile_definitions(oviora_remoteplay_chiaki PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(oviora_remoteplay_chiaki PUBLIC oviora_remoteplay_core PRIVATE chiaki-lib crypt32 iphlpapi shell32 ole32 winhttp)
