# Code Structure

The JAQR compiler is a classic multi-pass compiler written in C++20. Each pass
lives in its own folder under `src/` and hands a single data structure to the next.

## 1. Pipeline

```text
 source.jaqr
     │  main.cpp reads the file into one vector<char> per line (each keeps its '\n')
     ▼
┌──────────────┐  vector<token>     src/tokeniser/   DFA, maximal munch
│  Tokeniser   │──────────────────▶                  errors: LEX001, LEX002
└──────────────┘
     ▼
┌──────────────┐  program_ptr (AST) src/parser/      recursive descent, 1-token lookahead
│    Parser    │──────────────────▶ src/ast/         errors: SYN001, SYN002
└──────────────┘
     ▼
┌──────────────┐  analysis_root     src/semantic/    built-in resolver, then scopes, types,
│   Semantic   │──────────────────▶ src/symbol_table/ Step rules; errors: SEM001–SEM025
│   analysis   │  (checked AST + impl and invariant indexes)
└──────────────┘
     ▼                                               ── code_generator::gen_code ──
┌──────────────┐  builtin_calls     src/semantic/builtins.*   print / scan binding
│   Built-in   │                                              (resolved again here)
│   resolver   │
└──────────────┘
     ▼
┌──────────────┐  optimization_plan src/optimizer/   folding, dead code, unrolling
│  Optimiser   │
└──────────────┘
     ▼
┌──────────────┐  vector<string>    src/code_generator/  emits C++20
│ Code         │──────────────────▶ generated.cpp + runtime/jaqr/runtime/io.h
│ generator    │
└──────────────┘
     ▼
┌──────────────┐                    src/run.cpp       writes the C++ file; with --exec
│   Driver     │                                      compiles and runs it
└──────────────┘
```

`src/main.cpp` runs **tokenise → parse → analyse** on the file given on the command
line, then passes the result to
`code_generator{impls, invs}.gen_code(*root)`. `gen_code` runs the built-in resolver,
the optimiser and the emitter in that order. The resolver also runs as the first step
of `analyse`, so print/scan mistakes are reported with the other semantic errors;
`gen_code` runs it again to get the call map it needs.

## 2. Folder map

```text
jaqr/
├── CMakeLists.txt          builds every src/**/*.cpp into `jaqr`; defines CTest tests
├── CMakePresets.json       debug / release presets (Ninja, build/<preset>/)
├── justfile                build, run, test, e2e, format, lint, clean shortcuts
├── .clang-format / .clang-tidy / .clangd
├── .github/workflows/checks.yml   CI on pull requests: format-check + clang-tidy, and a build with tests + ctest
├── runtime/jaqr/runtime/io.h      runtime header included by generated C++
├── docs/                   this documentation
├── tests/                  unit, integration, generated-code and end-to-end (`tests/e2e/`) tests
└── src/
    ├── main.cpp            command-line driver
    ├── run.h / run.cpp     writes the generated C++; with --exec, finds a compiler, builds and runs it
    ├── tokens/             token type enum + token struct
    ├── tokeniser/          DFA graph and tokenise()
    ├── parser/             Parser class and parse()
    ├── ast/                AST node types, builders, grammar rules
    ├── visitor/            abstract visitor interface over the AST
    ├── symbol_table/       generic scoped symbol table
    ├── semantic/           semantic analyser + built-in resolver
    ├── optimizer/          optimisation plan builder
    └── code_generator/     code_generator class and the internal C++ emitter
```

## 3. Modules

### 3.1 `main.cpp` — driver

- Parses the command line: `jaqr [--exec] <source.jaqr> [-o <output.cpp>]`. The output
  defaults to `a.cpp`. A bad command line throws a `usage_error`, which is printed as
  `Error: <reason>` followed by the usage line.
- Refuses to write the generated C++ over the source file.
- Splits the file into lines, keeping `'\n'` so tokens such as strings can span lines.
  A file that cannot be opened gives `Error: cannot open source file '<file>'`.
- Calls `analyse(parse(tokenise(lines)), filename)`, then
  `code_generator{impls, invs}.gen_code(*root)` and `run_generated(...)`.
- Catches `semantic_error` (prints it as-is) and any other `std::exception`
  (prints `Error: <message>`), returning exit code `1`. Lexer and parser errors are
  plain `std::runtime_error`s, so they print as `Error: LEX001: ...` with no file name.

`run.cpp` writes the generated lines to the output file. With `--exec` it then finds
a C++ compiler (`JAQR_CXX`, else `g++` and `clang++` on `PATH`, in the order that
suits the platform), compiles with `-std=c++20 -I <runtime dir>` into a temporary
directory, runs the program and returns its exit code. It starts processes without a
shell. On Windows it quotes each argument itself, so paths with spaces work, and it
accepts only `.exe` and `.com` files as compilers (a name without an extension gets
`.exe` or `.com` appended). The runtime directory is the source tree's `runtime/`,
baked in at build time as `JAQR_RUNTIME_DIR`. The command line has no option for the
optimiser's settings; `gen_code` is always called with the defaults.

### 3.2 `tokens/` and `tokeniser/` — lexical analysis

| File | Role |
|---|---|
| `tokens/token.h` | `enum class token_type` (keywords, operators, literals, punctuation, `WHITESPACE`, `COMMENT`) and `struct token { type, lexeme, line, column }` |
| `tokeniser/graph_node.h`, `edge.h` | DFA node and edge. An edge is a character predicate plus a target node. A node has an optional accepting `token_type` and, built once from its edges when the node is constructed, a 256-entry transition table indexed by character. The first edge that matches a character decides its table entry, so specific edges come before general ones. `next(ch)` is one table lookup and returns null when no edge matches |
| `tokeniser/dfa.h`, `dfa.cpp` | The whole DFA as `graph_node` objects, built at startup, exposed as `dfa_start` and `dfa_reject`: identifiers, keyword prefix chains, numbers (decimal, `2b`, `16b`, float), char/string literals with escapes, comments, operators, punctuation |
| `tokeniser/tokeniser.cpp` | `tokenise()`: from each position, follow `next(ch)` until it returns null; emit the token if the state is accepting. An edge to `dfa_reject`, or stopping in a non-accepting state, throws `LEX001` (unexpected character); running out of input inside a token throws `LEX002` |

Keywords are recognised inside the DFA itself: there is one node per keyword
prefix, and any other identifier character falls back to the identifier node.

### 3.3 `parser/` — syntax analysis

`parser.cpp` contains a `Parser` class with one method per grammar rule
(`parse_program`, `parse_statement`, `parse_logical_or_expression`, …), mirroring the
EBNF one-to-one. It:

- Skips `WHITESPACE` and `COMMENT` tokens (`skip_trivia`).
- Uses one token of lookahead, plus the lexeme check that tells `fn main()` from `fn f()`.
- Splits `<-` into `<` `-` when it appears directly after an operand (`x<-1`).
- Records a `source_span` on every node for diagnostics.
- Throws `SYN001: unexpected token ...` (or `unexpected end of input`) on the first
  error, and `SYN002` for a number literal that does not fit its type.

The grammar it implements is written out in `src/ast/grammar_rules/*.txt` and
in [`grammar.md`](grammar.md).

### 3.4 `ast/` — abstract syntax tree

Nodes are owned through `std::unique_ptr` and every node has an `accept(visitor&)`.
The root is a `program`, held as a `program_ptr`.

| File | Nodes |
|---|---|
| `declaration.h` | `program`, `variable_decl` (`ORDINARY` / `STATE`, `immutable`), `function_decl` (`ORDINARY` / `UPDATE`, state + normal params), `struct_decl`, `impl_decl`, `invariant_decl` |
| `statement_base.h` | `stmt` base class and `block_stmt` |
| `statement.h` | `expr_stmt`, `decl_stmt`, `return_stmt`, `if_stmt` (with `elif` branches), `while_stmt`, `for_stmt`, `step_stmt` (body + optional reject block) |
| `expression_base.h` | `expr` base class |
| `expression.h` | `binary_expr`, `unary_expr`, `postfix_expr` (`SUBSCRIPT`, `MEMBER_ACCESS`, `FUNCT_CALL`, `UPDATE_CALL`), `literal`, `identifier`, `assignment_expr`, `array_literal`, `struct_literal`, `dynamic_alloc`, `conversion_expr`, `this_expr` |
| `type.h` | `primitive_type_syntax`, `named_type_syntax`, `pointer_type_syntax`, `array_type_syntax` |
| `source.h` | `source_span` (start/end line and column, zero-indexed, end exclusive) |
| `ast_builder.h/.cpp` | `make_*` factory functions the parser uses to build nodes, including `make_program` |
| `grammar_rules/` | EBNF for programs, statements and expressions |

### 3.5 `visitor/` — traversal

`visitor.h` declares a pure virtual `visit` overload for every AST node type.
The semantic checker (in `semantic.cpp`), the optimiser (in `optimizer.cpp`) and the
code generator's `emitter` (in `code_generator.cpp`) are all visitors, so adding a
node type forces every pass to handle it at compile time. `code_generator` and
`resolve_builtins` are not visitors themselves; the built-in resolver walks the tree
with `dynamic_cast`.

### 3.6 `symbol_table/` — scopes

`symbol_table<Variable, Structure, Function>` is a header-only template with:

- A stack of variable scopes (index 0 holds globals).
- Separate maps for structs, functions and per-struct methods. `Structure` and
  `Function` must be pointer types (handles to AST nodes).
- Lookups that return a plain pointer, null when the name is missing
  (`lookup_variable`, `lookup_struct`, `lookup_function`, `lookup_method`).
- An RAII `scope_guard`: entering a block returns a guard that pops the scope when
  it goes out of scope, so scopes cannot leak on early returns or exceptions.

### 3.7 `semantic/` — semantic analysis

| File | Role |
|---|---|
| `semantic.h/.cpp` | `analyse(program_ptr, filename)`: runs `resolve_builtins`, then a visitor that resolves names, computes a `value_type` for every expression, and enforces the rules in [`semantic_spec.md`](semantic_spec.md). It returns an `analysis_root`: the checked AST plus indexes of method declarations by struct (`impls`) and invariants by state variable (`invs`) that borrow nodes from it |
| `builtins.h/.cpp` | `resolve_builtins(program)`: binds `print` / `scan` calls by lexical scope, checks arity, argument types and storage, and rejects I/O inside steps, update functions and invariants (directly or through called helpers). It returns a `builtin_calls` map and throws `builtin_error` (a code, span and detail) |

`analyse` throws a `semantic_error` carrying the filename, a stable code such as
`SEM017`, the `source_span` and a message, printed as
`file:line:col-line:col: SEM017: message`. A `builtin_error` from the resolver is
rethrown as a `semantic_error` with the filename added, so I/O errors look like every
other semantic error and are found before the checker's own errors. Checks include:

- Declaration before use, no duplicates, no shadowing (`SEM001`, `SEM002`).
- Type rules, implicit `int ↔ float` only, exact types at call/return (`SEM003`), operand types (`SEM004`), argument count and return presence (`SEM006`, `SEM007`).
- All paths of a non-void function return (`SEM008`).
- `imut` initialisation and immutability (`SEM011`, `SEM012`).
- Array lengths/initialisers/indexes and pointer rules (`SEM013`–`SEM016`).
- State writes only in steps / update functions, update calls only in an active
  step, no nested steps, pure invariants, no early exit from a step
  (`SEM017`–`SEM020`, `SEM023`).
- No `print` / `scan` in steps, update functions or invariants, including through a
  called function (`SEM021`); `scan` targets and arguments (`SEM003`, `SEM004`,
  `SEM012`, `SEM017`), argument count (`SEM006`) and call syntax (`SEM018`).
- Exactly one `fn main()` (`SEM024`).

### 3.8 `optimizer/` — optimisation plan

`optimize(program, options)` does **not** modify the AST. It returns an
`optimization_plan`:

- `constants`: expressions folded to a known literal value.
- `omitted`: statements to skip (unreachable code, dead stores, false loops).
- `unrolled`: `for` loops to emit unrolled, with their trip count.

`optimization_options` holds `enabled` (default true), `max_unroll_iterations`
(default 8) and `max_unrolled_statements` (default 64).

The optimiser is a `visitor`, so a new AST node without a handler is a compile error.
Its pattern matchers deliberately stay `dynamic_cast` whitelists that decline
anything they do not recognise: `match_counted_loop`, `unrollable_body` and
`trip_count` for loop unrolling, and `classify` and `remove_dead_stores` for dead
local removal.

Keeping the AST intact preserves the node identities that invariant and impl
indexes point to. Details: [`optimisation.md`](optimisation.md).

### 3.9 `code_generator/` — C++ emission

`code_generator` is a small class, not a visitor. It holds the impl index
(`struct_impls_t`) and the invariant index (`state_invs_t`), both taken by value, and
has one member, `gen_code(const program&, optimization_options = {}) const`, which
returns C++20 lines as a `vector<string>`. All emission is done by `emitter`, a
`visitor` that lives inside `code_generator.cpp` and is built fresh for every
`gen_code` call, so no output, scope or step counter carries over between calls.

1. `gen_code` runs `resolve_builtins`, then `optimize` (which does nothing when
   `options.enabled` is false), and hands both results to a new `emitter`.
2. The emitter writes headers, globals/state, structs, invariants as `bool`
   lambdas, functions and `main`. `int` becomes `std::int32_t`, `float` becomes
   `double`, arrays become `std::array`, and user names get a `jaqr_` prefix (see
   [`generated-names.md`](generated-names.md)).
3. It lowers each `step` into a transaction: copy every visible mutable state
   variable (immutable state is read directly), run the body on the copies, evaluate
   every invariant that is in force, then commit the copies only if all pass;
   otherwise run the `reject` block, if there is one. An invariant is in force for
   the steps that follow it in the source: a top-level `inv` for the rest of the
   program, a block-level `inv` for the rest of its block.
4. It emits `print` / `scan` as `jaqr_runtime::print` / `jaqr_runtime::scan`, and
   includes `<jaqr/runtime/io.h>` only when they are used.

State used in a step must be a value type without pointers (pointers would still
alias the original). Otherwise `gen_code` throws `std::invalid_argument`, which the
driver prints as `Error: Step state must have a value type without pointers: <name>`.

### 3.10 `runtime/` — support library

`runtime/jaqr/runtime/io.h` defines `jaqr_runtime::print` (`std::cout << value`)
and `jaqr_runtime::scan` (`std::cin >> value`, returns success as `bool`).
See [`io.md`](io.md).

## 4. Tests

| File | Type | What it covers |
|---|---|---|
| `tests/tokeniser_tests.cpp` | GoogleTest | Every token kind, literal edge cases, `LEX` errors |
| `tests/parser_tests.cpp` | GoogleTest | Every grammar rule, precedence, literal decoding, `SYN` errors |
| `tests/tokeniser_parser_integration_tests.cpp` | GoogleTest | Real source text through both stages, AST dumps, error positions |
| `tests/ast_dump.*`, `tests/test_helpers.h` | Helpers | S-expression AST printer and token builders |
| `tests/optimizer.cpp` + `optimizer_test.py` | CTest `optimizer_semantics` | Generates C++ with and without optimisation, compiles at `-O0`/`-O2`, compares behaviour |
| `tests/io.cpp` + `io_test.py` | CTest `io_semantics` | Generated I/O with piped input, EOF, failures, shadowing, rejected contexts |
| `tests/names.cpp` (also run by `optimizer_test.py`) | CTest `generated_names` | Generated names: keywords, prefixed and escaped names, with optimisation on and off |
| `tests/e2e/run_e2e.py` + `tests/e2e/cases/` | CTest `e2e`, or Python directly | Whole `.jaqr` programs through the real `jaqr` binary: output, compile success, rejection codes |

The three generated-code fixtures (`names.cpp`, `io.cpp`, `optimizer.cpp`) build
hand-made ASTs and call `code_generator` directly; the Python scripts compile and run
what they print. The scripts run each fixture twice; the second time they pass
`--no-optimize`, an extra argument that turns the optimiser off. It is a convention of
the fixtures, not an option of `jaqr`.

The GoogleTest suites are configured in `tests/CMakeLists.txt`, which fetches
GoogleTest 1.15.2 and builds the front end as a static library `jaqr_front_end`.
Run everything with `just test` (configures with `BUILD_TESTING=ON`, builds, runs
`ctest`).

### 4.1 End-to-end tests

These run real `.jaqr` files through `jaqr --exec <case> -o <tempfile>`, which compiles
each program and runs it. Every case gets its own temporary directory for the `-o` file.
They test the whole pipeline: tokeniser, parser, semantic analysis, optimiser, code
generation, the C++ build and the runtime. `ctest` runs them as the test `e2e`, with
the C++ compiler CMake used.

Run them alone after building:

```sh
just e2e                                   # builds, then runs all cases
just e2e -k step                           # only cases with "step" in the name

python tests/e2e/run_e2e.py                # the same without Just (needs a built jaqr)
python tests/e2e/run_e2e.py --cxx clang++  # C++ compiler jaqr uses (sets JAQR_CXX)
python tests/e2e/run_e2e.py --jaqr build/release/jaqr
```

The runner prints `ok` or `FAIL` per case, then the details of each failure, and
exits non-zero if anything failed. A failing run case says which stage broke: jaqr
rejected the program, the generated C++ did not compile, or the program failed.

There are two kinds of case:

| Folder | Contract | Files |
|---|---|---|
| `cases/run/` | `jaqr` accepts the program, the generated C++ compiles, the program exits 0 and prints exactly the expected output | `name.jaqr`, `name.out` (committed expected stdout), optional `name.in` (stdin) |
| `cases/reject/` | `jaqr` exits with an error without crashing, reports the expected code, never reaches the C++ compiler, and the program never runs | `name.jaqr` whose first line is `# expect: SEM017` (any `LEX`, `SYN` or `SEM` code) |

The run cases cover every core feature: I/O (including bad input and EOF), every
operator and literal form, casts, control flow, functions, recursion, arrays,
pointers, structs and methods, globals, steps (commit, rollback, atomicity across
several state variables, arrays and struct state), C++ name collisions and
optimiser rewrites. The reject cases cover every diagnostic code in
[`semantic_spec.md`](semantic_spec.md#diagnostic-catalogue) that the compiler can
report, except `SEM025`, plus `LEX001` and `SYN001`.

To add a case, drop the files into the right folder. No registration is needed.
For a run case, write `name.jaqr` (and `name.in` if it reads input), check by hand
what the program should print, then create `name.out` from
`jaqr --exec name.jaqr -o scratch.cpp < name.in`. `.gitignore` ignores `*.out`
files except under `tests/e2e/cases/`, so `name.out` is committed with the case.
For a reject case, put `# expect: CODE` on the first line.

On Windows the runner puts the toolchain that built `jaqr` (from
`build/debug/CMakeCache.txt`) and the `--cxx` compiler first on `PATH`. Without
this, a MinGW `jaqr.exe` started from Git Bash loads Git's `libstdc++-6.dll` and
exits with `0xC0000139` before printing anything.

## 5. Build system

- `CMakeLists.txt` uses `file(GLOB_RECURSE ... CONFIGURE_DEPENDS src/*.cpp)`, so new
  `.cpp` files under `src/` are picked up on the next build without editing CMake.
- The option `JAQR_WARNINGS` (default `ON`) builds `jaqr` with `-Wall -Wextra`
  (`/W4` with MSVC).
- `CMakePresets.json` defines `debug` and `release` with the Ninja generator and
  exports `compile_commands.json` for clangd and clang-tidy.
- `include(CTest)` makes `BUILD_TESTING` default to `ON`. With it on, `tests/` is
  added (GoogleTest suites), and `CMakeLists.txt` defines the fixtures `name_tests`,
  `io_tests` and `optimizer_tests` and the `e2e` test. Those fixtures compile a fixed
  list of sources, `JAQR_CODEGEN_SOURCES`, so a new file the code generator depends
  on has to be added there (and to `jaqr_front_end` in `tests/CMakeLists.txt` if the
  front-end tests need it).
- `just test` builds with tests and runs `ctest`; `just e2e` runs only the
  end-to-end cases.
- CI (`.github/workflows/checks.yml`) runs on every pull request. One job runs
  `just format-check` and `just lint`. Another builds with `BUILD_TESTING=ON` using
  `g++-13` and runs `ctest`.

## 6. Adding a language feature

1. Add tokens to `tokens/token.h` and DFA nodes to `tokeniser/dfa.cpp`.
2. Update the rule in `ast/grammar_rules/*.txt`.
3. Add or change AST nodes in `ast/` and a `make_*` builder.
4. Parse it in `parser/parser.cpp`.
5. Add a `visit` overload to `visitor/visitor.h`; the compiler then lists every
   pass that must handle it (the semantic checker, the optimiser and the code
   generator's emitter). `semantic/builtins.cpp` is not a visitor, so check it by
   hand.
6. Type-check it in `semantic/semantic.cpp`.
7. Emit it in `code_generator/code_generator.cpp`.
8. Add tests in `tests/`: unit tests, and an end-to-end case in
   `tests/e2e/cases/` (see 4.1).
