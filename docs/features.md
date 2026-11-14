# Language Features

JAQR is a statically typed, C-like language that compiles to C++20. State can only
change inside a `step`, and a step commits only if every invariant still holds.
Otherwise all its changes are rolled back.

Full rules: [semantic_spec.md](semantic_spec.md). Syntax: [grammar.md](grammar.md).

## Summary

| Area | Features |
|---|---|
| Types | `int`, `float`, `char`, `byte`, `bool`, arrays `[N]T`, pointers `->T`, structs |
| Variables | `let` (mutable), `imut` (immutable), `state` (transactional), `define` (global) |
| Control flow | `if` / `elif` / `else`, `while`, `for`, `return` |
| Functions | Typed params, recursion, methods, update functions |
| Operators | `+ - * /`, floor division `\`, power `~`, bitwise, logical, pointer `@` `<-` |
| Literals | `42`, `2b101`, `16bff`, `3.14`, `'a'`, `"hi"`, `true` |
| I/O | `print(value)`, `scan(target)` |

## Basics

```text
struct Point { x: int; y: int; };

fn square(x: int): int {
    return x * x;
}

fn main() {
    let a: [3]int = [1, 2, 3];       # arrays are copied by value
    imut p: Point = Point{x: 1, y: 2};
    let x: int = 5;
    let ptr: ->int = @x;             # address-of
    <-ptr = 10;                      # dereference
    let f: float = float(7) / 2.0;   # explicit cast

    for (let i: int = 0; i < 3; i = i + 1) {
        print(square(a[i]));
    }
}
```

Key rules:

- No shadowing, no overloading, no `%`, `++` or compound assignment
- Only implicit conversion is `int ↔ float`
- Functions must be defined before use
- Exactly one `fn main()`

## Step system

```text
define state balance: int = 100;
inv { balance >= 0 };

update fn deposit(b: int)(amount: int) {
    b = b + amount;
}

fn main() {
    step {
        balance = balance - 30;
        deposit(balance)(10);
    } reject {
        print("rolled back\n");
    }
}
```

- `state` is readable anywhere but writable only inside a step or update function
- `inv` must be a pure `bool` expression over state; it guards every later step
- A step commits only if all invariants pass; otherwise changes are discarded and `reject` runs
- No I/O, nested steps or `return` inside a step
- Update functions take state in the first parameter list and must be called inside a step

## I/O

- `print(value)` writes `int`, `float`, `char`, `bool` or a string literal. No newline added.
- `scan(target)` reads into a mutable, non-state variable and returns `true` on success.
