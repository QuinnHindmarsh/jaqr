# Tests

Run everything with `just test`, or only the end-to-end tests with `just e2e`.
The first test build downloads GoogleTest.

## Suites

| Suite | Covers |
|---|---|
| `tokeniser_tests.cpp` | Tokens, literals, `LEX` errors |
| `parser_tests.cpp` | Grammar rules, precedence, `SYN` errors |
| `tokeniser_parser_integration_tests.cpp` | Source through both stages |
| `optimizer.cpp` | Optimised vs unoptimised output behaves the same |
| `io.cpp` | `print` / `scan` with piped input and EOF |
| `names.cpp` | Generated C++ names |
| `e2e/` | Whole `.jaqr` programs through the real `jaqr` binary |

## End-to-end tests

```sh
just e2e                                   # all cases
just e2e -k step                           # names containing "step"
python tests/e2e/run_e2e.py --cxx clang++  # without Just, choose compiler
```

Add a case by dropping files into `tests/e2e/cases/`. No registration needed.

| Folder | Files | Passes when |
|---|---|---|
| `run/` | `name.jaqr`, `name.out`, optional `name.in` | Program exits 0 and prints exactly `name.out` |
| `reject/` | `name.jaqr` starting with `# expect: SEM017` | `jaqr` fails with that code |

Create `name.out` after checking the output by hand:

```sh
jaqr --exec name.jaqr -o scratch.cpp < name.in > name.out
```
