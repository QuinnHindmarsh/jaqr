include(FetchContent)

# CMake builds whose curl uses OpenSSL (such as those shipped with MinGW toolchains, which
# carry libcrypto next to cmake.exe) have no CA certificates on Windows, so downloading
# GoogleTest fails certificate verification. Unless a bundle was given, use Git for Windows'.
if(CMAKE_HOST_WIN32 AND NOT CMAKE_TLS_CAINFO AND NOT DEFINED ENV{CMAKE_TLS_CAINFO})
    get_filename_component(_jaqr_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
    file(GLOB _jaqr_openssl "${_jaqr_cmake_bin}/libcrypto-*.dll")
    find_package(Git QUIET)
    if(_jaqr_openssl AND GIT_EXECUTABLE)
        get_filename_component(_jaqr_git_root "${GIT_EXECUTABLE}" DIRECTORY)
        get_filename_component(_jaqr_git_root "${_jaqr_git_root}" DIRECTORY)
        # git.exe is in <Git>/cmd or <Git>/mingw64/bin, so the root is <Git> or <Git>/mingw64
        find_file(JAQR_CA_BUNDLE ca-bundle.crt
            PATHS "${_jaqr_git_root}/mingw64/etc/ssl/certs" "${_jaqr_git_root}/etc/ssl/certs"
            NO_DEFAULT_PATH)
        if(JAQR_CA_BUNDLE)
            set(CMAKE_TLS_CAINFO "${JAQR_CA_BUNDLE}")
        endif()
    endif()
endif()

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# keep gtest on the same runtime as the code under test when building with MSVC
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# the compiler front end (everything the tests exercise), built once and shared by every test
add_library(jaqr_front_end STATIC
    ${PROJECT_SOURCE_DIR}/src/tokeniser/tokeniser.cpp
    ${PROJECT_SOURCE_DIR}/src/tokeniser/dfa.cpp
    ${PROJECT_SOURCE_DIR}/src/parser/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/ast/ast_builder.cpp
    ${PROJECT_SOURCE_DIR}/src/ast/declaration.cpp
    ${PROJECT_SOURCE_DIR}/src/ast/expression.cpp
    ${PROJECT_SOURCE_DIR}/src/ast/statement.cpp
    ${PROJECT_SOURCE_DIR}/src/ast/type.cpp
)
target_compile_features(jaqr_front_end PUBLIC cxx_std_20)
set_target_properties(jaqr_front_end PROPERTIES CXX_EXTENSIONS OFF)

# adds a gtest executable named after its source file: jaqr_add_test(foo) builds foo.cpp
function(jaqr_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(${name} PRIVATE jaqr_front_end GTest::gtest_main GTest::gmock)
    # with MinGW, link the runtime in so the tests never pick up a different libstdc++ DLL from
    # PATH (e.g. the one Git Bash ships), which crashes on the first thrown exception
    if(MINGW)
        target_link_options(${name} PRIVATE -static)
    endif()
    # tests are found when ctest runs, so a failing test never breaks the build
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

jaqr_add_test(tokeniser_tests)
jaqr_add_test(parser_tests ast_dump.cpp)
jaqr_add_test(tokeniser_parser_integration_tests ast_dump.cpp)
