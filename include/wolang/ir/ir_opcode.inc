// Copyright 2026 The Wolang Authors. All rights reserved.
//
// Every IR operation, in one place. Include this file after defining the
// macros; both default to expanding to nothing.
//
//   WOLANG_IR_OP(Name, Mnemonic, Description)
//       An operation that produces a value.
//   WOLANG_IR_TERMINATOR(Name, Mnemonic, Description)
//       An operation that ends a block. Produces no value, and must be the
//       last instruction of its block.

#ifndef WOLANG_IR_OP
#define WOLANG_IR_OP(Name, Mnemonic, Description)
#endif
#ifndef WOLANG_IR_TERMINATOR
#define WOLANG_IR_TERMINATOR(Name, Mnemonic, Description)
#endif

// ----------------------------------------------------------- definitions ---

WOLANG_IR_OP(Param, "param", "a block parameter; the SSA form of a phi node")
WOLANG_IR_OP(ConstInt, "const.int", "an integer constant")
WOLANG_IR_OP(ConstFloat, "const.float", "a floating point constant")
WOLANG_IR_OP(ConstBool, "const.bool", "`true` or `false`")
WOLANG_IR_OP(ConstNull, "const.null", "the absent value of an optional")
WOLANG_IR_OP(ConstString, "const.str", "a string literal, by index")
WOLANG_IR_OP(Undef, "undef", "a value that was never produced; only an "
                             "unreachable path may use one")

// ------------------------------------------------------------- arithmetic ---

WOLANG_IR_OP(Add, "add", "")
WOLANG_IR_OP(Sub, "sub", "")
WOLANG_IR_OP(Mul, "mul", "")
WOLANG_IR_OP(Div, "div", "")
WOLANG_IR_OP(Rem, "rem", "")
WOLANG_IR_OP(Neg, "neg", "")

WOLANG_IR_OP(BitAnd, "and", "")
WOLANG_IR_OP(BitOr, "or", "")
WOLANG_IR_OP(BitXor, "xor", "")
WOLANG_IR_OP(BitNot, "not", "")
WOLANG_IR_OP(Shl, "shl", "")
WOLANG_IR_OP(Shr, "shr", "")

// --------------------------------------------------------------- compares ---

WOLANG_IR_OP(Eq, "eq", "")
WOLANG_IR_OP(Ne, "ne", "")
WOLANG_IR_OP(Lt, "lt", "")
WOLANG_IR_OP(Le, "le", "")
WOLANG_IR_OP(Gt, "gt", "")
WOLANG_IR_OP(Ge, "ge", "")

WOLANG_IR_OP(LogicalNot, "lnot", "negates a bool")

// ----------------------------------------------------------------- memory ---

WOLANG_IR_OP(Alloca, "alloca", "a slot for a mutable local. Lowering puts "
                               "every mutable binding in one; SROA promotes "
                               "the ones whose address never escapes")
WOLANG_IR_OP(FuncAddress, "func.addr",
             "a function as a value. What a callback, a router's handler "
             "and a signal's subscriber are; |callee| says which function")
WOLANG_IR_OP(New, "new", "storage for one object of the type, on the heap. "
                         "What a `class` value is a handle to. The size is "
                         "the tier's own answer, which is why this is an "
                         "operation and not a call lowering writes")
WOLANG_IR_OP(Load, "load", "reads a slot")
WOLANG_IR_OP(Store, "store", "writes a slot; produces no value")

// ------------------------------------------------------------- aggregates ---

WOLANG_IR_OP(MakeStruct, "struct", "builds an aggregate from its fields")
WOLANG_IR_OP(Extract, "extract", "reads field |index| of an aggregate")
WOLANG_IR_OP(Insert, "insert", "a copy of an aggregate with one field "
                               "replaced")
WOLANG_IR_OP(MakeArray, "array", "builds an array from its elements")
WOLANG_IR_OP(Index, "index", "reads element |rhs| of an array or slice")
WOLANG_IR_OP(FieldAddress, "field.addr",
             "the address of field |index| of the aggregate at an address. "
             "What a frame that outlives its function is read through")
WOLANG_IR_OP(SoaNew, "soa.new",
             "storage for |rhs| elements of a record, one array per field. "
             "The size is the tier's own answer, the way `new`'s is")
WOLANG_IR_OP(SoaAddress, "soa.addr",
             "the address of field |index| of element |rhs| in storage "
             "laid out one array per field. What makes `soa []T` a "
             "different *layout* rather than a different type")
WOLANG_IR_OP(MakeVariant, "variant",
             "builds the variant |field| of an enum from the values it "
             "carries: the tag, and the payload beside it")
WOLANG_IR_OP(VariantPayload, "variant.payload",
             "what the variant |field| carries, read out of an enum "
             "value. Undefined when the value is a different variant, "
             "which a `match` is what stops happening")
WOLANG_IR_OP(LocalAddress, "local.addr",
             "where a slot is. `&x` on a whole local: the reference tier "
             "answers with the slot itself, and a tier with real frames "
             "answers with the address of it")
WOLANG_IR_OP(GlobalAddress, "global.addr",
             "where a module-level `var` lives. The runtime owns the "
             "storage, so a global written in one tier is the one read in "
             "the other")
WOLANG_IR_OP(ClassTable, "class.table",
             "the token that says which class an object is. Written into "
             "the object when it is created, and read by every virtual "
             "call on it")
WOLANG_IR_OP(VirtualCall, "call.virtual",
             "calls the body slot |field| names, found through the token "
             "the receiver carries. What `virtual` means")
WOLANG_IR_OP(IndexCheck, "index.check",
             "traps unless |lhs| is below the length |rhs|. What is left "
             "of an element address once the arithmetic around it has been "
             "expanded into ordinary instructions: the part that has to "
             "stay where it is, because it is about this element")
WOLANG_IR_OP(IndexAddress, "index.addr",
             "the address of element |rhs| of the array at an address. The "
             "dynamic counterpart of a field address, and what writing one "
             "element of a buffer goes through rather than copying it")

// -------------------------------------------------------------- optionals ---

WOLANG_IR_OP(OptionalWrap, "opt.wrap", "an optional holding a value")
WOLANG_IR_OP(OptionalIsPresent, "opt.present", "whether an optional holds a "
                                               "value")
WOLANG_IR_OP(OptionalValue, "opt.value", "the value inside; undefined when "
                                         "absent")

// ------------------------------------------------------------------ calls ---

WOLANG_IR_OP(Call, "call", "calls a known function")
WOLANG_IR_OP(SpawnCall, "spawn.call",
             "starts a `task` alongside the caller and produces a handle "
             "for it. Expanded by the transform in `ir/tasks.h` into a "
             "frame and a call to the scheduler")
WOLANG_IR_OP(WaitCall, "wait.call",
             "calls a `task` and suspends until it finishes. Not something "
             "any back end sees: the transform in `ir/tasks.h` splits every "
             "one of these into a state machine before anything runs")
WOLANG_IR_OP(CallIndirect, "call.indirect", "calls through a value")

WOLANG_IR_OP(TypeSize, "type.size",
             "how many bytes a value of the type named by |integer| "
             "takes. A question only a tier can answer, the way `new`'s "
             "size is -- and what making room for a component needs")
// ------------------------------------------------------------ conversions ---

WOLANG_IR_OP(Cast, "cast", "a numeric conversion written with `as`")

// ------------------------------------------------------------ terminators ---

WOLANG_IR_TERMINATOR(Return, "ret", "leaves the function")
WOLANG_IR_TERMINATOR(Jump, "jmp", "goes to one block, passing its parameters")
WOLANG_IR_TERMINATOR(Branch, "br", "goes to one of two blocks on a bool")
WOLANG_IR_TERMINATOR(Switch, "switch", "goes to one of several blocks on an "
                                       "integer, with a default")
WOLANG_IR_TERMINATOR(Unreachable, "unreachable", "control never gets here; "
                                                 "what a diverging path ends "
                                                 "with")

#undef WOLANG_IR_OP
#undef WOLANG_IR_TERMINATOR
