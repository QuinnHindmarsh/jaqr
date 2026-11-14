# Code Structure

A multi-pass compiler in C++20. Each pass lives in its own folder under `src/`.

## 1. Pipeline

```text
source.jaqr
  → Tokeniser    src/tokeniser/       DFA, maximal munch          LEX errors
  → Parser       src/parser/, ast/    recursive descent           SYN errors
  → Semantic     src/semantic/        scopes, types, step rules   SEM errors
  → Optimiser    src/optimizer/       folding, dead code, unrolling
  → Code gen     src/code_generator/  emits C++20
  → Driver       src/run.cpp          writes C++; --exec compiles and runs it
```

`src/main.cpp` runs tokenise → parse → analyse, then `code_generator::gen_code`,
which runs the optimiser and emitter.

## 2. Folder map

```text
jaqr/
├── runtime/jaqr/runtime/io.h   print / scan used by generated C++
├── docs/
├── tests/                      see tests.md
└── src/
    ├── main.cpp, run.cpp       command-line driver
    ├── tokens/, tokeniser/     lexical analysis
    ├── parser/, ast/           syntax analysis and AST nodes
    ├── visitor/                visitor interface over the AST
    ├── symbol_table/           scoped symbol table
    ├── semantic/               semantic checks + print/scan resolver
    ├── optimizer/              optimisation plan
    └── code_generator/         C++ emission
```

## 3. Adding a language feature

1. Add tokens in `tokens/token.h` and `tokeniser/dfa.cpp`
2. Update `ast/grammar_rules/*.txt` and add AST nodes in `ast/`
3. Parse it in `parser/parser.cpp`
4. Add a `visit` overload in `visitor/visitor.h`; the build then flags every pass to update
5. Type-check in `semantic/semantic.cpp`, emit in `code_generator/code_generator.cpp`
6. Add tests (see [tests.md](tests.md))
