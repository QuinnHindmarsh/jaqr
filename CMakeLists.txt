cmake_minimum_required(VERSION 3.21)

# On Windows, CMake can select an MSVC installation even outside a Visual Studio
# developer shell, where it cannot link. If no compiler was requested and cl is
# not on PATH, prefer a g++/clang++ that is.
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(JAQR_MSVC_CL NAMES cl NO_CACHE)
    if(NOT JAQR_MSVC_CL)
        find_program(JAQR_DEFAULT_CXX NAMES g++ clang++ NO_CACHE)
        if(JAQR_DEFAULT_CXX)
            set(CMAKE_CXX_COMPILER "${JAQR_DEFAULT_CXX}" CACHE FILEPATH "C++ compiler")
        endif()
    endif()
endif()

# A build directory first configured with MSVC keeps cl in its cache, and the check above
# cannot change that. Stop with a clear message instead of failing inside project().
if(CMAKE_HOST_WIN32 AND CMAKE_CXX_COMPILER MATCHES "[/\\\\]cl\\.exe$")
    find_program(JAQR_MSVC_CL NAMES cl NO_CACHE)
    if(NOT JAQR_MSVC_CL)
        message(FATAL_ERROR
            "This build directory is configured with MSVC (${CMAKE_CXX_COMPILER}), but cl is "
            "not on PATH, so it cannot link. Delete the build directory (just clean) and "
            "configure again, or run from a Visual Studio developer shell.")
    endif()
endif()

project(jaqr LANGUAGES CXX)

option(JAQR_WARNINGS "Build the jaqr compiler with -Wall -Wextra (/W4 with MSVC)" ON)

file(GLOB_RECURSE JAQR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_executable(jaqr ${JAQR_SOURCES})
target_compile_definitions(jaqr PRIVATE JAQR_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime")
set_target_properties(jaqr PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
if(JAQR_WARNINGS)
    if(MSVC)
        target_compile_options(jaqr PRIVATE /W4)
    else()
        target_compile_options(jaqr PRIVATE -Wall -Wextra)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(JAQR_CODEGEN_SOURCES
        src/optimizer/optimizer.cpp
        src/semantic/builtins.cpp
        src/code_generator/code_generator.cpp
        src/ast/declaration.cpp
        src/ast/expression.cpp
        src/ast/statement.cpp
        src/ast/type.cpp
    )

    # A fixture that prints generated C++, built from tests/<source> with the code generator,
    # and a test that compiles and runs that C++ through tests/<script>. Arguments after the
    # script are passed to it after the fixture and compiler.
    function(jaqr_add_codegen_test target source test_name script)
        add_executable(${target} tests/${source} ${JAQR_CODEGEN_SOURCES})
        target_include_directories(${target} PRIVATE src)
        target_compile_features(${target} PRIVATE cxx_std_20)
        add_test(NAME ${test_name}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}"
                $<TARGET_FILE:${target}> "${CMAKE_CXX_COMPILER}" ${ARGN})
    endfunction()

    jaqr_add_codegen_test(name_tests names.cpp generated_names optimizer_test.py
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    jaqr_add_codegen_test(io_tests io.cpp io_semantics io_test.py
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    jaqr_add_codegen_test(optimizer_tests optimizer.cpp optimizer_semantics optimizer_test.py)

    # Every case under tests/e2e/cases through the real jaqr executable, which compiles the
    # programs with the same C++ compiler CMake uses.
    add_test(NAME e2e
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_e2e.py"
            --jaqr $<TARGET_FILE:jaqr> --cxx "${CMAKE_CXX_COMPILER}")
endif()
