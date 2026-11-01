// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/fuzz/generator.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wolang {
namespace fuzz {
namespace {

// A generator, not a source of randomness: the point is that a seed names
// a program, forever. xorshift64 rather than anything from the standard
// library, because the library's engines are free to differ between
// implementations and a seed that reproduces nothing is worth nothing.
class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15u
                                                    : seed) {}

  uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  // In [0, bound).
  uint32_t Below(uint32_t bound) {
    return bound == 0 ? 0 : static_cast<uint32_t>(Next() % bound);
  }

  bool Chance(uint32_t percent) { return Below(100) < percent; }

 private:
  uint64_t state_;
};

// The numeric types a value can have (D231). Every width and both
// signednesses, because the bugs found by hand lived in the corners of
// them: 64-bit constant folding, casts that narrow, printing a u64.
enum class Ty {
  kS8,
  kS16,
  kS32,
  kS64,
  kInt,
  kU8,
  kU16,
  kU32,
  kU64,
  kF32,
  kF64,
};

// `int` is its own type, s64 in width (D238): a program must be able to mix
// it with a real s32, and neither may print as the other.
constexpr Ty kAllTypes[] = {Ty::kS8,  Ty::kS16, Ty::kS32, Ty::kS64, Ty::kInt,
                            Ty::kU8,  Ty::kU16, Ty::kU32, Ty::kU64, Ty::kF32,
                            Ty::kF64};
constexpr uint32_t kIntTypeCount = 9;
constexpr Ty kIntTypes[kIntTypeCount] = {
    Ty::kS8, Ty::kS16, Ty::kS32, Ty::kS64, Ty::kInt,
    Ty::kU8, Ty::kU16, Ty::kU32, Ty::kU64};
// The types that get a `Maybe` and a `Check` helper.
constexpr Ty kHelperTypes[] = {Ty::kS8, Ty::kS32, Ty::kS64, Ty::kInt,
                               Ty::kU8, Ty::kU64};

const char* TyName(Ty t) {
  switch (t) {
    case Ty::kS8:
      return "s8";
    case Ty::kS16:
      return "s16";
    case Ty::kS32:
      return "s32";
    case Ty::kS64:
      return "s64";
    case Ty::kInt:
      return "int";
    case Ty::kU8:
      return "u8";
    case Ty::kU16:
      return "u16";
    case Ty::kU32:
      return "u32";
    case Ty::kU64:
      return "u64";
    case Ty::kF32:
      return "float";
    case Ty::kF64:
      return "f64";
  }
  return "int";
}

bool IsFloat(Ty t) { return t == Ty::kF32 || t == Ty::kF64; }

bool IsSigned(Ty t) {
  return t == Ty::kS8 || t == Ty::kS16 || t == Ty::kS32 || t == Ty::kS64 ||
         t == Ty::kInt;
}

int Bits(Ty t) {
  switch (t) {
    case Ty::kS8:
    case Ty::kU8:
      return 8;
    case Ty::kS16:
    case Ty::kU16:
      return 16;
    case Ty::kS32:
    case Ty::kU32:
    case Ty::kF32:
      return 32;
    default:
      return 64;
  }
}

// What a name in scope is.
enum class VKind { kNum, kText, kClass, kOpt, kShape, kSlice };

struct Variable {
  std::string name;
  VKind kind = VKind::kNum;
  Ty ty = Ty::kInt;
  bool is_mutable = false;
};

struct Callable {
  std::string name;
  std::vector<Ty> parameters;
  bool takes_cell = false;
  Ty result = Ty::kInt;
};

struct Loop {
  std::string label;
};

class Writer {
 public:
  Writer(const Options& options, Random* random)
      : options_(options), random_(random) {}

  std::string Run() {
    Prelude();
    for (int i = 0; i < options_.functions; ++i) {
      Function(i);
    }
    Entry();
    return out_;
  }

 private:
  // ----------------------------------------------------------- helpers --

  uint32_t Below(uint32_t n) { return random_->Below(n); }
  bool Chance(uint32_t percent) { return random_->Chance(percent); }

  // Whether this expression may be one that can trap. Rare, because a
  // program that traps at its first operation compares nothing after it;
  // the rest are written so that they cannot (D231).
  bool Wild() { return options_.traps && Chance(6); }

  Ty RandomType() {
    if (!options_.floats) {
      return kIntTypes[Below(kIntTypeCount)];
    }
    // Floats are two of ten but are worth more than a fifth of the
    // arithmetic, since `%` and the casts live there.
    return Chance(25) ? (Chance(50) ? Ty::kF32 : Ty::kF64)
                      : kIntTypes[Below(kIntTypeCount)];
  }

  Ty RandomInt() { return kIntTypes[Below(kIntTypeCount)]; }

  std::string Fresh(const char* stem) {
    return stem + std::to_string(next_name_++);
  }

  void Line(int indent, const std::string& text) {
    for (int i = 0; i < indent; ++i) {
      out_.append("  ");
    }
    out_.append(text);
    out_.push_back('\n');
  }

  // ------------------------------------------------------------ literals --

  // A boundary of an integer type, written so that it is that type's
  // value and not an out-of-range literal: the minimum of a signed type is
  // spelled as a subtraction.
  std::string Boundary(Ty t) {
    const int bits = Bits(t);
    const uint64_t all = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    if (bits >= 32 && Chance(25)) {
      // The edges of the old 32-bit int and of the 64-bit one (D238): a
      // value one step either side of 2^31 and 2^32 must not change width
      // in any tier.
      static const char* const kEdges[] = {
          "2147483647", "2147483648", "4294967295", "4294967296",
          "4294967297", "(-2147483647 - 1)", "(-2147483648 - 1)",
          "(-4294967296)"};
      const uint32_t pick = Below(bits == 32 ? 3 : 8);
      const std::string edge = kEdges[pick];
      if (bits == 32 && pick == 1) {
        return IsSigned(t) ? "2147483646" : edge;
      }
      if (bits == 32 && pick == 2) {
        return IsSigned(t) ? "(-2147483647)" : edge;
      }
      if (!IsSigned(t) && edge[0] == '(') {
        return "4294967295";
      }
      return edge;
    }
    if (IsSigned(t)) {
      const uint64_t max = all >> 1;
      switch (Below(7)) {
        case 0:
          return "0";
        case 1:
          return "1";
        case 2:
          return "(-1)";
        case 3:
          return std::to_string(max);
        case 4:
          return std::to_string(max - 1);
        case 5:
          return "(-" + std::to_string(max) + " - 1)";
        default:
          return "(-" + std::to_string(max) + ")";
      }
    }
    switch (Below(6)) {
      case 0:
        return "0";
      case 1:
        return "1";
      case 2:
        return std::to_string(all);
      case 3:
        return std::to_string(all - 1);
      case 4:
        return std::to_string(all >> 1);
      default:
        return std::to_string((all >> 1) + 1);
    }
  }

  std::string Literal(Ty t) {
    if (IsFloat(t)) {
      static const char* const kFloats[] = {"0.0",  "0.5",   "1.5", "2.5",
                                            "3.5",  "10.5",  "100.25",
                                            "1e30", "(-2.5)", "7.5"};
      return kFloats[Below(10)];
    }
    if (Chance(40)) {
      return Boundary(t);
    }
    if (IsSigned(t)) {
      const int v = static_cast<int>(Below(21)) - 10;
      return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v);
    }
    return std::to_string(Below(21));
  }

  // ------------------------------------------------------------- reading --

  // A copy, not a pointer into the scope: generating the code around a
  // use can declare a variable (a match binds its payload), the scope can
  // grow and move, and a pointer read after that is a pointer into freed
  // memory. Which happened on Windows only, where the operands of `+` are
  // evaluated right to left and the vector grows at a different size.
  std::optional<Variable> PickVariable(VKind kind, bool need_mutable) {
    std::vector<const Variable*> candidates;
    for (const Variable& variable : scope_) {
      if (variable.kind == kind && (!need_mutable || variable.is_mutable)) {
        candidates.push_back(&variable);
      }
    }
    if (candidates.empty()) {
      return std::nullopt;
    }
    return *candidates[Below(static_cast<uint32_t>(candidates.size()))];
  }

  // A variable of type |t|. The preamble declares one of every type, so
  // there always is one.
  std::string Read(Ty t) {
    std::vector<const Variable*> candidates;
    for (const Variable& variable : scope_) {
      if (variable.kind == VKind::kNum && variable.ty == t) {
        candidates.push_back(&variable);
      }
    }
    return candidates[Below(static_cast<uint32_t>(candidates.size()))]->name;
  }

  std::string ClassVar() {
    const std::optional<Variable> cell = PickVariable(VKind::kClass, false);
    return cell->name;
  }

  // ---------------------------------------------------------- expressions --

  // An expression of type |t|, nesting at most |depth| deep. |anchored|
  // means the expression must carry its own type: a bare literal takes the
  // type its context gives it, and a context that has none (an operand of
  // `as`, the subject of a match) would make `300 as u8` a literal that
  // does not fit an int, which is the generator's mistake and not a bug.
  std::string Expression(Ty t, int depth, bool anchored) {
    if (depth <= 0) {
      return Chance(70) || anchored ? Read(t) : Literal(t);
    }
    const uint32_t choice = Below(100);
    std::string made;
    if (choice < 20) {
      return Read(t);
    }
    if (choice < 26) {
      return anchored ? Read(t) : Literal(t);
    }
    if (choice < 33) {
      made = Call(t, depth);
    } else if (choice < 39) {
      made = Conditional(t, depth, anchored);
    } else if (choice < 47) {
      made = Member(t, depth);
    } else if (choice < 56) {
      made = Cast(t, depth);
    } else if (choice < 63 && !IsFloat(t)) {
      made = Bitwise(t, depth);
    } else if (choice < 69) {
      made = Unary(t, depth);
    } else if (choice < 75 && !IsFloat(t)) {
      made = Shift(t, depth);
    } else if (choice < 80) {
      made = IntMatch(t, depth, anchored);
    } else if (choice < 85) {
      made = ShapeMatch(t, depth, anchored);
    } else if (choice < 91) {
      made = Optional(t, depth);
    } else if (choice < 94) {
      made = TextLength(t, depth);
    }
    if (made.empty()) {
      made = Arithmetic(t, depth);
    }
    return made;
  }

  std::string Call(Ty t, int depth) {
    std::vector<const Callable*> candidates;
    for (const Callable& callable : functions_) {
      if (callable.result == t) {
        candidates.push_back(&callable);
      }
    }
    if (candidates.empty()) {
      return "";
    }
    const Callable& callee =
        *candidates[Below(static_cast<uint32_t>(candidates.size()))];
    std::string call = callee.name + "(";
    for (size_t i = 0; i < callee.parameters.size(); ++i) {
      if (i != 0) {
        call.append(", ");
      }
      call.append(Expression(callee.parameters[i], depth - 1, false));
    }
    if (callee.takes_cell) {
      if (!callee.parameters.empty()) {
        call.append(", ");
      }
      call.append(ClassVar());
    }
    call.push_back(')');
    return call;
  }

  // Arms are anchored whatever the context: a match or `if` whose arm is a
  // bare literal is an int, and a peer operand's type does not reach in.
  std::string Conditional(Ty t, int depth, bool) {
    if (!braces_ok_) {
      return "";
    }
    // One draw per statement, in this order: the operands of `+` are
    // evaluated in an order the language leaves open, and clang-cl takes
    // them right to left, so a seed wrote another program there (D231).
    const std::string test = Comparison(depth - 1);
    const std::string yes = Expression(t, depth - 1, true);
    const std::string no = Expression(t, depth - 1, true);
    return "(if " + test + " { " + yes + " } else { " + no + " })";
  }

  // A field, an element or a slice element: storage that something else
  // may have written through another name.
  std::string Member(Ty t, int depth) {
    std::vector<std::string> options;
    if (t == Ty::kInt) {
      options.push_back("pair.a");
      options.push_back(ClassVar() + ".n");
      options.push_back("buf[" + Index(depth) + "]");
      if (const std::optional<Variable> slice = PickVariable(VKind::kSlice, false)) {
        options.push_back(slice->name + "[" + SliceIndex(depth) + "]");
      }
    }
    if (t == Ty::kF32) {
      options.push_back("pair.b");
    }
    if (t == Ty::kU64) {
      options.push_back(ClassVar() + ".big");
    }
    if (t == Ty::kF64) {
      options.push_back(ClassVar() + ".f");
    }
    if (options.empty()) {
      return "";
    }
    return options[Below(static_cast<uint32_t>(options.size()))];
  }

  // An index into `buf`. Kept in range with a mask unless the program is
  // meant to be able to fail, and then it is allowed to be raw.
  std::string Index(int depth) {
    const Ty t = RandomInt();
    if (Wild()) {
      return Expression(t, depth - 1, true);
    }
    return "(" + Expression(t, depth - 1, true) + " & 3)";
  }

  std::string SliceIndex(int depth) {
    const Ty t = RandomInt();
    if (Wild()) {
      return Expression(t, depth - 1, true);
    }
    return "(" + Expression(t, depth - 1, true) + " & 1)";
  }

  std::string Cast(Ty t, int depth) {
    Ty from = RandomType();
    if (from == t) {
      from = t == Ty::kInt ? Ty::kU8 : Ty::kInt;
    }
    const std::string suffix = std::string(" as ") + TyName(t) + ")";
    if (IsFloat(t) || (IsFloat(from) && IsFloat(t))) {
      // A number to a float never traps, from any width.
      return "(" + Expression(from, depth - 1, true) + suffix;
    }
    if (IsFloat(from)) {
      if (Wild()) {
        return "(" + Expression(from, depth - 1, true) + suffix;
      }
      // A float to an integer traps when it does not fit, so the value is
      // pinned to (0, 100) first: that tests truncation without making
      // the cast a coin flip on whether the program ends here.
      const std::string v = Read(from);
      return "((if " + v + " < 100.0 && " + v + " > 0.0 { " + v +
             " } else { 1.5 })" + suffix;
    }
    if (Wild()) {
      return "(" + Expression(from, depth - 1, true) + suffix;
    }
    return "((" + Expression(from, depth - 1, true) + " & 15)" + suffix;
  }

  std::string Bitwise(Ty t, int depth) {
    static const char* const kOps[] = {"&", "|", "^"};
    const std::string left = Expression(t, depth - 1, true);
    const std::string right = Expression(t, depth - 1, false);
    switch (Below(5)) {
      case 3:
        // Subtraction that cannot overflow: x - (x & y) is x & ~y.
        return "(" + left + " - (" + left + " & " + right + "))";
      case 4:
        // And addition: the two halves share no bits.
        return "((" + left + " & " + right + ") + (" + left + " ^ " + right +
               "))";
      default:
        return "(" + left + " " + kOps[Below(3)] + " " + right + ")";
    }
  }

  std::string Unary(Ty t, int depth) {
    if (IsFloat(t)) {
      return "(-" + Expression(t, depth - 1, true) + ")";
    }
    if (!IsSigned(t) || Chance(40)) {
      return "(~" + Expression(t, depth - 1, true) + ")";
    }
    if (Wild()) {
      return "(-" + Expression(t, depth - 1, true) + ")";
    }
    return "(-(" + Expression(t, depth - 1, true) + " & 7))";
  }

  std::string Shift(Ty t, int depth) {
    const char* op = Chance(50) ? "<<" : ">>";
    const std::string left = Expression(t, depth - 1, true);
    if (Wild()) {
      // A count that may be the width or more, which traps.
      return "(" + left + " " + op + " " + Expression(t, depth - 1, false) +
             ")";
    }
    if (Chance(50)) {
      return "(" + left + " " + op + " " +
             std::to_string(Below(static_cast<uint32_t>(Bits(t)))) + ")";
    }
    return "(" + left + " " + op + " (" + Expression(t, depth - 1, true) +
           " & " + std::to_string(Bits(t) - 1) + "))";
  }

  std::string Arithmetic(Ty t, int depth) {
    static const char* const kOps[] = {"+", "-", "*", "/", "%"};
    const uint32_t which = Below(5);
    const std::string left = Expression(t, depth - 1, true);
    const std::string right = Expression(t, depth - 1, false);
    const std::string op = kOps[which];
    if (IsFloat(t) || Wild()) {
      return "(" + left + " " + op + " " + right + ")";
    }
    // Masked so that the result fits the narrowest type: 7 + 7, 7 * 7 and
    // an unsigned 8 + 7 - 7 all do, and a divisor of at least one cannot
    // be zero or -1.
    const std::string l = "(" + left + " & 7)";
    const std::string r = "(" + right + " & 7)";
    switch (which) {
      case 1:
        return IsSigned(t) ? "(" + l + " - " + r + ")"
                           : "((" + l + " + 8) - " + r + ")";
      case 3:
      case 4:
        return "(" + left + " " + op + " (" + r + " + 1))";
      default:
        return "(" + l + " " + op + " " + r + ")";
    }
  }

  std::string IntMatch(Ty t, int depth, bool) {
    if (!braces_ok_) {
      return "";
    }
    const Ty subject = RandomInt();
    std::string text = "(match (" + Expression(subject, depth - 1, true) +
                       " & 3) { ";
    text += "0 => " + Expression(t, depth - 1, true) + ", ";
    text += "1 => " + Expression(t, depth - 1, true) + ", ";
    if (Chance(50)) {
      text += "2 => " + Expression(t, depth - 1, true) + ", ";
    }
    text += "_ => " + Expression(t, depth - 1, true) + " })";
    return text;
  }

  std::string ShapeValue(int depth) {
    switch (Below(5)) {
      case 0:
        return "Shape.Dot";
      case 1:
        return "Shape.Circle(" + Expression(Ty::kInt, depth - 1, false) + ")";
      case 2: {
        const std::string w = Expression(Ty::kS8, depth - 1, false);
        const std::string h = Expression(Ty::kU64, depth - 1, false);
        return "Shape.Box(" + w + ", " + h + ")";
      }
      default:
        return PickVariable(VKind::kShape, false)->name;
    }
  }

  std::string ShapeMatch(Ty t, int depth, bool) {
    if (!braces_ok_) {
      return "";
    }
    const std::string subject = ShapeValue(depth);
    const std::string r = Fresh("r");
    const std::string w = Fresh("w");
    const std::string h = Fresh("h");
    std::string text = "(match " + subject + " { ";
    text += "Shape.Dot => " + Expression(t, depth - 1, true) + ", ";
    const size_t before = scope_.size();
    scope_.push_back(Variable{r, VKind::kNum, Ty::kInt, false});
    text += "Shape.Circle(" + r + ") => " +
            Expression(t, depth - 1, true) + ", ";
    scope_.resize(before);
    scope_.push_back(Variable{w, VKind::kNum, Ty::kS8, false});
    scope_.push_back(Variable{h, VKind::kNum, Ty::kU64, false});
    text += "Shape.Box(" + w + ", " + h + ") => " +
            Expression(t, depth - 1, true) + " })";
    scope_.resize(before);
    return text;
  }

  // `??` over an optional, and a match over a `T!`.
  std::string Optional(Ty t, int depth) {
    if (const std::optional<Variable> opt = PickVariable(VKind::kOpt, false)) {
      if (opt->ty == t && Chance(60)) {
        return "(" + opt->name + " ?? " + Expression(t, depth - 1, false) +
               ")";
      }
    }
    bool has_helper = false;
    for (Ty helper : kHelperTypes) {
      has_helper = has_helper || helper == t;
    }
    if (!has_helper) {
      return "";
    }
    const std::string name = TyName(t);
    if (!braces_ok_ || Chance(50)) {
      const std::string held = Expression(t, depth - 1, false);
      const std::string fallback = Expression(t, depth - 1, false);
      return "(Maybe_" + name + "(" + held + ") ?? " + fallback + ")";
    }
    const std::string v = Fresh("v");
    const std::string checked = Expression(t, depth - 1, false);
    const std::string bad = Expression(t, depth - 1, false);
    const std::string worse = Expression(t, depth - 1, false);
    return "(match Check_" + name + "(" + checked + ") { Ok(" + v + ") => " +
           v + ", Bad => " + bad + ", Worse => " + worse + ", Odd(_c) => " + bad + " })";
  }

  // A number made from a text's length, so that building one is observed.
  std::string TextLength(Ty t, int depth) {
    if (!braces_ok_ || IsFloat(t)) {
      return "";
    }
    if (Chance(40)) {
      return "(" + PickVariable(VKind::kText, false)->name + ".len as " +
             TyName(t) + ")";
    }
    return "((" + Text(depth - 1) + ").len as " + TyName(t) + ")";
  }

  // ---------------------------------------------------------------- text --

  // Always an interpolation or a join of them: a plain literal is a
  // `text` and an interpolated one an owned `Text`, and a variable holds
  // one or the other, so mixing them in assignments is a type error. A
  // text variable is never named here either: an owned text is moved by
  // `+` and by an interpolation of it, and a second read in a loop is
  // rejected. Its length is read instead.
  std::string Text(int depth) {
    if (depth > 0 && Chance(35)) {
      const std::string left = Text(depth - 1);
      const std::string right = Text(depth - 1);
      return "(" + left + " + " + right + ")";
    }
    return Interpolation(depth);
  }

  // Interpolation of a value of any numeric type, printed by the runtime
  // in every tier. The values are kept to reads and members so that no
  // brace or quote ends up inside the braces.
  std::string Interpolation(int depth) {
    const bool saved = braces_ok_;
    braces_ok_ = false;
    std::string text = "\"";
    const uint32_t parts = 1 + Below(2);
    for (uint32_t i = 0; i < parts; ++i) {
      const Ty t = RandomType();
      const char letter = static_cast<char>('a' + Below(26));
      const std::string value = Expression(t, depth > 1 ? 1 : 0, true);
      text += std::string(1, letter) + "{" + value + "}";
    }
    braces_ok_ = saved;
    return text + "\"";
  }

  // ----------------------------------------------------------- conditions --

  std::string Comparison(int depth) {
    if (depth > 0 && Chance(20)) {
      const std::string left = Comparison(depth - 1);
      const char* const join = Chance(50) ? " && " : " || ";
      const std::string right = Comparison(depth - 1);
      return "(" + left + join + right + ")";
    }
    if (depth > 0 && Chance(8)) {
      return "!(" + Comparison(depth - 1) + ")";
    }
    if (braces_ok_ && Chance(8)) {
      const std::string left = Text(1);
      const char* const op = Chance(50) ? " == " : " != ";
      const std::string right = Text(1);
      return left + op + right;
    }
    static const char* const kOps[] = {"<", "<=", ">", ">=", "==", "!="};
    const Ty t = RandomType();
    const std::string left = Expression(t, depth, true);
    const char* const op = kOps[Below(6)];
    const std::string right = Expression(t, depth, false);
    return left + " " + op + " " + right;
  }

  // ---------------------------------------------------------- statements --

  // A conditional jump out of or back to the head of an enclosing loop,
  // by name when the loop has one (D231). Every loop increments its own
  // counter before its body, so a `continue` cannot skip the step that
  // ends the loop.
  void MaybeJump(int indent, int depth) {
    if (loops_.empty() || !Chance(30)) {
      return;
    }
    size_t target = loops_.size() - 1;
    if (loops_.size() > 1 && Chance(50)) {
      target = Below(static_cast<uint32_t>(loops_.size()));
    }
    std::string label = loops_[target].label;
    if (label.empty()) {
      target = loops_.size() - 1;
      label = loops_[target].label;
    }
    const bool innermost = target == loops_.size() - 1;
    const bool named = !label.empty() && (!innermost || Chance(50));
    std::string jump = Chance(50) ? "break" : "continue";
    if (named) {
      jump += " " + label;
    }
    Line(indent, "if " + Comparison(depth) + " { " + jump + " }");
  }

  void Body(int indent, int depth) {
    const size_t before = scope_.size();
    MaybeJump(indent, depth - 1);
    Statement(indent, depth - 1);
    if (Chance(50)) {
      Statement(indent, depth - 1);
    }
    MaybeJump(indent, depth - 1);
    scope_.resize(before);
  }

  void Loops(int indent, int depth) {
    const std::string label = Chance(60) ? Fresh("L") : "";
    const std::string head = label.empty() ? "" : label + ": ";
    const std::string counter = Fresh("i");
    const uint32_t rounds = 1 + Below(4);
    loops_.push_back(Loop{label});
    const size_t before = scope_.size();
    switch (Below(3)) {
      case 0: {
        std::string bound = Chance(30) && !loops_.empty()
                                ? "(" + Expression(Ty::kInt, 1, true) +
                                      " & 3)"
                                : std::to_string(rounds);
        Line(indent, head + "for " + counter + " in 0" +
                         (Chance(30) ? "..=" : "..") + bound + " {");
        scope_.push_back(Variable{counter, VKind::kNum, Ty::kInt, false});
        Body(indent + 1, depth);
        Line(indent, "}");
        break;
      }
      case 1:
        Line(indent, "var " + counter + " = 0");
        Line(indent, head + "while " + counter + " < " +
                         std::to_string(rounds) + " {");
        // The step comes first and the counter is not in scope: a body
        // that could assign it could stop the loop ending, and a hang is
        // a compiled tier with no step limit, not a disagreement.
        Line(indent + 1, counter + " = " + counter + " + 1");
        Body(indent + 1, depth);
        Line(indent, "}");
        break;
      default:
        Line(indent, "var " + counter + " = 0");
        Line(indent, head + "loop {");
        Line(indent + 1, counter + " = " + counter + " + 1");
        Line(indent + 1, "if " + counter + " > " + std::to_string(rounds) +
                             " { break" +
                             (label.empty() ? "" : " " + label) + " }");
        Body(indent + 1, depth);
        Line(indent, "}");
        break;
    }
    scope_.resize(before);
    loops_.pop_back();
  }

  void Statement(int indent, int depth) {
    // Past the depth, only a statement that nests nothing: the point of
    // the depth is that a program which disagrees can be read.
    const uint32_t choice = Below(100);
    const bool nests = depth > 0;

    if (choice < 14) {
      const Ty t = RandomType();
      const std::string name = Fresh("v");
      Line(indent, "var " + name + ": " + TyName(t) + " = " +
                       Expression(t, depth, false));
      scope_.push_back(Variable{name, VKind::kNum, t, true});
      return;
    }
    if (choice < 30) {
      // Assigning to something already there, which is what makes a slot
      // worth promoting and a block parameter worth inserting.
      if (const std::optional<Variable> target = PickVariable(VKind::kNum, true)) {
        Line(indent, target->name + " = " +
                         Expression(target->ty, depth, false));
        return;
      }
    }
    if (choice < 35) {
      const std::optional<Variable> text = PickVariable(VKind::kText, true);
      if (Chance(50) && text.has_value()) {
        Line(indent, text->name + " = " + Text(depth));
      } else {
        const std::string name = Fresh("t");
        Line(indent, "var " + name + " = " + Text(depth));
        scope_.push_back(Variable{name, VKind::kText, Ty::kInt, true});
      }
      return;
    }
    if (choice < 40) {
      OptionalStatement(indent, depth);
      return;
    }
    if (choice < 47) {
      ClassStatement(indent, depth);
      return;
    }
    if (choice < 51) {
      const std::optional<Variable> shape = PickVariable(VKind::kShape, true);
      Line(indent, shape->name + " = " + ShapeValue(depth));
      return;
    }
    if (choice < 56) {
      SliceStatement(indent, depth);
      return;
    }
    if (choice < 60) {
      // Into the buffer, so that a store through an element address is in
      // the mix as well as a read.
      const std::string at = Index(depth);
      const std::string value = Expression(Ty::kInt, depth, false);
      Line(indent, "buf[" + at + "] = " + value);
      return;
    }
    if (choice < 63) {
      if (Chance(50)) {
        Line(indent, "pair.a = " + Expression(Ty::kInt, depth, false));
      } else if (options_.floats) {
        Line(indent, "pair.b = " + Expression(Ty::kF32, depth, false));
      } else {
        Line(indent, "pair.a = " + Expression(Ty::kInt, depth, false));
      }
      return;
    }
    if (choice < 77 && nests) {
      Line(indent, "if " + Comparison(depth - 1) + " {");
      Branch(indent + 1, depth);
      Line(indent, "} else {");
      Branch(indent + 1, depth);
      Line(indent, "}");
      return;
    }
    if (choice < 93 && nests) {
      Loops(indent, depth);
      return;
    }
    Line(indent, "const _ = " + Expression(RandomType(), depth, true));
  }

  void Branch(int indent, int depth) {
    const size_t before = scope_.size();
    Statement(indent, depth - 1);
    scope_.resize(before);
  }

  void OptionalStatement(int indent, int depth) {
    const std::optional<Variable> opt = PickVariable(VKind::kOpt, true);
    if (opt.has_value() && Chance(60)) {
      if (Chance(30)) {
        Line(indent, opt->name + " = null");
      } else if (Chance(50)) {
        const std::string v = Fresh("v");
        Line(indent, "if const " + v + " = " + opt->name + " {");
        const size_t before = scope_.size();
        scope_.push_back(Variable{v, VKind::kNum, opt->ty, false});
        Statement(indent + 1, depth - 1);
        scope_.resize(before);
        Line(indent, "}");
      } else {
        Line(indent, opt->name + " = " + Expression(opt->ty, depth, false));
      }
      return;
    }
    const Ty t = RandomInt();
    const std::string name = Fresh("o");
    Line(indent, "var " + name + ": " + TyName(t) + "? = " +
                     (Chance(30) ? std::string("null")
                                 : Expression(t, depth, false)));
    scope_.push_back(Variable{name, VKind::kOpt, t, true});
  }

  std::string NewCell(int depth) {
    const std::string n = Expression(Ty::kInt, depth, false);
    const std::string big = Expression(Ty::kU64, depth, false);
    const std::string f = Expression(Ty::kF64, depth, false);
    return "Cell(" + n + ", " + big + ", " + f + ")";
  }

  // Allocation, aliasing and writes through either name: the reference
  // counts are what these are for.
  void ClassStatement(int indent, int depth) {
    const std::optional<Variable> target = PickVariable(VKind::kClass, true);
    const uint32_t choice = Below(5);
    if (choice == 0 || !target.has_value()) {
      const std::string name = Fresh("c");
      Line(indent, "var " + name + " = " + NewCell(depth));
      scope_.push_back(Variable{name, VKind::kClass, Ty::kInt, true});
      return;
    }
    if (choice == 1) {
      Line(indent, target->name + " = " + NewCell(depth));
      return;
    }
    if (choice == 2) {
      Line(indent, target->name + " = " + ClassVar());
      return;
    }
    const std::string cell = ClassVar();
    if (choice == 3) {
      Line(indent, cell + ".n = " + Expression(Ty::kInt, depth, false));
    } else if (Chance(50)) {
      Line(indent, cell + ".big = " + Expression(Ty::kU64, depth, false));
    } else {
      Line(indent, cell + ".f = " + Expression(Ty::kF64, depth, false));
    }
  }

  // A view of two elements of `buf`, whose start was computed, and then a
  // write through the view.
  void SliceStatement(int indent, int depth) {
    const std::optional<Variable> slice = PickVariable(VKind::kSlice, false);
    if (slice.has_value() && Chance(40)) {
      const std::string at = SliceIndex(depth);
      const std::string value = Expression(Ty::kInt, depth, false);
      Line(indent, slice->name + "[" + at + "] = " + value);
      return;
    }
    const std::string start = Fresh("k");
    const std::string name = Fresh("s");
    Line(indent, "const " + start + ": int = (" +
                     Expression(RandomInt(), depth, true) + " & 3) as int" +
                     " & 1");
    Line(indent, "var " + name + " = buf[" + start + ".." + start + " + 2]");
    scope_.push_back(Variable{start, VKind::kNum, Ty::kInt, false});
    scope_.push_back(Variable{name, VKind::kSlice, Ty::kInt, false});
  }

  // --------------------------------------------------------- declarations --

  void Prelude() {
    out_.append("extern \"C\" fun wolang_objects_live() -> s64\n\n");
    out_.append("struct Pair {\n  var a: int = 0\n  var b: float = 0.0\n}\n\n");
    out_.append(
        "class Cell {\n  var n: int = 0\n  var big: u64 = 0\n"
        "  var f: f64 = 0.0\n}\n\n");
    out_.append(
        "enum Shape {\n  Dot,\n  Circle(r: int),\n  Box(w: s8, h: u64),\n}\n\n");
    out_.append("error Bad\nerror Worse\nerror Odd(code: int)\n\n");
    for (Ty t : kHelperTypes) {
      const std::string name = TyName(t);
      out_.append("fun Maybe_" + name + "(x: " + name + ") -> " + name +
                  "? {\n  if x > 100 {\n    return null\n  }\n"
                  "  return x\n}\n\n");
      out_.append("fun Check_" + name + "(x: " + name + ") -> " + name +
                  "!(Bad, Worse, Odd) {\n  if x > 100 {\n    return Bad\n  }\n"
                  "  if x < 5 {\n    return Worse\n  }\n"
                  "  if x == 7 {\n    return Odd(1)\n  }\n"
                  "  return x - 5\n}\n\n");
    }
    out_.append(
        "fun ShapeVal(s: Shape) -> int {\n  return match s {\n"
        "    Shape.Dot => 1,\n    Shape.Circle(r) => (r & 127),\n"
        "    Shape.Box(w, h) => (((w & 15) as int) + ((h & 127) as int)),\n"
        "  }\n}\n\n");
  }

  // Every function starts from the same variables, one of each type, so
  // that any expression can read any type.
  void Declarations(int indent) {
    loops_.clear();
    for (Ty t : kAllTypes) {
      // Floats are declared even when none are generated: a class field
      // is an f64 and its constructor reads one.
      const std::string name = std::string("x_") + TyName(t);
      Line(indent, "var " + name + ": " + TyName(t) + " = " + Literal(t));
      scope_.push_back(Variable{name, VKind::kNum, t, true});
    }
    Line(indent, "var buf: [4]int");
    for (int i = 0; i < 4; ++i) {
      Line(indent, "buf[" + std::to_string(i) + "] = " +
                       std::to_string(i + 1));
    }
    Line(indent, "var pair = Pair(5, 1.5)");
    Line(indent, "var tx = \"seed{1}\"");
    scope_.push_back(Variable{"tx", VKind::kText, Ty::kInt, true});
    Line(indent, "var sh0: Shape = Shape.Circle(3)");
    scope_.push_back(Variable{"sh0", VKind::kShape, Ty::kInt, true});
    Line(indent, "var c0 = Cell(1, 2, 0.5)");
    Line(indent, "var c1 = Cell(3, 4, 1.5)");
    Line(indent, "var c2 = c0");
    for (const char* name : {"c0", "c1", "c2"}) {
      scope_.push_back(Variable{name, VKind::kClass, Ty::kInt, true});
    }
  }

  void Function(int index) {
    Callable callable;
    callable.name = "f" + std::to_string(index);
    callable.result = RandomType();
    const uint32_t parameters = Below(3);
    callable.takes_cell = Chance(25);

    scope_.clear();
    std::string header = "fun " + callable.name + "(";
    for (uint32_t i = 0; i < parameters; ++i) {
      const Ty t = RandomType();
      callable.parameters.push_back(t);
      const std::string name = "p" + std::to_string(i);
      if (i != 0) {
        header += ", ";
      }
      header += name + ": " + TyName(t);
      scope_.push_back(Variable{name, VKind::kNum, t, false});
    }
    if (callable.takes_cell) {
      header += std::string(parameters != 0 ? ", " : "") + "pc: Cell";
    }
    header += std::string(") -> ") + TyName(callable.result) + " {";
    Line(0, header);

    Declarations(1);
    if (callable.takes_cell) {
      scope_.push_back(Variable{"pc", VKind::kClass, Ty::kInt, false});
    }
    const int statements =
        1 + static_cast<int>(Below(static_cast<uint32_t>(options_.statements)));
    for (int i = 0; i < statements; ++i) {
      Statement(1, options_.depth);
    }
    Line(1, "return " + Expression(callable.result, options_.depth, false));
    Line(0, "}");
    out_.push_back('\n');

    // Added after the body so a function cannot call itself: recursion
    // with generated arguments does not terminate often enough to be
    // worth the comparison.
    functions_.push_back(std::move(callable));
  }

  // The term a variable adds to the digest: both ends of an integer, so
  // that a wrong high word shows as well as a wrong low one.
  std::string Digest(const Variable& v) {
    const std::string n = v.name;
    const std::string guard_f = "(if " + n + " < 100.0 && " + n +
                                " > -100.0 { " + n + " as int } else { 0 })";
    switch (v.kind) {
      case VKind::kNum: {
        if (IsFloat(v.ty)) {
          return guard_f;
        }
        const int shift = Bits(v.ty) - 8;
        return "((" + n + " & 127) as int) + (((" + n + " >> " +
               std::to_string(shift) + ") & 127) as int)";
      }
      case VKind::kText:
        return "(" + n + ".len as int)";
      case VKind::kClass:
        return "(" + n + ".n & 127) + ((" + n +
               ".big & 127) as int) + (if " + n + ".f < 100.0 && " + n +
               ".f > -100.0 { " + n + ".f as int } else { 0 })";
      case VKind::kOpt:
        return "(((" + n + " ?? 0) & 127) as int)";
      case VKind::kShape:
        return "ShapeVal(" + n + ")";
      case VKind::kSlice:
        return "((" + n + "[0] & 127) + (" + n + "[1] & 127))";
    }
    return "0";
  }

  void Entry() {
    scope_.clear();
    // The body is its own function so that everything it holds is
    // released before the live count is read: a tier that keeps an object
    // alive past its last use, or frees one twice, shows as a different
    // answer here (D231).
    Line(0, "fun Body() -> int {");
    Declarations(1);
    const int statements =
        1 + static_cast<int>(Below(static_cast<uint32_t>(options_.statements)));
    for (int i = 0; i < statements; ++i) {
      Statement(1, options_.depth);
    }
    Line(1, "var acc = 0");
    const auto mix = [&](const std::string& term) {
      Line(1, "acc = (acc % 1000003) * 31 + " + term);
    };
    for (const Variable& variable : std::vector<Variable>(scope_)) {
      mix(Digest(variable));
    }
    mix("(buf[0] & 127) + (buf[1] & 127) + (buf[2] & 127) + (buf[3] & 127)");
    mix("(pair.a & 127)");
    if (options_.floats) {
      mix("(if pair.b < 100.0 && pair.b > -100.0 { pair.b as int } else "
          "{ 0 })");
    }
    mix(Expression(Ty::kInt, options_.depth, false) + " & 127");
    Line(1, "return acc");
    Line(0, "}\n");
    out_.append(
        "fun Live() -> int uses unsafe {\n"
        "  return wolang_objects_live() as int\n}\n\n"
        "fun Probe() -> int uses unsafe {\n"
        "  const before = Live()\n"
        "  const result = Body()\n"
        "  const after = Live()\n"
        "  if after != before {\n"
        "    return 0 - 1000000 - (after - before)\n  }\n"
        "  return result\n}\n");
  }

  const Options& options_;
  Random* random_;
  std::string out_;
  std::vector<Variable> scope_;
  std::vector<Callable> functions_;
  std::vector<Loop> loops_;
  int next_name_ = 0;
  // Off while writing the inside of a string interpolation (and a few
  // other places), where a brace or a quote would end it.
  bool braces_ok_ = true;
};

}  // namespace

std::string Generate(const Options& options) {
  Random random(options.seed);
  Writer writer(options, &random);
  return writer.Run();
}

}  // namespace fuzz
}  // namespace wolang
