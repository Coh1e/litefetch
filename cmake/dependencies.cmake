# Dependencies from source, with CMake's own fetcher.
#
# The project used to rely on vcpkg: an extra tool nobody has to configure, a
# toolchain file baked into the preset, and a package cache outside the build
# tree. Everything here is pinned by URL and unpacked into the build directory,
# so a fresh checkout plus CMake is enough to build.

include(FetchContent)

# These upstreams still declare compatibility below 3.5, which CMake 4 refuses by
# default. Keep their own policy level rather than patching them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
# SQLite ships a source archive with no CMakeLists, so it is populated rather
# than added as a subdirectory.
cmake_policy(SET CMP0169 OLD)

set(LITEFETCH_FETCH_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
    "Where fetched sources are unpacked")

# --- SQLite: the amalgamation ------------------------------------------------
# SQLite publishes a plain source archive with no CMakeLists, which FetchContent's
# sub-build cannot use, so the same pinned URL is downloaded and unpacked here.
set(LITEFETCH_SQLITE_URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip)
set(LITEFETCH_SQLITE_DIR "${LITEFETCH_FETCH_DIR}/sqlite3-src")
set(LITEFETCH_SQLITE_ZIP "${LITEFETCH_FETCH_DIR}/sqlite-amalgamation-3540100.zip")
if(NOT EXISTS "${LITEFETCH_SQLITE_DIR}/sqlite3.c")
    file(MAKE_DIRECTORY "${LITEFETCH_FETCH_DIR}")
    # A failed attempt leaves an empty file behind, so the size decides whether
    # the archive is really here.
    set(sqliteNeedsDownload TRUE)
    if(EXISTS "${LITEFETCH_SQLITE_ZIP}")
        file(SIZE "${LITEFETCH_SQLITE_ZIP}" sqliteZipSize)
        if(sqliteZipSize GREATER 100000)
            set(sqliteNeedsDownload FALSE)
        else()
            file(REMOVE "${LITEFETCH_SQLITE_ZIP}")
        endif()
    endif()
    if(sqliteNeedsDownload)
        file(DOWNLOAD "${LITEFETCH_SQLITE_URL}" "${LITEFETCH_SQLITE_ZIP}"
            STATUS sqliteDownload TLS_VERIFY ON)
        list(GET sqliteDownload 0 sqliteDownloadCode)
        if(NOT sqliteDownloadCode EQUAL 0)
            message(FATAL_ERROR "cannot download ${LITEFETCH_SQLITE_URL}: ${sqliteDownload}")
        endif()
    endif()
    file(REMOVE_RECURSE "${LITEFETCH_FETCH_DIR}/sqlite3-extract")
    file(ARCHIVE_EXTRACT INPUT "${LITEFETCH_SQLITE_ZIP}" DESTINATION "${LITEFETCH_FETCH_DIR}/sqlite3-extract")
    file(GLOB sqliteCandidate LIST_DIRECTORIES true "${LITEFETCH_FETCH_DIR}/sqlite3-extract/*")
    list(GET sqliteCandidate 0 sqliteInner)
    file(COPY "${sqliteInner}/" DESTINATION "${LITEFETCH_SQLITE_DIR}")
endif()
add_library(litefetch-sqlite3 STATIC ${LITEFETCH_SQLITE_DIR}/sqlite3.c)
target_include_directories(litefetch-sqlite3 PUBLIC ${LITEFETCH_SQLITE_DIR})
target_compile_definitions(litefetch-sqlite3 PUBLIC
    SQLITE_THREADSAFE=1 SQLITE_ENABLE_COLUMN_METADATA=1 SQLITE_OMIT_LOAD_EXTENSION=1)
if(MSVC)
    # The amalgamation is C; silence the noisy C4996 family it triggers.
    target_compile_options(litefetch-sqlite3 PRIVATE /wd4996 /wd4267 /wd4244 /wd4100)
endif()
add_library(unofficial::sqlite3::sqlite3 ALIAS litefetch-sqlite3)

# --- libcurl: Windows TLS, no third party crypto ----------------------------
# Static, so the package is a single executable and no DLL has to be shipped.
# curl honours BUILD_STATIC_LIBS/BUILD_SHARED_LIBS instead of one switch.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_STATIC_CURL ON CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(HTTP_ONLY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(curl
    URL https://curl.se/download/curl-8.10.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${LITEFETCH_FETCH_DIR}/curl")
# curl builds its own test suite unless testing is off while it configures; the
# setting is restored afterwards so the downloader keeps its own tests.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(curl)
set(BUILD_TESTING ON CACHE BOOL "" FORCE)
enable_testing()
if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl ALIAS libcurl)
endif()

# --- JSON: header-only, used by the Win32 native bridge ----------------------
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${LITEFETCH_FETCH_DIR}/json")
FetchContent_MakeAvailable(nlohmann_json)

# --- Boost headers and libtorrent -------------------------------------------
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
FetchContent_Declare(boost
    URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${LITEFETCH_FETCH_DIR}/boost")
FetchContent_GetProperties(boost)
if(NOT boost_POPULATED)
    FetchContent_Populate(boost)
endif()
set(BOOST_ROOT "${boost_SOURCE_DIR}" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR "${boost_SOURCE_DIR}" CACHE PATH "" FORCE)

# WebRTC support drags in libdatachannel, which ships as a git submodule and is
# therefore absent from the source archive; the downloader does not use it.
set(webtorrent OFF CACHE BOOL "" FORCE)
set(build_tests OFF CACHE BOOL "" FORCE)
set(build_examples OFF CACHE BOOL "" FORCE)
set(python-bindings OFF CACHE BOOL "" FORCE)
set(python-bindings-tests OFF CACHE BOOL "" FORCE)
# The release tarball carries no submodules and libtorrent needs deps/try_signal,
# so this one is cloned and only that submodule is fetched.
FetchContent_Declare(libtorrent
    GIT_REPOSITORY https://github.com/arvidn/libtorrent.git
    GIT_TAG v2.1.2
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "deps/try_signal"
    PREFIX "${LITEFETCH_FETCH_DIR}/libtorrent")
FetchContent_MakeAvailable(libtorrent)
if(NOT TARGET LibtorrentRasterbar::torrent-rasterbar)
    add_library(LibtorrentRasterbar::torrent-rasterbar ALIAS torrent-rasterbar)
endif()
