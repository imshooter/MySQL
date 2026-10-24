cmake_minimum_required(VERSION 3.19)
project(mysql VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(ExternalProject)
include(FetchContent)

FetchContent_Declare(omp-sdk
	GIT_REPOSITORY https://github.com/openmultiplayer/open.mp-sdk.git
	GIT_TAG 08cc69a32d96c0eb8fe063d26c78259e54ca853f
)
FetchContent_Declare(pawn
	GIT_REPOSITORY https://github.com/openmultiplayer/compiler.git
	GIT_TAG 29f1a8c7fd2e01929807cd8c50353fbc93bbd651
	SOURCE_SUBDIR none
)
FetchContent_Declare(mariadbcc
	GIT_REPOSITORY https://github.com/mariadb-corporation/mariadb-connector-c.git
	GIT_TAG v3.4.11
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(omp-sdk pawn mariadbcc)

set(MARIADB_BUILD "${CMAKE_BINARY_DIR}/mariadb")
set(MARIADB_LIBRARY "${MARIADB_BUILD}/libmariadb/Release/mariadbclient.lib")
file(TO_NATIVE_PATH "${mariadbcc_SOURCE_DIR}/" MARIADB_SOURCE_PREFIX)
ExternalProject_Add(mariadb
	SOURCE_DIR "${mariadbcc_SOURCE_DIR}"
	BINARY_DIR "${MARIADB_BUILD}"
	CMAKE_ARGS
		"-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /d1trimfile:${MARIADB_SOURCE_PREFIX}"
		-DWITH_UNIT_TESTS=OFF
		-DWITH_CURL=OFF
		-DWITH_SSL=SCHANNEL
		-DWITH_EXTERNAL_ZLIB=OFF
		-DCLIENT_PLUGIN_DIALOG=STATIC
		-DCLIENT_PLUGIN_CLIENT_ED25519=STATIC
		-DCLIENT_PLUGIN_CACHING_SHA2_PASSWORD=STATIC
		-DCLIENT_PLUGIN_SHA256_PASSWORD=STATIC
		-DCLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD=STATIC
		-DCLIENT_PLUGIN_PVIO_NPIPE=STATIC
		-DCLIENT_PLUGIN_PVIO_SHMEM=STATIC
		-DCLIENT_PLUGIN_AUTH_GSSAPI_CLIENT=OFF
		-DCLIENT_PLUGIN_MYSQL_OLD_PASSWORD=OFF
		-DCLIENT_PLUGIN_REMOTE_IO=OFF
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target mariadbclient
	INSTALL_COMMAND ""
	BUILD_BYPRODUCTS "${MARIADB_LIBRARY}"
)

add_library(passwords STATIC
	external/argon2/src/argon2.c
	external/argon2/src/core.c
	external/argon2/src/encoding.c
	external/argon2/src/opt.c
	external/argon2/src/thread.c
	external/argon2/src/blake2/blake2b.c
	external/crypt_blowfish/crypt_blowfish.c
)
target_include_directories(passwords
	PUBLIC external/argon2/include external/crypt_blowfish
	PRIVATE external/argon2/src
)

add_library(mysql SHARED
	src/connection.cpp
	src/format.cpp
	src/main.cpp
	src/natives.cpp
	src/passwords.cpp
	src/results.cpp
	external/yyjson/yyjson.c
)
set_target_properties(mysql PROPERTIES PREFIX "")
add_dependencies(mysql mariadb)
target_include_directories(mysql PRIVATE
	"${pawn_SOURCE_DIR}/source"
	"${pawn_SOURCE_DIR}/source/linux"
	"${mariadbcc_SOURCE_DIR}/include"
	"${MARIADB_BUILD}/include"
	external/yyjson
)
target_compile_definitions(mysql PRIVATE
	HAVE_STDINT_H=1
	PAWN_CELL_SIZE=32
	COMPONENT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	COMPONENT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	COMPONENT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
	COMPONENT_VERSION="${PROJECT_VERSION}"
)
if(WIN32)
	target_compile_definitions(mysql PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
	target_compile_options(mysql PRIVATE /W3 /utf-8)
endif()
target_link_libraries(mysql PRIVATE OMP-SDK passwords "${MARIADB_LIBRARY}" ws2_32 advapi32 shlwapi crypt32 bcrypt secur32)
