# Fetch the pinned llama.cpp source tree with Git alone: no Node, no npm.
#
# The pin comes from packaging/vendor/llama.cpp.conf, the same file
# scripts/install-vendor.js, scripts/vendor.sh and make-vendor-package.sh
# read, so the commit is written down once. The fetch mirrors
# install-vendor.js: a sparse, depth-1 fetch of exactly VENDOR_COMMIT, a
# check that HEAD is that commit, the .git directory removed, and a
# .vendor-source-commit stamp that CMakeLists.txt verifies before building.
#
# Used two ways:
#   include()d by CMakeLists.txt, which calls onyx_fetch_llama(<dest>)
#   cmake -DONYX_FETCH_DEST=<dest> -P cmake/fetch-llama.cmake   (script mode)

set(_ONYX_FETCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

function(onyx_read_llama_spec)
    file(READ "${_ONYX_FETCH_ROOT}/packaging/vendor/llama.cpp.conf" _spec)
    foreach(key VENDOR_UPSTREAM VENDOR_COMMIT VENDOR_SPARSE VENDOR_REQUIRED)
        if (NOT _spec MATCHES "(^|\n)${key}=\"([^\"]*)\"")
            message(FATAL_ERROR "packaging/vendor/llama.cpp.conf: ${key} is missing")
        endif()
        set(ONYX_SPEC_${key} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endforeach()
endfunction()

function(onyx_fetch_llama dest)
    onyx_read_llama_spec()
    if (NOT ONYX_SPEC_VENDOR_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "VENDOR_COMMIT must be hex")
    endif()
    string(LENGTH "${ONYX_SPEC_VENDOR_COMMIT}" _len)
    if (NOT _len EQUAL 40)
        message(FATAL_ERROR "VENDOR_COMMIT must be 40 hex characters")
    endif()
    separate_arguments(_sparse UNIX_COMMAND "${ONYX_SPEC_VENDOR_SPARSE}")
    separate_arguments(_required UNIX_COMMAND "${ONYX_SPEC_VENDOR_REQUIRED}")

    set(_stamp "${dest}/.vendor-source-commit")
    if (EXISTS "${_stamp}")
        file(READ "${_stamp}" _have)
        string(STRIP "${_have}" _have)
        set(_complete TRUE)
        foreach(p IN LISTS _required)
            if (NOT EXISTS "${dest}/${p}")
                set(_complete FALSE)
            endif()
        endforeach()
        if (_have STREQUAL ONYX_SPEC_VENDOR_COMMIT AND _complete)
            message(STATUS "onyx-engine: llama.cpp ${ONYX_SPEC_VENDOR_COMMIT} already fetched into ${dest}")
            return()
        endif()
    endif()

    find_package(Git QUIET)
    if (NOT GIT_FOUND)
        message(FATAL_ERROR "Git is required to fetch llama.cpp. Install Git, or point "
                            "-DONYX_ENGINE_LLAMA_DIR at a llama.cpp checkout of ${ONYX_SPEC_VENDOR_COMMIT}.")
    endif()

    message(STATUS "onyx-engine: fetching llama.cpp ${ONYX_SPEC_VENDOR_COMMIT} into ${dest}")
    file(REMOVE_RECURSE "${dest}")
    file(MAKE_DIRECTORY "${dest}")

    macro(_onyx_git)
        execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
                        WORKING_DIRECTORY "${dest}" RESULT_VARIABLE _rc)
        if (NOT _rc EQUAL 0)
            file(REMOVE_RECURSE "${dest}")
            string(JOIN " " _cmd ${ARGN})
            message(FATAL_ERROR "git ${_cmd} failed (${_rc}); the llama.cpp fetch was abandoned")
        endif()
    endmacro()

    _onyx_git(init -q)
    _onyx_git(remote add origin "${ONYX_SPEC_VENDOR_UPSTREAM}")
    if (_sparse)
        _onyx_git(sparse-checkout init --cone)
        _onyx_git(sparse-checkout set ${_sparse})
    endif()
    _onyx_git(fetch --depth 1 origin "${ONYX_SPEC_VENDOR_COMMIT}")
    _onyx_git(checkout -q FETCH_HEAD)

    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
                    WORKING_DIRECTORY "${dest}" OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT _head STREQUAL ONYX_SPEC_VENDOR_COMMIT)
        file(REMOVE_RECURSE "${dest}")
        message(FATAL_ERROR "fetched llama.cpp is at ${_head}, expected ${ONYX_SPEC_VENDOR_COMMIT}")
    endif()
    file(REMOVE_RECURSE "${dest}/.git")
    foreach(p IN LISTS _required)
        if (NOT EXISTS "${dest}/${p}")
            file(REMOVE_RECURSE "${dest}")
            message(FATAL_ERROR "fetched llama.cpp is missing ${p}")
        endif()
    endforeach()
    file(WRITE "${_stamp}" "${ONYX_SPEC_VENDOR_COMMIT}\n")
endfunction()

if (CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if (NOT ONYX_FETCH_DEST)
        set(ONYX_FETCH_DEST "${_ONYX_FETCH_ROOT}/.deps/llama.cpp")
    endif()
    get_filename_component(ONYX_FETCH_DEST "${ONYX_FETCH_DEST}" ABSOLUTE)
    onyx_fetch_llama("${ONYX_FETCH_DEST}")
endif()
