
include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(DISABLE_EXTRA_LIBS ON CACHE BOOL "" FORCE)
set(DISABLE_BSON_OUTPUT ON CACHE BOOL "" FORCE)

FetchContent_Declare(json-c
	URL https://github.com/json-c/json-c/archive/refs/tags/json-c-0.17-20230812.tar.gz
	URL_HASH SHA256=024d302a3aadcbf9f78735320a6d5aedf8b77876c8ac8bbb95081ca55054c7eb
)
FetchContent_MakeAvailable(json-c)

# Chiaki includes headers as <json-c/json_object.h> but FetchContent provides
# them without the json-c/ prefix. Create a symlink bridge directory.
set(JSONC_PREFIX_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/json-c-prefix-include")
file(MAKE_DIRECTORY "${JSONC_PREFIX_INCLUDE_DIR}")
if(NOT EXISTS "${JSONC_PREFIX_INCLUDE_DIR}/json-c")
	if(CMAKE_HOST_WIN32)
		# Symlinks need Developer Mode or admin rights on Windows build hosts; a copy works the same
		file(COPY "${json-c_SOURCE_DIR}/" DESTINATION "${JSONC_PREFIX_INCLUDE_DIR}/json-c")
	else()
		file(CREATE_LINK "${json-c_SOURCE_DIR}" "${JSONC_PREFIX_INCLUDE_DIR}/json-c" SYMBOLIC)
	endif()
endif()
