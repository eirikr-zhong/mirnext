# MIR register IR and C++ API

MIRNext uses a register IR with stable C++ objects. Public instructions retain
MIR semantics and file spellings; SSA and machine IR are internal stages. The
product API is declared in [IR.hpp](include/mirnext/IR/IR.hpp). The descriptor
table in [Opcodes.def](include/mirnext/IR/Opcodes.def) defines enum names, binary
numbers, operand roles, arity and control-flow classification. Lowercase names
below are MIR text spellings.

## Ownership and construction

`Context` owns modules and interns strings, types and signatures. A `Module`
owns stable symbols, function definitions and ordered data items. A `Function`
owns blocks, registers and label anchors; blocks own instructions. Several
labels can identify one block entry. Block layout order preserves fallthrough.
Ordinary inspection does not rewrite the body or create a second instruction
representation.

Use `Context::create_module`, `Module::create_function`,
`Function::create_block` and an `IRBuilder` at that block. Operations return
`Result`; inspect failures before using their handles. Builder records its first
error. `verify()` checks functions, modules or the Context. References carry
ownership; registers and labels come from their owner, not raw numbers.
The [introductory executable](README.md#build-and-execute-ir) shows a complete
program with error handling.

Builder provides expressions, assignments, memory access, branches, calls,
stack scopes and varargs. `emit(Opcode, span<const Operand>)` and its
initializer-list overload use the same validation. Use `IRRewriter` to change
existing IR; it maintains revisions and analysis invalidation. Internal SSA
edits use the SSA interfaces rather than ordinary Builder emission.

## Types and signatures

`Signature` contains result types, parameter types and a `variadic` flag.
Definitions, prototypes and imports share this model. A prototype is a symbol
with a signature, not an instruction-free Function.

| MIR type | C++ representation | Meaning |
| --- | --- | --- |
| `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64` | `Type::Kind::{I8,U8,I16,U16,I32,U32,I64,U64}` | Integer storage and ABI types |
| `f`, `d`, `ld` | `Type::Kind::{F32,F64,LongDouble}` | Floating types; long double follows the native layout |
| `p` | `Type::pointer()` | Pointer; maintained targets have 64-bit pointers |
| `blk`, `blk1`–`blk4` | `Type::block(size, BlockClass)` | Sized, ABI-classified block parameter |
| `rblk` | `Type::return_block(size)` | Implicit result-buffer parameter |

`Type::Kind::Bool` is a C++ DSL scalar materialized with MIR scalar semantics.
`Type::Kind::Undef` is restricted to special permitted memory operands. Blocks
require a positive size and valid ABI class; they cannot be ordinary scalar
results or local registers. `BlockArgument(address, size, classification)` and
`BlockArgument::return_buffer` encapsulate call blocks; see the
[block-call example](examples/native-calls.cpp).

Registers hold scalar values; instruction width and signedness determine
arithmetic behavior. Narrow loads and ABI arguments extend into the scalar
representation, while narrow stores and results truncate. Hardware binding is a
register attribute: the same binding within a function refers to one register,
including writes introduced by inlining. SSA definitions distinguish writes.
A bound register has no memory address; copy to an ordinary local if addressable
storage is needed.

## Modules, symbols and data

`declare_function` and `define_function` share one Symbol identity across a
forward declaration and its definition. `Module::items()` retains module order;
`functions()` and `data()` expose typed owner ranges.

| Item | MIR spelling | C++ API / initializer |
| --- | --- | --- |
| Module | `name: module` … `endmodule` | `Context::create_module` |
| Function | `name: func` … `endfunc` | `create_function` / `define_function` |
| Prototype | `name: proto` | `prototype` |
| Import / export | `import name` / `export name` | `import` / `export_symbol` |
| Forward declaration | `forward name` | `forward` |
| Scalar array / string | `[name:] type values` / `[name:] string "..."` | `InitializedData` (strings use byte elements) |
| Zero storage | `[name:] bss bytes` | `ZeroData` |
| Symbol address | `[name:] ref symbol[, displacement]` | `SymbolReference` |
| Label address / difference | `[name:] lref label[, relative_label][, displacement]` | `LabelReference` |
| Evaluated initializer | `[name:] expr function` | `ExpressionData` |

`create_data` defines data; `declare_data` / `define_data` reserve its ordered
slot first. Reference data can address functions, data or imports. Expression
data requires a verified zero-argument function with one scalar result; calls,
writes and arbitrary memory effects are rejected before execution. Consecutive
anonymous data items extend the preceding section without padding. A named or
non-data item starts a new layout boundary; explicitly pad packed sections when
needed. Zero storage starts cleared; references and expressions initialize
through the loaded symbol image.

`create_variable` binds a typed scalar to initialized or zero storage. Builder
`load`, `store`, `address_of` and variable expressions generate accesses;
a read-only binding rejects writes. See [module variables](examples/native-variables.cpp).

## Operands and memory

`Operand` holds explicit signed/unsigned integers and floating values, strings,
register and label references, `SymbolReference` and `Memory`. Output operands
come first, with instruction-specific exceptions such as branches and calls.
Registers and labels belong to the function; symbols resolve in the module and
import environment.

`Memory` contains a type, optional base/index registers, displacement and scale.
Its address is `displacement + base + index * scale`. Integer loads sign- or
zero-extend according to storage type; stores truncate. Text forms are
`type:displacement` and `type:[displacement](base[, index[, scale]])`, with
omitted displacement zero and scale 1, 2, 4 or 8. `SymbolReference` retains a
relocation until addresses are known. `address_of(symbol)` uses a symbol move;
`address_of(register)` selects the appropriate `addr` instruction.

## Instruction semantics

Counts below describe public IR. `call`, `inline`, `ret` and `switch` have
signature- or list-dependent counts. `label` is a block-entry anchor, not an
ordinary instruction in this owning model. Floating comparisons retain NaN
behavior: unordered inequality is true and ordered relational comparisons
are false.

### move insns
  * There are following MIR move insns:

    | Insn Code               | Nops |   Description                                          |
    |-------------------------|-----:|--------------------------------------------------------|
    | `mov`               | 2    | move 64-bit integer values                             |
    | `fmov`              | 2    | move **single precision** floating point values        |
    | `dmov`              | 2    | move **double precision** floating point values        |
    | `ldmov`             | 2    | move **long double** floating point values             |

### integer insns
  * If insn has suffix `s` in insn name, the insn works with lower 32-bit part of 64-bit integer value
  * The higher part of 32-bit insn result is undefined
  * If insn has prefix `u` in insn name, the insn treats integer as unsigned integers
  * Some instructions have no unsigned variant because MIR uses two's-complement
    arithmetic. In particular, `mul` and `muls` keep the low 64 or 32 product
    bits, which are the same for signed and unsigned multiplication.

    | Insn Code               | Nops |   Description                                          |
    |-------------------------|-----:|--------------------------------------------------------|
    | `ext8`              | 2    | **sign** extension of lower **8 bit** input part       |
    | `uext8`             | 2    | **zero** extension of lower **8 bit** input part       |
    | `ext16`             | 2    | **sign** extension of lower **16 bit** input part      |
    | `uext16`            | 2    | **zero** extension of lower **16 bit** input part      |
    | `ext32`             | 2    | **sign** extension of lower **32 bit** input part      |
    | `uext32`            | 2    | **zero** extension of lower **32 bit** input part      |
    |                         |      |                                                        |
    | `neg`               | 2    | changing sign of **64-bit** integer value               |
    | `negs`              | 2    | changing sign of **32-bit** integer value               |
    |                         |      |                                                        |
    | `add`, `sub`    | 3    | **64-bit** integer addition and subtraction            |
    | `adds`, `subs`  | 3    | **32-bit** integer addition and subtraction            |
    | `mul`, `div`    | 3    | **64-bit signed** multiplication and division          |
    | `udiv`                  | 3    | **64-bit unsigned** integer division                   |
    | `muls`, `divs`  | 3    | **32-bit signed**  multiplication and division          |
    | `udivs`                 | 3    | **32-bit unsigned** integer division                   |
    | `mod`               | 3    | **64-bit signed**  modulo operation                    |
    | `umod`              | 3    | **64-bit unsigned** integer modulo operation           |
    | `mods`              | 3    | **32-bit signed**  modulo operation                    |
    | `umods`             | 3    | **32-bit unsigned** integer modulo operation           |
    |                         |      |                                                        |
    | `and`, `or`     | 3    | **64-bit** integer bitwise AND and OR                  |
    | `ands`, `ors`   | 3    | **32-bit** integer bitwise AND and OR                  |
    | `xor`               | 3    | **64-bit** integer bitwise XOR                         |
    | `xors`              | 3    | **32-bit** integer bitwise XOR                         |
    |                         |      |                                                        |
    | `lsh`               | 3    | **64-bit** integer left shift                          |
    | `lshs`              | 3    | **32-bit** integer left shift                          |
    | `rsh`               | 3    | **64-bit** integer right shift with **sign** extension |
    | `rshs`              | 3    | **32-bit** integer right shift with **sign** extension |
    | `ursh`              | 3    | **64-bit** integer right shift with **zero** extension |
    | `urshs`             | 3    | **32-bit** integer right shift with **zero** extension |
    |                         |      |                                                        |
    | `eq`, `ne`      | 3    | equality/inequality of **64-bit** integers             |
    | `eqs`, `nes`    | 3    | equality/inequality of **32-bit** integers             |
    | `lt`, `le`      | 3    | **64-bit signed** less than/less than or equal         |
    | `ult`, `ule`    | 3    | **64-bit unsigned** less than/less than or equal       |
    | `lts`, `les`    | 3    | **32-bit signed** less than/less than or equal         |
    | `ults`, `ules`  | 3    | **32-bit unsigned** less than/less than or equal       |
    | `gt`, `ge`      | 3    | **64-bit signed** greater than/greater than or equal   |
    | `ugt`, `uge`    | 3    | **64-bit unsigned** greater than/greater than or equal |
    | `gts`, `ges`    | 3    | **32-bit signed** greater than/greater than or equal   |
    | `ugts`, `uges`  | 3    | **32-bit unsigned** greater than/greater than or equal |

### integer overflow insns
  * All the insns set up an overflow flag which can be checked by branches on overflow `bo`, `bno`, `ubo`, and `ubno`
  * If insn has suffix `s` in insn name, the insn works with lower 32-bit part of 64-bit integer value
  * The higher part of 32-bit insn result is undefined
  * If insn has prefix `u` in insn name, the insn treats integer as unsigned integers

    | Insn Code               | Nops |   Description                                          |
    |-------------------------|-----:|--------------------------------------------------------|
    | `addo`, `subo`  | 3    | **64-bit** integer addition and subtraction            |
    | `addos`, `subos`| 3    | **32-bit** integer addition and subtraction            |
    | `mulo`, `mulos` | 3    | **signed** multiplication, 64-bit / 32-bit          |
    | `umulo`, `umulos`| 3    | **unsigned** multiplication, 64-bit / 32-bit |

### floating point insns
  * If insn has prefix `f` in insn name, the insn is single precision floating point insn.  Its operands should have `f` type
  * If insn has prefix `d` in insn name, the insn is double precision floating point insn.  Its operands should have `d` type
  * Otherwise, insn has prefix `ld` in insn name and the insn is a long double insn.
    Its operands should have `ld` type.
  * The result of comparison insn is a 64-bit integer value, so the result operand should be of integer type

    | Insn Code                            | Nops |   Description                                                   |
    |--------------------------------------|-----:|-----------------------------------------------------------------|
    | `f2i`, `d2i`, `ld2i`     | 2    | transforming floating point value into 64-bit integer           |
    | `f2d`                            | 2    | transforming single to double precision FP value                |
    | `f2ld`                           | 2    | transforming single precision to long double FP value           |
    | `d2f`                            | 2    | transforming double to single precision FP value                |
    | `d2ld`                           | 2    | transforming double precision to long double FP value           |
    | `ld2f`                           | 2    | transforming long double to single precision FP value           |
    | `ld2d`                           | 2    | transforming long double to double precision FP value           |
    | `i2f`, `i2d`, `i2ld`     | 2    | transforming 64-bit integer into a floating point value         |
    | `ui2f`, `ui2d`, `ui2ld`  | 2    | transforming unsigned 64-bit integer into a floating point value|
    | `fneg`, `dneg`, `ldneg`  | 2    | changing sign of floating point value                           |
    | `fadd`, `fsub`               | 3    | **single** precision addition and subtraction                   |
    | `dadd`, `dsub`               | 3    | **double** precision addition and subtraction                   |
    | `ldadd`, `ldsub`             | 3    | **long double** addition and subtraction                        |
    | `fmul`, `fdiv`               | 3    | **single** precision multiplication and division                 |
    | `dmul`, `ddiv`               | 3    | **double** precision multiplication and division                 |
    | `ldmul`, `lddiv`             | 3    | **long double** multiplication and division                      |
    | `feq`, `fne`                 | 3    | equality/inequality of **single** precision values              |
    | `deq`, `dne`                 | 3    | equality/inequality of **double** precision values              |
    | `ldeq`, `ldne`               | 3    | equality/inequality of **long double** values                   |
    | `flt`, `fle`                 | 3    | **single** precision less than/less than or equal               |
    | `dlt`, `dle`                 | 3    | **double** precision less than/less than or equal               |
    | `ldlt`, `ldle`               | 3    | **long double** less than/less than or equal                    |
    | `fgt`, `fge`                 | 3    | **single** precision greater than/greater than or equal         |
    | `dgt`, `dge`                 | 3    | **double** precision greater than/greater than or equal         |
    | `ldgt`, `ldge`               | 3    | **long double** greater than/greater than or equal              |

### address insns
  * The insns take address of variable as the 2nd operand and put it into the 1st operand
    * They are used to implement C expression `&local_variable`

    | Insn Code               | Nops |   Description                                                         |
    |-------------------------|-----:|-----------------------------------------------------------------------|
    | `addr`              | 2    | Take address of variable in its natural type                          |
    | `addr8`             | 2    | Take address of variable treated as 8-bit value                       |
    | `addr16`            | 2    | Take address of variable treated as 16-bit value                      |
    | `addr32`            | 2    | Take address of variable treated as 32-bit value                      |

  * `addr` is used to address variables keeping values of 64-bit integer, float, double, or long double types
  * Other address insns are used to address variables keeping integer values of smaller types
  * Usage of the right address insn is important for big-endian targets

### branch insns
  * The first operand of the insn should be label

    | Insn Code               | Nops |   Description                                                         |
    |-------------------------|-----:|-----------------------------------------------------------------------|
    | `jmp`               | 1    | unconditional jump to the label                                       |
    | `bt`                | 2    | jump to the label when 2nd **64-bit** operand is **nonzero**          |
    | `bts`               | 2    | jump to the label when 2nd **32-bit** operand is **nonzero**          |
    | `bf`                | 2    | jump to the label when 2nd **64-bit** operand is **zero**             |
    | `bfs`               | 2    | jump to the label when 2nd **32-bit** operand is **zero**             |
    | `jmpi`              | 1    | unconditional jump to address in **64-bit** operand (see `laddr`) |

### branch on overflow insns
  * The first operand of the insn should be label
  * A compatible overflow instruction must reach this branch. Verification checks the producer, intervening instructions and signedness

    | Insn Code               | Nops |   Description                                                         |
    |-------------------------|-----:|-----------------------------------------------------------------------|
    | `bo`                | 1    | jump to the label when signed overflow is set up                      |
    | `bno`               | 1    | jump to the label when signed overflow is not set up                  |
    | `ubo`               | 1    | jump to the label when unsigned overflow is set up                    |
    | `ubno`              | 1    | jump to the label when unsigned overflow is not set up                    |

### `laddr` insn
  * The insn takes address of a function label given as the 2nd operand and put it into 64-bit integer register
    or memory given as the the first operand
  * The insn with `jmpi` insn is used to implement GNU C extension "labels as values"

### switch insn
  * The first operand of `switch` insn should have an integer value from 0 to `N - 1` inclusive
  * The rest operands should be `N` labels, where `N > 0`
  * Execution of the insn will be an jump on the label corresponding to the first operand value
  * If the first operand value is out of the range of permitted values, the execution result is undefined

### integer comparison and branch insn
  * The first operand of the insn should be label.  Label will be the next executed insn if the result of comparison is non-zero

    | Insn Code               | Nops |   Description                                                 |
    |-------------------------|-----:|---------------------------------------------------------------|
    | `beq`, `bne`    | 3    | jump on **64-bit** equality/inequality                        |
    | `beqs`, `bnes`  | 3    | jump on **32-bit** equality/inequality                        |
    | `blt`, `ble`    | 3    | jump on **signed 64-bit** less than/less than or equal        |
    | `ublt`, `uble`  | 3    | jump on **unsigned 64-bit** less than/less than or equal      |
    | `blts`, `bles`  | 3    | jump on **signed 32-bit** less than/less than or equal        |
    | `ublts`, `ubles`| 3    | jump on **unsigned 32-bit** less than/less than or equal      |
    | `bgt`, `bge`    | 3    | jump on **signed 64-bit** greater than/greater than or equal  |
    | `ubgt`, `ubge`  | 3    | jump on **unsigned 64-bit** greater than/greater than or equal|
    | `bgts`, `bges`  | 3    | jump on **signed 32-bit** greater than/greater than or equal  |
    | `ubgts`, `ubges`| 3    | jump on **unsigned 32-bit** greater than/greater than or equal|

### floating point comparison and branch insn
  * The first operand of the insn should be label.  Label will be the next executed insn if the result of comparison is non-zero
  * See comparison semantics in the corresponding comparison insns

    | Insn Code                 | Nops |   Description                                                  |
    |---------------------------|-----:|----------------------------------------------------------------|
    | `fbeq`, `fbne`    | 3    | jump on **single** precision equality/inequality               |
    | `dbeq`, `dbne`    | 3    | jump on **double** precision equality/inequality               |
    | `ldbeq`, `ldbne`  | 3    | jump on **long double** equality/inequality                    |
    | `fblt`, `fble`    | 3    | jump on **single** precision less than/less than or equal      |
    | `dblt`, `dble`    | 3    | jump on **double** precision less than/less than or equal      |
    | `ldblt`, `ldble`  | 3    | jump on **long double** less than/less than or equal           |
    | `fbgt`, `fbge`    | 3    | jump on **single** precision greater than/greater than or equal|
    | `dbgt`, `dbge`    | 3    | jump on **double** precision greater than/less/ than or equal  |
    | `ldbgt`, `ldbge`  | 3    | jump on **long double** greater than/less/ than or equal       |

### ret insn
  * Return insn has zero or more operands
  * Return insn operands should correspond to return types of the function
  * 64-bit integer value is truncated to the corresponding function return type first
  * The return values will be the function call values

### call insn
  * The insn has variable number of operands
  * The first operand is a prototype reference operand
  * The second operand is a called function address
    * The prototype should correspond MIR function definition if function address represents a MIR function
    * The prototype should correspond C function definition if the address is C function address
  * If the prototype has *N* return types, the next *N* operands are
    output operands which will contain the result values of the function
    call
  * The subsequent operands are arguments.  Their types and number and should be the same as in the prototype
    * Integer arguments are truncated according to integer prototype argument type

### inline insn
  * This insn is analogous to `call` but during optimization its call
    may be replaced by the callee body when the inlining rules allow it
  * Calls of vararg functions are never inlined

### jcall and jret insns
  * `jcall` calls a function without setting up the return address
    (usually the return address is put on the stack or in a register depending on the ABI)
    * The first operand is a prototype reference operand
    * The second operand is a called function address
  * `jret` returns from the function called by `jcall`
    * The single operand contains the return address as 64-bit integer value
  * To call a function you can only use pairs `call` (or `inline`) and `ret` or `jcall` and `jret`
  * `jcall` and `jret` implement non-standard ABI for functions without args and return values
    * The ABI is useful for high performance switching from direct threading interpreters to JITted code
    * The argument and return values can be passed through global vars which are tied to specific hard regs

### alloca insn
  * Reserve memory on the stack whose size is given as the 2nd operand and assign the memory address to the 1st operand
  * The reserved memory will be aligned according target ABI

### bstart and bend insns
  * MIR users can use them implement blocks with automatic
    deallocation of memory allocated by `alloca` inside the
    blocks.  But mostly these insns are used to implement call
    inlining of functions using alloca
  * The both insns use one operand
  * The first insn saves the stack pointer in the operand
  * The second insn restores stack pointer from the operand

### va_start, va_arg, va_block_arg, and va_end insns
  * These insns are only for variable number arguments functions
  * `va_start` and `va_end` have one input operand, an address
    of va_list structure (see C stdarg.h for more details).  Unlike C
    va_start, va_start just takes one parameter
  * `va_arg` takes va_list and any memory operand and returns
    address of the next argument in the 1st insn operand.  The memory
    operand type defines the type of the argument
  * `va_block_arg` takes result address, va_list address, integer operand (size),
    and block type (case) number and moves the next argument passed as block of given
    size and type to the result address
  * va_list operand can be memory with undefined type.  In this case
    address of the va_list is not in the memory but is the
    memory address

### property insns
  * These insns never generate any corresponding machine insns but can be used to a generate specialized code
    when lazy basic block versioning is used
  * `prset` sets property of the variable given as the 1st operand to integer constant given as the 2nd operand
  * `prbeq` and `prbne` jumps to the label given as the 1st operand if property of the variable given as the
    2nd operand equal or not equal to integer constant given as the 3rd operand

## Stack scopes and calls

Builder stack scopes annotate canonical blocks. Materialization inserts restores
only on edges leaving a scope, adding cleanup blocks where needed. Explicit
`stack_save` / `stack_restore` retain caller order. See the
[stack-scope example](examples/native-stack-scope.cpp).

`call` and `call_indirect` use ordinary call semantics and infer or use a
prototype. `jump_call` / `ret_to` model the distinct `jcall` / `jret` convention;
they do not model ordinary function-pointer calls with arguments and results.
`CallResult` exposes individual result registers. Fixed arguments match the
signature; variadic arguments carry actual scalar or block types. Builder
`start_varargs`, `next_vararg`, `next_vararg_block` and `end_varargs` encapsulate
the special operands of the low-level forms.

## Text and binary formats

`serialization::parse_text` and `read_binary` return independently owned
Contexts through `Result`. `write_text` produces portable text and `write_binary`
emits the MIR protocol, compressed by default, with `BinaryEncoding::Raw`
available. Reader limits and source names belong to `ParseOptions` /
`BinaryOptions`. Noncanonical NaN payloads lack a lossless text spelling and
return a diagnostic; binary preserves floating payloads. Internal SSA, machine
IR and runtime state do not enter the file protocols. `dump` is a separate
diagnostic view, not a round-trip format.

Text directives and instructions are separated by newlines or semicolons;
`#` starts a comment. This is a complete ordinary loop:

```mir
example: module
loop:    func i64, i64:limit
         local i64:count
         mov count, 0
again:   bge done, count, limit
         add count, count, 1
         jmp again
done:    ret count
         endfunc
         export loop
         endmodule
```

The [format utilities](tools/README.md) use the same C++ readers and writers.
The [original MIR files](mir-tests) remain format and execution inputs.

## Optimization and execution

`transforms::optimize` verifies and materializes a clone, runs O0–O3 and returns
an independent `OptimizedProgram` after out-of-SSA. Module/function pass and
analysis managers expose individual stages and fallible observers. Cached
results track owner revisions and dependencies. Source handles remain valid.

`ExecutionSession::create` loads an independent program, resolves imports,
initializes data and selects interpretation, eager JIT, lazy JIT or BBV. Default
JIT uses O2. Original interpretation is available without optimization; optimized
programs can be interpreted explicitly. Runtime addresses and compilation state
belong to the loaded program. Typed `FunctionHandle` calls return `Result` and
retain the loaded program beyond the source Context and session lifetime.
`NativeEntry` is an explicit ABI boundary whose machine signature must be known
by its caller. Windows native calls retain the single-result ABI restriction;
interpreted calls support multiple results.

See [execution examples](README.md#examples), [code-memory services](CUSTOM-ALLOCATORS.md),
[target development](HOW-TO-PORT-MIR.md) and the
[native platform gate](docs/native-acceptance.md).
