# Optimisation

The optimiser runs inside `code_generator::gen_code` and is always on from the
command line. It builds a plan and never modifies the AST.

## Passes

- **Constant folding**: integer `+ - *`, comparisons, boolean operators and negation
- **Dead code removal**: statements after `return`, unused constant locals, dead stores
- **Constant branches**: `if (true)` / `if (false)` simplified; constant-false loops removed
- **Loop unrolling**: small `for` loops with constant bounds (max 8 iterations, 64 statements)

## Not optimised

- Floating-point maths, division and shifts
- Function calls (never assumed pure)
- Step bodies and `reject` blocks
