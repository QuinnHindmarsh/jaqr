# JAQR standard I/O

`print(value)` and `scan(target)` are built-in prelude functions. They use ordinary
function-call syntax and require no JAQR declarations or parser special cases.

- `print(value)` returns void and performs `std::cout << value`. It adds no spaces,
  newline or explicit flush. Use `print("\n")` for a newline.
- `scan(target)` performs `std::cin >> target` and returns bool: true when the stream
  has neither its failure nor bad-state flag set. The target is passed by reference.
  Input failures and EOF retain the normal C++ stream state; calling scan again does
  not clear a previous failure. Failed extraction may modify the target, exactly as
  the underlying stream operation does.
- Both take exactly one argument, written as an ordinary call. They support int,
  float, char and bool. Printing also supports string literals written directly as
  the argument (`print("hi")`); a `char` array is not printable. Default bool
  formatting is 0/1. Formatted char extraction skips leading whitespace.
- Scan needs mutable, non-state storage, including supported array elements and
  struct fields. Constants, temporary values and expressions such as `scan(x + 1)`
  are rejected. Byte, whole arrays, structs and pointer values are not printable or
  scannable as values in this initial library. A suitably typed pointer may denote
  a scalar scan target by dereference; pointer/alias legality remains the general
  semantic checker's responsibility.

The generated calls are qualified as `jaqr_runtime::print` / `jaqr_runtime::scan`.
The implementations live in `runtime/jaqr/runtime/io.h`. Compile generated programs
using the runtime include directory:

```sh
c++ -std=c++20 -O2 -I runtime generated.cpp -o generated
```

The include path above is relative to the repository root; use an absolute path
when compiling elsewhere. The runtime header is included only when the program
resolves a built-in I/O call.

## Compiler integration

`gen_code` runs `resolve_builtins` before optimisation. The resolver registers the
prelude below user scopes, identifies calls by their lexical binding, checks arity
and supported argument types/storage, and produces a call-node-to-built-in map.
User-defined functions or local bindings named print/scan shadow the prelude. Member
calls are resolved as members, never rewritten merely because their name matches.

Both operations have observable effects; the optimiser retains calls and input
writes. Direct calls in steps, update functions and invariants are rejected. The
resolver also propagates I/O effects through resolved user functions and methods,
so calling an I/O helper from those contexts is rejected. A reject handler executes
outside the failed step and may perform I/O.

This focused built-in resolver is not a complete semantic analyser: ordinary call
validity, full type checking and pointer/state alias rules still require that stage.
`analyse` (src/semantic/semantic.cpp) runs the resolver first, before its own checks,
so the command-line compiler reports every rule below before any other semantic
error. The resolver throws a `builtin_error` and `analyse` rethrows it as a
`semantic_error` with the file name, so it prints like every other semantic error:

```text
file.jaqr:0:12-0:22: SEM006: print requires exactly one argument
```

| Code | Rule |
|---|---|
| `SEM018` | `print` or `scan` written as an update call, `print(x)(y)` |
| `SEM006` | Not exactly one argument |
| `SEM021` | Call in a step, update function or invariant, directly or through a called function |
| `SEM004` | `scan` target is a computed value rather than storage, such as `x + 1` |
| `SEM012` | `scan` target is immutable |
| `SEM017` | `scan` target is state |
| `SEM003` | Unsupported argument type, such as `byte`, a pointer, a struct or an array |

`code_generator::gen_code` calls the resolver again to get the call map. A program
that did not go through `analyse` gets the same rules there as a `builtin_error`,
which is a `std::invalid_argument`.

Run `just test` for generated-code tests with piped input, exact output comparisons,
input failure and EOF, name shadowing, rejected calls and step/update restrictions.
Both optimisation modes and C++ `-O0`/`-O2` are exercised.
