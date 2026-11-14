# JAQR — Silent R Language (Step)

JAQR is a statically typed, C-like language that compiles to C++20. It supports
functions, arrays, structs and a `step` system for changing state while checking
invariants: conditions that the state must satisfy.

**By default, JAQR writes generated C++ to a file. Add `--exec` to compile and run
your program.**

## Before you start

To build and use JAQR, install:

- A **C++20 compiler and standard library**, including support for `std::format`.
- **[CMake](https://cmake.org/download/)** 3.21 or newer.
- **[Ninja](https://github.com/ninja-build/ninja/releases)**, used by the build presets.

For automatic execution with `--exec`, put `clang++` or `g++` on your `PATH`.
The runner uses Clang/GCC-style compiler options, so MSVC is not supported for
this mode. On Windows the compiler must be a `.exe` or `.com` file; its name may
omit the extension.

These additional tools are only needed for development:

| Tool | Used for |
| --- | --- |
| [Just](https://just.systems/man/en/installation.html) | Shortcuts for building, testing, formatting and linting |
| Python 3 | The test suite, including `just e2e` |
| Python's `venv` and `pip` | Installing the pinned formatter |
| `clang-tidy` | C++ lint checks |

## Quick start

### 1. Get the source and build

```sh
git clone https://github.com/QuinnHindmarsh/jaqr.git
cd jaqr
cmake --preset debug -DBUILD_TESTING=OFF
cmake --build --preset debug
```

If you already have the repository, start from its root directory and run the two
CMake commands. This builds the compiler at `build/debug/jaqr`.

`BUILD_TESTING=OFF` keeps this first build small and avoids downloading test
dependencies. CMake's `BUILD_TESTING` option defaults to `ON`, so a configure that
leaves it out (including `just build` in a fresh `build/debug`) downloads
GoogleTest. You can enable tests later using the commands below.

### 2. Write your first program

Save this as `hello.jaqr` in the repository root:

```text
fn main() {
    print("Hello, JAQR!\n");
}
```

### 3. Run it

```sh
./build/debug/jaqr --exec hello.jaqr
```

Expected output:

```text
Hello, JAQR!
```

JAQR also saves the generated C++ as `a.cpp`. The executable used by `--exec` is
temporary; use the manual compilation instructions below if you want to keep one.

On Windows, replace `./build/debug/jaqr` with `build\debug\jaqr.exe`.

## Generate, run or keep an executable

Run these commands from the repository root:

| What you want to do | Command |
| --- | --- |
| Generate C++ without running the program | `./build/debug/jaqr hello.jaqr` |
| Choose the generated C++ filename | `./build/debug/jaqr hello.jaqr -o hello.cpp` |
| Generate, compile and run | `./build/debug/jaqr --exec hello.jaqr` |
| Run and choose the C++ filename | `./build/debug/jaqr --exec hello.jaqr -o hello.cpp` |

`-o` names the **C++ source file**, not an executable. Output paths are relative to
your current directory, and an existing output file is overwritten. Quote paths
that contain spaces, for example `"my program.jaqr"`.

To create an executable that you can run again:

```sh
./build/debug/jaqr hello.jaqr -o hello.cpp
c++ -std=c++20 -O2 -I runtime hello.cpp -o hello
./hello
```

The `-I runtime` option lets C++ find JAQR's runtime headers, including the
implementations of `print` and `scan`. Replace `c++` with your compiler's command
if needed. On Windows, name the executable `hello.exe` and run `.\hello.exe`.

To select the compiler used by `--exec` on macOS/Linux:

```sh
export JAQR_CXX=clang++
./build/debug/jaqr --exec hello.jaqr
```

`JAQR_CXX` accepts an executable name on `PATH` or a path to a compiler, without
extra compiler flags. This selects the compiler for **generated programs**; it
does not change the compiler CMake uses to build JAQR itself. If `JAQR_CXX` does
not name an executable, JAQR stops with an error instead of searching `PATH`.
Without `JAQR_CXX`, JAQR looks for `g++` then `clang++` on Linux and Windows, and
`clang++` then `g++` on macOS.

On Windows, `JAQR_CXX` may leave out `.exe` (`JAQR_CXX=g++` works), and paths with
spaces work. Only `.exe` and `.com` files are accepted, not `.bat` or `.cmd`.

`jaqr --exec` compiles the generated C++ with `-std=c++20` and no optimisation
flags, runs the program with your terminal's input and output, and returns its exit
code. If the C++ compiler fails, `jaqr` returns the compiler's exit code.

Errors in your JAQR source stop `jaqr` before any program runs. Command-line and file
errors print `Error: <reason>`, followed by the usage line for command-line
mistakes:

```text
Error: no source file given
Usage: jaqr [--exec] <source.jaqr> [-o <output.cpp>]
```

## Read input and print results

`print(value)` writes a value without adding spaces or a newline. `scan(variable)`
reads into a writable variable and returns whether the input succeeded.

```text
fn main() {
    let number: int = 0;
    print("Enter an integer: ");
    if (scan(number)) {
        print("Twice that is ");
        print(number * 2);
        print('\n');
    } else {
        print("That was not a valid integer.\n");
    }
}
```

When running this program, type an integer and press Enter. A program waiting at
`scan` has not necessarily stalled—it may simply need your input.

## Build and development commands

Just is optional. Its recipes run from the repository root:

| Command | What it does |
| --- | --- |
| `just` or `just build` | Configure and build the debug compiler |
| `just run hello.jaqr` | Build JAQR and generate `a.cpp`; does **not** execute the program |
| `just run hello.jaqr --exec` | Build JAQR, then compile and run the program |
| `just run hello.jaqr -o hello.cpp` | Build JAQR and choose the generated C++ filename |
| `just release` | Configure and build the release compiler |
| `just test` | Configure with `BUILD_TESTING=ON`, build, and run every test with `ctest` |
| `just e2e` | Build, then run only the end-to-end cases in `tests/e2e/cases`; extra arguments go to the runner, for example `just e2e -k step` |
| `just format` | Format C++ files under `src/` |
| `just format-check` | Check formatting without editing files |
| `just lint` | Run `clang-tidy` |
| `just clean` | Remove the `build` directory |

The formatting recipes install the pinned `clang-format` version into
`.format-venv`; this requires Python's `venv`/`pip` and network access.

Without Just, make a release build with:

```sh
cmake --preset release -DBUILD_TESTING=OFF
cmake --build --preset release
./build/release/jaqr --exec hello.jaqr
```

A release build optimises the JAQR compiler itself. The `-O2` option in the manual
compilation example optimises your generated executable.

### Run the tests

Install Python 3, then run:

```sh
just test
```

or, without Just:

```sh
cmake --preset debug -DBUILD_TESTING=ON
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
```

The first test-enabled configuration downloads GoogleTest, so it needs network
access. The suite has unit tests for the tokeniser and parser, generated-code tests
(names, I/O and optimiser behaviour) and the end-to-end tests, which run whole
`.jaqr` programs through the real `jaqr` executable. Under `ctest` the end-to-end
tests use the C++ compiler CMake built `jaqr` with.

CMake remembers `BUILD_TESTING` in each build directory. Plain `just build` keeps
that setting; explicitly set it to `ON` or `OFF` when you want to change it.

To run only the end-to-end tests against the debug build:

```sh
just e2e                      # every case
just e2e -k step              # cases whose name contains "step"
just e2e --cxx clang++        # C++ compiler for jaqr --exec (sets JAQR_CXX)
```

#### Add an end-to-end test

Cases live in `tests/e2e/cases/` and need no registration.

- A program that should run: add `tests/e2e/cases/run/<name>.jaqr`, and
  `<name>.in` if it reads input. Create `<name>.out` from the program's output
  after you have checked by hand that the output is right:
  `jaqr --exec <name>.jaqr -o scratch.cpp < <name>.in > <name>.out`. The test
  passes when the program exits with 0 and prints exactly `<name>.out`.
- A program the compiler should reject: add `tests/e2e/cases/reject/<name>.jaqr`
  whose first line is `# expect: CODE`, for example `# expect: SEM017`. The test
  passes when `jaqr` exits non-zero and reports that code without running the
  program.

See [Code structure](docs/code_structure.md#41-end-to-end-tests) for details.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| No output appears after `jaqr file.jaqr` | This generates `a.cpp` only. Add `--exec` to run the program. |
| The program seems to wait forever | If you used `--exec`, check for `scan` calls. Enter the required input, or press Ctrl+C to stop. |
| `No C++ compiler found on PATH` | Install a compatible `clang++`/`g++`, add it to `PATH`, or set `JAQR_CXX`. |
| `JAQR_CXX='...' does not name an executable C++ compiler` | Fix the value: a name on `PATH` or a path to a compiler, not a script, and with no flags. On Windows it must be a `.exe` or `.com` file. |
| `Error: cannot open source file '...'` | Check the path and the current directory. |
| `Error: no source file given` or `unknown option` | `jaqr` takes one source file, `--exec` and `-o <file>`; the usage line is printed after the error. |
| A `LEX`, `SYN` or `SEM` message | Your JAQR source has an error. `SEM` messages start with `file:line:col-line:col:`; `LEX` and `SYN` messages end with `at line N, column M`. Positions start counting at 0. See [Semantic rules](docs/semantic_spec.md#diagnostic-catalogue). |
| The C++ compiler reports errors in the generated file | `jaqr` should report source errors itself. Keep the file with `-o`; if the JAQR program is valid, this is a compiler bug. |
| `jaqr/runtime/io.h` cannot be found | When compiling generated C++ yourself, include `-I runtime` from the repository root, or use an absolute runtime directory path. |
| CMake cannot find Ninja | Install Ninja and ensure its executable is on `PATH`. |
| Test configuration fails while downloading GoogleTest | Check network access, or configure with `-DBUILD_TESTING=OFF` to build only JAQR. |
| Changes to the compiler do not appear | Rebuild using `just build` or `cmake --build --preset debug`. |

## Learn more

- [Language features](docs/features.md) — types, functions, arrays and transactional state.
- [Example programs](docs/sample_code.md) — examples to save and try with `--exec`.
- [Grammar](docs/grammar.md) — the language's syntax.
- [Semantic rules](docs/semantic_spec.md) — type rules, restrictions and diagnostics.
- [Code structure](docs/code_structure.md) — how the compiler is organised.
- [Optimisation](docs/optimisation.md) — supported optimisations and their limits.
- [Standard I/O](docs/io.md) — how `print` and `scan` behave.
- [Generated names](docs/generated-names.md) — how JAQR names appear in the generated C++.
- [Documentation index](docs/README.md) — all of the above in one list.

More example programs are in `code_samples/`.

For contributors: CMake discovers `.cpp` files recursively under `src/`. Adding
or removing a compiler source file does not require editing `CMakeLists.txt`;
the next build updates the source list. The compiler builds with `-Wall -Wextra`
(`/W4` with MSVC); configure with `-DJAQR_WARNINGS=OFF` to turn that off. The
test targets in `tests/CMakeLists.txt` and `CMakeLists.txt` list their sources by
hand. Pull requests run the formatting check, `clang-tidy`, and a build with tests
plus `ctest` (see `.github/workflows/checks.yml`).
