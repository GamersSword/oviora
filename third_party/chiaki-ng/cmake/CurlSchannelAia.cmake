# Apply the tracked extension to a build-local copy, leaving the submodule clean.
find_package(Git REQUIRED)
set(_aia_patch "${CMAKE_CURRENT_LIST_DIR}/curl-schannel-aia.patch")
set(CHIAKI_CURL_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/curl-patched-source")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/curl/"
    DESTINATION "${CHIAKI_CURL_SOURCE_DIR}" PATTERN ".git" EXCLUDE)
# Refresh patch inputs even if the previous patched copy has newer timestamps.
foreach(_input include/curl/curl.h lib/vtls/schannel_verify.c)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/curl/${_input}"
        "${CHIAKI_CURL_SOURCE_DIR}/${_input}" COPYONLY)
endforeach()
# The copy can lie inside this repository's build folder. git apply run there works relative to the
# enclosing repository and silently skips ("Skipped patch") every file outside the current directory,
# leaving curl unpatched. Make the copy its own (empty) repository so paths are relative to it.
if(NOT EXISTS "${CHIAKI_CURL_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" init -q
        WORKING_DIRECTORY "${CHIAKI_CURL_SOURCE_DIR}"
        RESULT_VARIABLE _aia_init_result)
    if(NOT _aia_init_result EQUAL 0)
        message(FATAL_ERROR "Failed to prepare the bundled curl copy for the Schannel AIA patch")
    endif()
endif()
set(_aia_git "${GIT_EXECUTABLE}")
execute_process(COMMAND ${_aia_git} apply --check "${_aia_patch}"
    WORKING_DIRECTORY "${CHIAKI_CURL_SOURCE_DIR}"
    RESULT_VARIABLE _can_apply)
if(NOT _can_apply EQUAL 0)
    message(FATAL_ERROR "Bundled curl does not match the Schannel AIA patch")
endif()
execute_process(COMMAND ${_aia_git} apply "${_aia_patch}"
    WORKING_DIRECTORY "${CHIAKI_CURL_SOURCE_DIR}"
    RESULT_VARIABLE _apply_result)
if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply the Schannel AIA patch")
endif()
file(STRINGS "${CHIAKI_CURL_SOURCE_DIR}/include/curl/curl.h" _aia_flag REGEX "CURLSSLOPT_SCHANNEL_AIA")
if(NOT _aia_flag)
    message(FATAL_ERROR "The Schannel AIA patch was not applied to the bundled curl copy")
endif()
