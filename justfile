default: build

configure:
    cmake --preset debug

build: configure
    cmake --build --preset debug

[positional-arguments]
run source *args: build
    ./build/debug/jaqr "$@"

# configure with the tests enabled (the first time downloads GoogleTest), build, run every test
test:
    cmake --preset debug -DBUILD_TESTING=ON
    cmake --build --preset debug
    ctest --test-dir build/debug --output-on-failure

# only the end-to-end cases under tests/e2e/cases, against the debug build
[positional-arguments]
e2e *args: build
    python3 tests/e2e/run_e2e.py "$@"

release:
    cmake --preset release
    cmake --build --preset release

format-tool:
    #!/bin/sh
    set -eu
    if [ ! -x .format-venv/bin/python ]; then
        python3 -m venv .format-venv
    fi
    .format-venv/bin/python -m pip install --disable-pip-version-check 'clang-format==18.1.6'

# TODO: format and format-check cover only src/; extend them to tests/ and runtime/ once those
# have been reformatted in one dedicated commit (about 7000 test lines would change).
format: format-tool
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -exec .format-venv/bin/clang-format -i {} +

format-check: format-tool
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -exec .format-venv/bin/clang-format --dry-run --Werror {} +

lint *args: configure
    #!/bin/sh
    set -eu
    if command -v clang-tidy >/dev/null 2>&1; then
        tidy_executable="$(command -v clang-tidy)"
    elif command -v brew >/dev/null 2>&1; then
        tidy_executable="$(brew --prefix llvm)/bin/clang-tidy"
    else
        echo "clang-tidy was not found. Install LLVM and add its bin directory to PATH." >&2
        exit 1
    fi
    if [ ! -x "$tidy_executable" ]; then
        echo "clang-tidy was not found at $tidy_executable. Install it with: brew install llvm" >&2
        exit 1
    fi
    if command -v xcrun >/dev/null 2>&1; then
        SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
        export SDKROOT
    fi
    find src -type f -name '*.cpp' -exec "$tidy_executable" -p build/debug {{ args }} {} +

clean:
    cmake -E rm -rf build
