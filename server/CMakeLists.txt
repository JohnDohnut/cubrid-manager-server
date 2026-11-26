#
#  Copyright 2016 CUBRID Corporation
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#

# Builds the CUBRID Manager server either as part of a CUBRID build (add_subdirectory
# from the top-level CMakeLists.txt) or on its own through build_cmake.sh, which points
# this at a CUBRID build or install tree.
#
# The autotools build in this directory is left in place: 11.4 and earlier drive it from
# their own CMakeLists.txt and must keep working unchanged.

cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMS_STANDALONE TRUE)
  project(CUBRID-Manager-Server C CXX)
else()
  set(CMS_STANDALONE FALSE)
endif()

if(NOT UNIX)
  message(FATAL_ERROR "The CMake build of the manager server is UNIX only; use build.bat on Windows.")
endif()

# Do not inherit the server's compile flags and include path when built in its tree. Its
# warning set is -Werror, which this code has never been held to, and its generated
# version.h would land on the include path and clash with the manager's own version
# macros. Both were absent when this was built as an ExternalProject, so keep it that
# way. Build-type flags (-O2, -g) are separate variables and are still inherited.
set(CMAKE_C_FLAGS "")
set(CMAKE_CXX_FLAGS "")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

find_package(Threads REQUIRED)

# config.h, which cm_porting.h includes. The automake build gets it from autoconf; here
# only the macros the sources actually test are checked - see cmake/config.h.cmake.
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckTypeSize)
check_function_exists(bzero HAVE_BZERO)
check_function_exists(memset HAVE_MEMSET)
check_function_exists(getopt_long HAVE_GETOPT_LONG)
check_include_file(getopt.h HAVE_GETOPT_H)
check_include_file(netinet/in.h HAVE_NETINET_IN_H)
check_include_file(sys/cdefs.h HAVE_SYS_CDEFS_H)
check_type_size(int64_t INT64_T)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/config.h)

set(CMS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(CMS_EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)

# --------------------------------------------------------------------------- version
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER CMS_BUILD_NUMBER_FILE)
string(STRIP "${CMS_BUILD_NUMBER_FILE}" CMS_BUILD_NUMBER_FILE)
string(REGEX REPLACE "\\.[0-9]+$" "" CMS_RELEASE_STRING "${CMS_BUILD_NUMBER_FILE}")
string(REGEX REPLACE "\\.[0-9]+$" "" CMS_MAJOR_RELEASE_STRING "${CMS_RELEASE_STRING}")
string(REPLACE "." ";" CMS_VERSION_PARTS "${CMS_RELEASE_STRING}")
list(GET CMS_VERSION_PARTS 0 CMS_MAJOR_VERSION)
list(GET CMS_VERSION_PARTS 1 CMS_MINOR_VERSION)
list(GET CMS_VERSION_PARTS 2 CMS_PATCH_VERSION)

execute_process(COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CMS_COMMIT_COUNT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CMS_COMMIT_COUNT)
  set(CMS_COMMIT_COUNT 0)
endif()
string(REGEX REPLACE "^([0-9]+)$" "\\1" CMS_COMMIT_COUNT "${CMS_COMMIT_COUNT}")
string(LENGTH "${CMS_COMMIT_COUNT}" CMS_COMMIT_COUNT_LEN)
while(CMS_COMMIT_COUNT_LEN LESS 4)
  set(CMS_COMMIT_COUNT "0${CMS_COMMIT_COUNT}")
  string(LENGTH "${CMS_COMMIT_COUNT}" CMS_COMMIT_COUNT_LEN)
endwhile()
set(CMS_BUILD_NUMBER "${CMS_RELEASE_STRING}.${CMS_COMMIT_COUNT}")
message(STATUS "CUBRID Manager server version: ${CMS_BUILD_NUMBER}")

# ------------------------------------------------------- CUBRID and OpenSSL locations
#
# In-tree: take them straight from the targets this build defines.
# Standalone: CUBRID_INCLUDE_DIR / CUBRID_LIB_DIR / OPENSSL_* are resolved by
# build_cmake.sh (or passed by hand) and pointed at a build or install tree.
include(ExternalProject)
set(CMS_3RDPARTY_DIR ${CMAKE_CURRENT_BINARY_DIR}/3rdparty)
if(CMS_STANDALONE)
  # Off: OPENSSL_* point at the OpenSSL of a CUBRID build tree (build_cmake.sh).
  # On: OpenSSL is built here from the same tarball and hash the engine's 3rdparty uses, for building
  # against an installed CUBRID, which ships no OpenSSL to link with.
  option(CMS_STATIC_OPENSSL "Build OpenSSL from source and link it statically (ignores OPENSSL_*)" OFF)
  set(CMS_REQUIRED CUBRID_INCLUDE_DIR CUBRID_LIB_DIR)
  if(NOT CMS_STATIC_OPENSSL)
    list(APPEND CMS_REQUIRED OPENSSL_INCLUDE_DIR OPENSSL_LIBRARIES)
  endif()
  foreach(required ${CMS_REQUIRED})
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
      message(FATAL_ERROR
        "${required} is not set. Run build_cmake.sh, or pass -D${required}=... pointing at a CUBRID build or installation.")
    endif()
  endforeach()
  set(CMS_CUBRID_INCLUDES ${CUBRID_INCLUDE_DIR})
  if(CMS_STATIC_OPENSSL)
    ExternalProject_Add(cms_openssl
      URL                  "https://github.com/CUBRID/3rdparty/raw/develop/openssl/openssl-3.5.7.tar.gz"
      URL_HASH             "SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
      PREFIX               ${CMS_3RDPARTY_DIR}/openssl
      LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE
      TIMEOUT 1800 INACTIVITY_TIMEOUT 60 DOWNLOAD_NO_PROGRESS 1
      # OPENSSLDIR points where no customer machine has an openssl.cnf, so nothing is read from the system
      CONFIGURE_COMMAND    <SOURCE_DIR>/config --prefix=${CMS_3RDPARTY_DIR} --libdir=lib --openssldir=/nonexistent/cubrid-cm-ssh no-shared no-module no-tests no-docs
      BUILD_COMMAND        make -j4 build_sw
      # a plain make under the parent "make -j" fails with "write jobserver: Bad file descriptor"
      INSTALL_COMMAND      ${CMAKE_COMMAND} -E env MAKEFLAGS= MFLAGS= make install_sw
      BUILD_BYPRODUCTS     ${CMS_3RDPARTY_DIR}/lib/libssl.a ${CMS_3RDPARTY_DIR}/lib/libcrypto.a
      )
    set(CMS_OPENSSL_TARGET cms_openssl)
    set(CMS_OPENSSL_ROOT ${CMS_3RDPARTY_DIR})
    set(CMS_OPENSSL_INCLUDES ${CMS_3RDPARTY_DIR}/include)
    set(CMS_OPENSSL_LIBS ${CMS_3RDPARTY_DIR}/lib/libssl.a ${CMS_3RDPARTY_DIR}/lib/libcrypto.a)
  else()
    set(CMS_OPENSSL_TARGET "")
    get_filename_component(CMS_OPENSSL_ROOT ${OPENSSL_INCLUDE_DIR} DIRECTORY)
    set(CMS_OPENSSL_INCLUDES ${OPENSSL_INCLUDE_DIR})
    set(CMS_OPENSSL_LIBS ${OPENSSL_LIBRARIES})
  endif()
  find_library(CMS_CUBRIDCS_LIB NAMES cubridcs PATHS ${CUBRID_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
  find_library(CMS_CMDEP_LIB NAMES cmdep PATHS ${CUBRID_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
  find_library(CMS_CMSTAT_LIB NAMES cmstat PATHS ${CUBRID_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
  set(CMS_CUBRID_LIBS ${CMS_CUBRIDCS_LIB} ${CMS_CMDEP_LIB} ${CMS_CMSTAT_LIB})
  message(STATUS "CUBRID headers: ${CMS_CUBRID_INCLUDES}")
  message(STATUS "CUBRID libraries: ${CMS_CUBRID_LIBS}")
  message(STATUS "OpenSSL: ${CMS_OPENSSL_LIBS}")
else()
  # Public headers of the libraries this links against are not installed yet at build
  # time, so read them off the targets.
  get_target_property(CMS_CMDEP_HEADER cmdep PUBLIC_HEADER)
  get_target_property(CMS_CMSTAT_HEADER cmstat PUBLIC_HEADER)
  set(CMS_CUBRID_INCLUDES "")
  foreach(header ${CMS_CMDEP_HEADER} ${CMS_CMSTAT_HEADER})
    get_filename_component(header_dir ${header} DIRECTORY)
    list(APPEND CMS_CUBRID_INCLUDES ${header_dir})
  endforeach()
  list(REMOVE_DUPLICATES CMS_CUBRID_INCLUDES)
  set(CMS_CUBRID_LIBS cubridcs cmdep cmstat)
  set(CMS_OPENSSL_INCLUDES ${LIBOPENSSL_INCLUDES})
  set(CMS_OPENSSL_LIBS ${LIBOPENSSL_LIBS})
  set(CMS_OPENSSL_TARGET ${LIBOPENSSL_TARGET})
  set(CMS_OPENSSL_ROOT ${3RDPARTY_LIBS_DIR})
endif()

# libssh2 (BSD-3-Clause) for cm_ssh, the ssh/sftp helper of the HA node-adding script. Built from source and
# linked statically against the same OpenSSL; nothing of ssh is taken from the system.
ExternalProject_Add(cms_libssh2
  URL                  "https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz"
  URL_HASH             "SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7"
  PREFIX               ${CMS_3RDPARTY_DIR}/libssh2
  DEPENDS              ${CMS_OPENSSL_TARGET}
  LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE
  TIMEOUT 600 INACTIVITY_TIMEOUT 60 DOWNLOAD_NO_PROGRESS 1
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${CMS_3RDPARTY_DIR}/ssh2
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
    -DBUILD_EXAMPLES=OFF
    -DBUILD_TESTING=OFF
    -DENABLE_ZLIB_COMPRESSION=OFF
    -DCRYPTO_BACKEND=OpenSSL
    -DOPENSSL_ROOT_DIR=${CMS_OPENSSL_ROOT}
    -DOPENSSL_USE_STATIC_LIBS=ON
  BUILD_BYPRODUCTS     ${CMS_3RDPARTY_DIR}/ssh2/lib/libssh2.a
  )
set(CMS_LIBSSH2_INCLUDES ${CMS_3RDPARTY_DIR}/ssh2/include)
set(CMS_LIBSSH2_LIBS ${CMS_3RDPARTY_DIR}/ssh2/lib/libssh2.a)

# jsoncpp and libevent stay as the prebuilt archives committed under external/.
# libevent_openssl only references OpenSSL's stable public API, so it links against
# 3.x unchanged.
set(CMS_EXTERNAL_INCLUDES
  ${CMS_EXTERNAL_DIR}/jsoncpp/linux_64/include
  ${CMS_EXTERNAL_DIR}/libevent/linux_64/include
  )
set(CMS_EXTERNAL_LIBS
  ${CMS_EXTERNAL_DIR}/jsoncpp/linux_64/lib/libjson.a
  ${CMS_EXTERNAL_DIR}/libevent/linux_64/lib/libevent.a
  ${CMS_EXTERNAL_DIR}/libevent/linux_64/lib/libevent_openssl.a
  )

# ------------------------------------------------------------------------ definitions
set(CMS_SYS_DEFS GCC LINUX _GNU_SOURCE I386 _GLIBCXX_USE_CXX11_ABI=0)
set(CMS_COMMON_DEFS ANSI=1 SYSV MAXPATHLEN=1024 _REENTRANT)
set(CMS_VERSION_DEFS
  MAJOR_VERSION=${CMS_MAJOR_VERSION}
  MINOR_VERSION=${CMS_MINOR_VERSION}
  PATCH_VERSION=${CMS_PATCH_VERSION}
  RELEASE_STRING=${CMS_RELEASE_STRING}
  MAJOR_RELEASE_STRING=${CMS_MAJOR_RELEASE_STRING}
  BUILD_NUMBER=${CMS_BUILD_NUMBER}
  )
set(CMS_WARN_FLAGS -Wall -W -Wwrite-strings -Wno-cast-qual -Wno-unused -pthread)

# This directory resets CMAKE_C_FLAGS/CMAKE_CXX_FLAGS above, so the -Wno flags that
# build.sh exports for Clang never reach these targets. cm_compress.h is an
# amalgamated copy of miniz and its initializer lists narrow char to mz_uint8, which
# Clang rejects outright rather than warning about.
#
# -Wno-unused covers -Wunused-parameter under GCC but not under Clang, so the opt-out
# is only needed, and only applied, for Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND CMS_WARN_FLAGS -Wno-c++11-narrowing -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------- targets
set(CM_ADMIN_SOURCES
  ${CMS_SRC_DIR}/cm_text_encryption.cpp
  ${CMS_SRC_DIR}/cm_config.cpp
  ${CMS_SRC_DIR}/cm_server_util.cpp
  ${CMS_SRC_DIR}/cm_user.cpp
  ${CMS_SRC_DIR}/cm_cmd.cpp
  ${CMS_SRC_DIR}/cm_cmd_util.cpp
  ${CMS_SRC_DIR}/cm_connect_info.cpp
  ${CMS_SRC_DIR}/cm_cmd_task.cpp
  ${CMS_SRC_DIR}/cm_server_status.cpp
  ${CMS_SRC_DIR}/cm_task_file.cpp
  )

set(CUB_MANAGER_SOURCES
  ${CMS_SRC_DIR}/cm_server_util.cpp
  ${CMS_SRC_DIR}/cm_autojob.cpp
  ${CMS_SRC_DIR}/cm_text_encryption.cpp
  ${CMS_SRC_DIR}/cm_config.cpp
  ${CMS_SRC_DIR}/cm_job_task.cpp
  ${CMS_SRC_DIR}/cm_cmd_exec.cpp
  ${CMS_SRC_DIR}/cm_server_status.cpp
  ${CMS_SRC_DIR}/cm_mem_cpu_stat.cpp
  ${CMS_SRC_DIR}/cm_connect_info.cpp
  ${CMS_SRC_DIR}/cm_auto_task.cpp
  ${CMS_SRC_DIR}/cm_httpd.cpp
  ${CMS_SRC_DIR}/cm_mon_stat.cpp
  ${CMS_SRC_DIR}/cm_user.cpp
  ${CMS_SRC_DIR}/cm_http_server.cpp
  ${CMS_SRC_DIR}/cm_server_extend_interface.cpp
  ${CMS_SRC_DIR}/cm_server_interface.cpp
  ${CMS_SRC_DIR}/cm_server_autoupdate.cpp
  ${CMS_SRC_DIR}/cm_ssl_cert.cpp
  ${CMS_SRC_DIR}/cm_session_ip.cpp
  ${CMS_SRC_DIR}/cm_os_release.cpp
  ${CMS_SRC_DIR}/cm_log_name.cpp
  ${CMS_SRC_DIR}/cm_task_file.cpp
  ${CMS_SRC_DIR}/cm_ha_add_nodes.cpp
  )

add_executable(cm_admin ${CM_ADMIN_SOURCES})
target_compile_definitions(cm_admin PRIVATE
  ${CMS_SYS_DEFS} ${CMS_COMMON_DEFS} ${CMS_VERSION_DEFS} _FILE_OFFSET_BITS=64)
target_compile_options(cm_admin PRIVATE ${CMS_WARN_FLAGS})
target_include_directories(cm_admin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR} ${CMS_SRC_DIR} ${CMS_CUBRID_INCLUDES} ${CMS_OPENSSL_INCLUDES})
# OpenSSL goes before the CUBRID libraries: libcubridcs of released engines (11.4.5) exports its own bundled
# OpenSSL 1.1.1, and a library listed first would take this program's OpenSSL calls.
target_link_libraries(cm_admin PRIVATE
  ${CMS_OPENSSL_LIBS} ${CMS_CUBRID_LIBS} Threads::Threads ${CMAKE_DL_LIBS})

add_executable(cub_manager ${CUB_MANAGER_SOURCES})
target_compile_definitions(cub_manager PRIVATE
  ${CMS_SYS_DEFS} ${CMS_COMMON_DEFS} ${CMS_VERSION_DEFS}
  FSERVER_SLAVE JSON_SUPPORT EVENT__HAVE_OPENSSL _FILE_OFFSET_BITS=64)
target_compile_options(cub_manager PRIVATE ${CMS_WARN_FLAGS})
target_include_directories(cub_manager PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR} ${CMS_SRC_DIR} ${CMS_CUBRID_INCLUDES} ${CMS_EXTERNAL_INCLUDES} ${CMS_OPENSSL_INCLUDES})
# OpenSSL is linked here rather than borrowed from libcubridcs, which used to export its
# bundled copy and no longer does. This also means the manager is compiled against the
# headers of the very library it links, which was not the case before.
# ${CMAKE_DL_LIBS} is for the static libcrypto, which calls dlopen/dlclose. In-tree it
# comes along with libcubridcs; a standalone build has to ask for it.
# OpenSSL before the CUBRID libraries, for the reason given at cm_admin; its symbols are not exported either,
# so the two copies cannot get mixed in one process.
target_link_libraries(cub_manager PRIVATE
  ${CMS_EXTERNAL_LIBS} ${CMS_OPENSSL_LIBS} ${CMS_CUBRID_LIBS}
  Threads::Threads ${CMAKE_DL_LIBS} rt util)
target_link_options(cub_manager PRIVATE "-Wl,--exclude-libs,libssl.a:libcrypto.a")

# The committed jsoncpp and libevent archives were built without -fPIC, so this has to
# be a non-PIE executable - which is what the automake build produced as well.
target_link_options(cub_manager PRIVATE -no-pie)

# cm_ssh: ssh and sftp for the HA node-adding script. It links no CUBRID library.
add_executable(cm_ssh ${CMS_SRC_DIR}/cm_ssh.cpp)
target_compile_definitions(cm_ssh PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(cm_ssh PRIVATE ${CMS_WARN_FLAGS})
target_include_directories(cm_ssh BEFORE PRIVATE ${CMS_LIBSSH2_INCLUDES} ${CMS_OPENSSL_INCLUDES})
# rt: clock_gettime lives in librt on glibc older than 2.17 (the official build image has 2.12)
target_link_libraries(cm_ssh PRIVATE ${CMS_LIBSSH2_LIBS} ${CMS_OPENSSL_LIBS} Threads::Threads ${CMAKE_DL_LIBS} rt)
target_link_options(cm_ssh PRIVATE "-Wl,--exclude-libs,libssl.a:libcrypto.a:libssh2.a")
add_dependencies(cm_ssh cms_libssh2)

if(CMS_OPENSSL_TARGET)
  add_dependencies(cm_admin ${CMS_OPENSSL_TARGET})
  add_dependencies(cub_manager ${CMS_OPENSSL_TARGET})
  add_dependencies(cm_ssh ${CMS_OPENSSL_TARGET})
endif()

# ---------------------------------------------------------------------------- install
if(CMS_STANDALONE)
  set(CMS_BINDIR bin)
  set(CMS_CONFDIR conf)
  set(CMS_SHAREDIR share)
  set(CMS_LICENSEDIR 3rdparty)
else()
  set(CMS_BINDIR ${CUBRID_BINDIR})
  set(CMS_CONFDIR ${CUBRID_CONFDIR})
  set(CMS_SHAREDIR ${CUBRID_DATADIR})
  set(CMS_LICENSEDIR ${CUBRID_3RDPARTYDIR})
endif()

install(TARGETS cm_admin cub_manager cm_ssh
  RUNTIME DESTINATION ${CMS_BINDIR} COMPONENT Manager)

# The HA node-adding script; the manager runs it from this fixed place only.
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ha_add_nodes.sh
  DESTINATION ${CMS_SHAREDIR}/cms COMPONENT Manager)

# libssh2 is BSD-3-Clause: its copyright notice, conditions and disclaimer go with the binaries, next to the
# license texts of the engine's 3rdparty libraries.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/license/libssh2_license.txt
  DESTINATION ${CMS_LICENSEDIR} COMPONENT Manager)

file(GLOB CMS_CONF_FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmserver/conf/*.conf
                         ${CMAKE_CURRENT_SOURCE_DIR}/cmserver/conf/*.pass)
install(FILES ${CMS_CONF_FILES}
  DESTINATION ${CMS_CONFDIR} COMPONENT Manager)

# The TLS key pair (conf/cm_ssl_cert.crt, conf/cm_ssl_cert.key) is not installed:
# cub_manager generates one when it first starts (ssl_cert_auto_generate in cm.conf),
# and an existing one is left as it is.
