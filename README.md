# JAQR — Silent R Language (Step)

A statically typed, C-like language that compiles to C++20. Its main feature is the
`step` system: state changes are committed only if every invariant still holds.

## Requirements

- C++20 compiler with `std::format`
- [CMake](https://cmake.org/download/) 3.21+ and [Ninja](https://github.com/ninja-build/ninja/releases)
- For `--exec`: `clang++` or `g++` on `PATH` (MSVC not supported)
- For development: [Just](https://just.systems/man/en/installation.html), Python 3, `clang-tidy`

## Quick start

```sh
git clone https://github.com/QuinnHindmarsh/jaqr.git
cd jaqr
cmake --preset debug -DBUILD_TESTING=OFF
cmake --build --preset debug
```

Save as `hello.jaqr`:

```text
fn main() {
    print("Hello, JAQR!\n");
}
```

Run it (Windows: `build\debug\jaqr.exe`):

```sh
./build/debug/jaqr --exec hello.jaqr
```

## Usage

```text
jaqr [--exec] <source.jaqr> [-o <output.cpp>]
```

| Command | Result |
| --- | --- |
| `jaqr hello.jaqr` | Writes C++ to `a.cpp`, does not run |
| `jaqr hello.jaqr -o hello.cpp` | Writes C++ to `hello.cpp` |
| `jaqr --exec hello.jaqr` | Writes C++, compiles and runs it |

Compile the generated C++ yourself:

```sh
c++ -std=c++20 -O2 -I runtime hello.cpp -o hello
```

Set `JAQR_CXX` to choose the compiler used by `--exec`. Default search: `g++` then
`clang++` (`clang++` first on macOS).

## Just commands

| Command | What it does |
| --- | --- |
| `just build` | Debug build |
| `just release` | Release build |
| `just run hello.jaqr --exec` | Build, then compile and run a program |
| `just test` | Build with tests and run `ctest` |
| `just e2e` | Run end-to-end tests only |
| `just format` / `just lint` | Format / run `clang-tidy` |

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| No output | Add `--exec`; without it JAQR only writes `a.cpp` |
| Program seems stuck | It is waiting on `scan`; type input or press Ctrl+C |
| `No C++ compiler found` | Put `g++`/`clang++` on `PATH` or set `JAQR_CXX` |
| `jaqr/runtime/io.h` not found | Add `-I runtime` when compiling generated C++ |
| `LEX`/`SYN`/`SEM` error | Error in your source; see [semantic rules](docs/semantic_spec.md#diagnostic-catalogue) |

## Docs

See the [documentation index](docs/README.md). More examples are in `code_samples/`.
