# HumC

**HumC** is a programming language that uses indentation to define blocks of code; code written in this language is translated into standard C. **Its goal is to make C-style programming clearer and simpler** through the use of indentation and concise constructs, such as `let`, `fn`, and `print`.

This project is a full-fledged compiler for the C11 standard, designed to be used via the command line. The compiler itself is written in C.


## How HumC works

HumC translates `.humc` source files into `.c` files. The generated C code can then be compiled by GCC, Clang, or another compatible C compiler.

```text
program.humc  ->  program.c  ->  cc/gcc/clang  ->  executable
```

For example, this HumC:

```humc
fn main
    let a = 10
    let b = 32
    print a + b
    return 0
```

is translated to C similar to:

```c
#include <stdio.h>

int main(void) {
    int a = 10;
    int b = 32;
    printf("%lld\n", (long long)(a + b));
    return 0;
}
```

The exact generated code can change as the compiler evolves. C preprocessor lines and many ordinary C statement expressions are currently passed through, but HumC does not yet parse or validate the entire C language.


### Build the compiler

```sh
make
make test
```

### Install HumC globally in PREFIX (default: ~/.local)

```sh
make install
```
## CLI

### Translate one file

```sh
humc examples/basic.humc
```

By default, the generated `.c` file is written alongside the source. Specify an output path with `-o`:

```sh
humc examples/basic.humc -o build/basic.c
```

### Build a file or directory

```sh
humc build examples/supported.humc
humc build examples -o build/generated
```

### Check syntax

`check` runs the HumC translation/validation path without writing generated `.c` files. It is not a substitute for compiling the generated C with GCC or Clang.

```sh
humc check examples/basic.humc
humc check examples
```

### Run a program

```sh
humc run examples/basic.humc
```

To choose a C compiler:

```sh
CC=gcc humc run examples/basic.humc
```

Program arguments can be placed after the source filename:

```sh
humc run program.humc arg1 arg2
```

### Format source files

```sh
humc fmt examples/basic.humc
humc fmt examples
```

### Watch for changes

```sh
humc watch examples/supported.humc
```

### Initialize a project

```sh
humc init myapp
cd myapp
```

```sh
make
./app
```

## Basic syntax

```bash
print "Hello from HumC!"

let name = "Bob"
let age = 25
let city = "Warsaw"

print name
print age
print city
```

Basic variable types are inferred from supported literal initializers. Explicit C type annotations can be used where inference is not sufficient:

```bash
let message: const char * = "hello"
let value: unsigned long = 42
```

All `let` declarations in the supported safe subset must have an initializer.

### Math

```bash
let a = 10
let b = 5

let sum = a + b
let difference = a - b
let product = a * b
let division = a / b

print sum
print difference
print product
print division
```

### Conditions

```bash
let score = 75

if score >= 90
    print "Excellent"
elif score >= 70
    print "Good"
elif score >= 50
    print "Pass"
else
    print "Fail"
```

### Loops

```bash
let i = 0

while i < 5
    print i
    i++
```

Numeric ranges use an exclusive upper bound:

```bash
for i in 0..3
    print i
```

Array iteration is supported for named numeric arrays initialized with literals:

```bash
let numbers = [1, 2, 3, 4, 5]

for number in numbers
    if number == 3
        continue
    print number
```

### Functions

```bash
fn greet(name)
    print "Hello " + name

greet("Bob")
greet("Alex")
```

A function can return a value:

```bash
fn add(int a, int b) -> int
    return a + b

let result = add(10, 20)
print result
```

### Switch

```bash
let value = 2

switch value
    case 1
        print "one"
        break
    case 2
        print "two"
        break
    default
        print "other"
```

## Safety status

Implemented for the currently supported subset:

- Accesses to known local fixed arrays are generated with runtime bounds checks for supported read/write forms.
- Negative and out-of-range indices in these supported forms produce a runtime diagnostic.
- `let` variables in the safe subset require initializers.
- Duplicate declarations in one lexical scope are rejected.
- Variables in closed blocks are not intended to leak into sibling scopes.
- C keywords are rejected as HumC identifiers.
- Arena allocation checks size arithmetic before allocating chunks.

Temporary compiler data is allocated from an arena and released together when a compilation finishes. The generated C output is returned as a separate allocation. The arena simplifies cleanup for temporary data, but it does not automatically manage memory used by programs generated from HumC.