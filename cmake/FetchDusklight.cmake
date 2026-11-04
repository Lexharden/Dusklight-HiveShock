# Provides DUSKLIGHT_DIR: a checkout of Dusklight pinned at DUSKLIGHT_VERSION.
#
# The mod SDK (sdk/) only needs headers and CMake helpers, but the game headers include
# extern/aurora/include, which is a git submodule and is NOT part of GitHub's source tarballs.
# So we do a shallow git fetch of the exact commit plus that single submodule.
#
# Overrides:
#   -DDUSKLIGHT_DIR=<path>      use an existing checkout (skips the download; handy for local work)
#   -DDUSKLIGHT_VERSION=<sha>   pin another commit/tag
#   -DDUSKLIGHT_REPO=<url>      use a fork

if (DUSKLIGHT_DIR)
    if (NOT EXISTS "${DUSKLIGHT_DIR}/sdk/CMakeLists.txt")
        message(FATAL_ERROR "DUSKLIGHT_DIR does not look like a Dusklight checkout: ${DUSKLIGHT_DIR}")
    endif ()
    message(STATUS "Dusklight: using ${DUSKLIGHT_DIR}")
    return()
endif ()

if (NOT DUSKLIGHT_VERSION)
    message(FATAL_ERROR "Set DUSKLIGHT_VERSION before including FetchDusklight.cmake")
endif ()
if (NOT DUSKLIGHT_REPO)
    set(DUSKLIGHT_REPO "https://github.com/TwilitRealm/dusklight.git")
endif ()

find_package(Git REQUIRED)

string(SUBSTRING "${DUSKLIGHT_VERSION}" 0 12 _short)
set(DUSKLIGHT_DIR "${CMAKE_BINARY_DIR}/_dusklight/${_short}" CACHE PATH "Dusklight checkout" FORCE)

if (NOT EXISTS "${DUSKLIGHT_DIR}/.fetched")
    message(STATUS "Dusklight: fetching ${DUSKLIGHT_VERSION} from ${DUSKLIGHT_REPO}")
    file(REMOVE_RECURSE "${DUSKLIGHT_DIR}")
    file(MAKE_DIRECTORY "${DUSKLIGHT_DIR}")

    function(_dusk_git)
        execute_process(
                COMMAND "${GIT_EXECUTABLE}" ${ARGN}
                WORKING_DIRECTORY "${DUSKLIGHT_DIR}"
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
        if (NOT _rc EQUAL 0)
            message(FATAL_ERROR "git ${ARGN} failed (${_rc}):\n${_out}\n${_err}")
        endif ()
    endfunction()

    _dusk_git(init -q)
    _dusk_git(remote add origin "${DUSKLIGHT_REPO}")
    _dusk_git(fetch -q --depth 1 origin "${DUSKLIGHT_VERSION}")
    _dusk_git(checkout -q FETCH_HEAD)
    _dusk_git(submodule update --init --depth 1 extern/aurora)
    file(WRITE "${DUSKLIGHT_DIR}/.fetched" "${DUSKLIGHT_VERSION}\n")
endif ()
