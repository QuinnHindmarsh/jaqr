# JAQR documentation

Start with the [project README](../README.md) to build JAQR and run a program.

## Using the language

- [Language features](features.md): a tour of types, functions, arrays and the `step` system.
- [Example programs](sample_code.md): programs to save and run with `--exec`, with their output.
- [Grammar](grammar.md): the lexical and syntactic grammar and operator precedence.
- [Semantic rules](semantic_spec.md): type rules, restrictions, runtime behaviour and the diagnostic codes.
- [Standard I/O](io.md): how `print` and `scan` behave.

## Working on the compiler

- [Code structure](code_structure.md): the compiler passes, the source tree and the tests.
- [Optimisation](optimisation.md): the optimiser's passes and options.
- [Generated names](generated-names.md): how JAQR names map to C++ names.

The grammar rules the parser follows are also kept next to the code, in
`src/ast/grammar_rules/`.
