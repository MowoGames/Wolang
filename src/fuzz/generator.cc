// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/fuzz/generator.h"

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

// What a value is. Only two, because what is being compared is arithmetic
// and control flow rather than the type system.
enum class Kind { kInt, kFloat };

struct Variable {
  std::string name;
  Kind kind = Kind::kInt;
  bool is_mutable = false;
};

struct Callable {
  std::string name;
  std::vector<Kind> parameters;
  Kind result = Kind::kInt;
};

class Writer {
 public:
  Writer(const Options& options, Random* random)
      : options_(options), random_(random) {}

  std::string Run() {
    // One struct and one array per program, so that field and element
    // access are exercised without the generator having to track shapes.
    out_.append("struct Pair {\n  var a: int = 0\n  var b: float = 0.0\n}\n\n");

    for (int i = 0; i < options_.functions; ++i) {
      Function(i);
    }
    Entry();
    return out_;
  }

 private:
  // --------------------------------------------------------- expressions --

  std::string Literal(Kind kind) {
    if (kind == Kind::kInt) {
      return std::to_string(static_cast<int>(random_->Below(21)) - 10);
    }
    // Written with a decimal point so it is a float literal, and kept
    // small so that a sum of them is exact in both widths.
    return std::to_string(static_cast<int>(random_->Below(9)) + 1) + ".5";
  }

  std::string Read(Kind kind) {
    std::vector<const Variable*> candidates;
    for (const Variable& variable : scope_) {
      if (variable.kind == kind) {
        candidates.push_back(&variable);
      }
    }
    if (candidates.empty()) {
      return Literal(kind);
    }
    return candidates[random_->Below(
        static_cast<uint32_t>(candidates.size()))]->name;
  }

  // An expression of |kind|, nesting at most |depth| deep.
  std::string Expression(Kind kind, int depth) {
    if (depth <= 0) {
      return random_->Chance(50) ? Read(kind) : Literal(kind);
    }

    const uint32_t choice = random_->Below(100);
    if (choice < 30) {
      return Read(kind);
    }
    if (choice < 40) {
      return Literal(kind);
    }
    if (choice < 50 && !functions_.empty()) {
      return Call(kind, depth);
    }
    if (choice < 60 && kind == Kind::kInt) {
      // A comparison is a bool, and `if` is where one is wanted; as an
      // integer it has to go through a conditional of its own.
      return "(if " + Comparison(depth - 1) + " { " +
             Expression(kind, depth - 1) + " } else { " +
             Expression(kind, depth - 1) + " })";
    }
    if (choice < 70 && kind == Kind::kInt) {
      return "pair.a";
    }
    if (choice < 75 && kind == Kind::kFloat && options_.floats) {
      return "pair.b";
    }
    if (choice < 85 && kind == Kind::kInt) {
      // An element: the index is kept inside the array's length unless
      // traps are wanted, and then it is not.
      const uint32_t index = options_.traps ? random_->Below(6)
                                            : random_->Below(4);
      return "buf[" + std::to_string(index) + "]";
    }

    const std::string left = Expression(kind, depth - 1);
    const std::string right = Expression(kind, depth - 1);
    const char* operation = "+";
    switch (random_->Below(options_.traps && kind == Kind::kInt ? 5u : 3u)) {
      case 0:
        operation = "+";
        break;
      case 1:
        operation = "-";
        break;
      case 2:
        operation = "*";
        break;
      case 3:
        operation = "/";
        break;
      default:
        operation = "%";
        break;
    }
    return "(" + left + " " + operation + " " + right + ")";
  }

  std::string Comparison(int depth) {
    const Kind kind =
        options_.floats && random_->Chance(30) ? Kind::kFloat : Kind::kInt;
    const char* operation = "<";
    switch (random_->Below(6)) {
      case 0:
        operation = "<";
        break;
      case 1:
        operation = "<=";
        break;
      case 2:
        operation = ">";
        break;
      case 3:
        operation = ">=";
        break;
      case 4:
        operation = "==";
        break;
      default:
        operation = "!=";
        break;
    }
    return Expression(kind, depth) + " " + operation + " " +
           Expression(kind, depth);
  }

  std::string Call(Kind kind, int depth) {
    std::vector<const Callable*> candidates;
    for (const Callable& callable : functions_) {
      if (callable.result == kind) {
        candidates.push_back(&callable);
      }
    }
    if (candidates.empty()) {
      return Literal(kind);
    }
    const Callable& callee = *candidates[random_->Below(
        static_cast<uint32_t>(candidates.size()))];
    std::string call = callee.name + "(";
    for (size_t i = 0; i < callee.parameters.size(); ++i) {
      if (i != 0) {
        call.append(", ");
      }
      call.append(Expression(callee.parameters[i], depth - 1));
    }
    call.push_back(')');
    return call;
  }

  // ---------------------------------------------------------- statements --

  void Indent(int depth) {
    for (int i = 0; i < depth; ++i) {
      out_.append("  ");
    }
  }

  void Statement(int indent, int depth) {
    // Past the depth, only a statement that nests nothing: the point of
    // the depth is that a program which disagrees can be read.
    const uint32_t choice =
        depth <= 0 ? random_->Below(45) : random_->Below(100);

    if (choice < 25) {
      const Kind kind =
          options_.floats && random_->Chance(30) ? Kind::kFloat : Kind::kInt;
      const std::string name = "v" + std::to_string(next_name_++);
      Indent(indent);
      out_.append("var ");
      out_.append(name);
      out_.append(" = ");
      out_.append(Expression(kind, depth));
      out_.push_back('\n');
      scope_.push_back(Variable{name, kind, true});
      return;
    }

    if (choice < 45) {
      // Assigning to something already there, which is what makes a slot
      // worth promoting and a block parameter worth inserting.
      std::vector<const Variable*> targets;
      for (const Variable& variable : scope_) {
        if (variable.is_mutable) {
          targets.push_back(&variable);
        }
      }
      if (!targets.empty()) {
        const Variable& target = *targets[random_->Below(
            static_cast<uint32_t>(targets.size()))];
        Indent(indent);
        out_.append(target.name);
        out_.append(" = ");
        out_.append(Expression(target.kind, depth));
        out_.push_back('\n');
        return;
      }
    }

    if (choice < 60) {
      Indent(indent);
      out_.append("if ");
      out_.append(Comparison(depth - 1));
      out_.append(" {\n");
      const size_t before = scope_.size();
      Statement(indent + 1, depth - 1);
      scope_.resize(before);
      Indent(indent);
      out_.append("} else {\n");
      Statement(indent + 1, depth - 1);
      scope_.resize(before);
      Indent(indent);
      out_.append("}\n");
      return;
    }

    if (choice < 75) {
      // A counted loop, written so that it always terminates: the bound is
      // a constant and the step is one.
      const std::string name = "i" + std::to_string(next_name_++);
      const uint32_t rounds = 1 + random_->Below(5);
      Indent(indent);
      out_.append("var ");
      out_.append(name);
      out_.append(" = 0\n");
      Indent(indent);
      out_.append("while ");
      out_.append(name);
      out_.append(" < ");
      out_.append(std::to_string(rounds));
      out_.append(" {\n");
      const size_t before = scope_.size();
      // The counter is deliberately *not* in scope for the body: a body
      // that may assign to it is a body that may stop the loop ending,
      // and a program that runs forever is not a disagreement -- it is a
      // compiled tier with no step limit, hanging.
      Statement(indent + 1, depth - 1);
      Indent(indent + 1);
      out_.append(name);
      out_.append(" = ");
      out_.append(name);
      out_.append(" + 1\n");
      scope_.resize(before);
      Indent(indent);
      out_.append("}\n");
      scope_.push_back(Variable{name, Kind::kInt, true});
      return;
    }

    if (choice < 85) {
      // Into the buffer, so that a store through an element address is in
      // the mix as well as a read.
      Indent(indent);
      out_.append("buf[");
      out_.append(std::to_string(random_->Below(4)));
      out_.append("] = ");
      out_.append(Expression(Kind::kInt, depth - 1));
      out_.push_back('\n');
      return;
    }

    if (choice < 92) {
      Indent(indent);
      out_.append("pair.a = ");
      out_.append(Expression(Kind::kInt, depth - 1));
      out_.push_back('\n');
      return;
    }

    Indent(indent);
    out_.append("const _ = ");
    out_.append(Expression(Kind::kInt, depth - 1));
    out_.push_back('\n');
  }

  // --------------------------------------------------------- declarations --

  void Preamble(int indent) {
    Indent(indent);
    out_.append("var buf: [4]int\n");
    Indent(indent);
    out_.append("buf[0] = 1\n");
    Indent(indent);
    out_.append("buf[1] = 2\n");
    Indent(indent);
    out_.append("buf[2] = 3\n");
    Indent(indent);
    out_.append("buf[3] = 4\n");
    Indent(indent);
    out_.append("var pair = Pair(5, 1.5)\n");
  }

  void Function(int index) {
    Callable callable;
    callable.name = "f" + std::to_string(index);
    callable.result = options_.floats && random_->Chance(25) ? Kind::kFloat
                                                             : Kind::kInt;
    const uint32_t parameters = random_->Below(3);

    scope_.clear();
    out_.append("fun ");
    out_.append(callable.name);
    out_.push_back('(');
    for (uint32_t i = 0; i < parameters; ++i) {
      const Kind kind =
          options_.floats && random_->Chance(30) ? Kind::kFloat : Kind::kInt;
      callable.parameters.push_back(kind);
      const std::string name = "p" + std::to_string(i);
      if (i != 0) {
        out_.append(", ");
      }
      out_.append(name);
      out_.append(kind == Kind::kInt ? ": int" : ": float");
      scope_.push_back(Variable{name, kind, false});
    }
    out_.append(") -> ");
    out_.append(callable.result == Kind::kInt ? "int" : "float");
    out_.append(" {\n");

    Preamble(1);
    const int statements = 1 + static_cast<int>(
                                   random_->Below(
                                       static_cast<uint32_t>(
                                           options_.statements)));
    for (int i = 0; i < statements; ++i) {
      Statement(1, options_.depth);
    }
    out_.append("  return ");
    out_.append(Expression(callable.result, options_.depth));
    out_.append("\n}\n\n");

    // Added after the body so a function cannot call itself: recursion
    // with generated arguments does not terminate often enough to be
    // worth the comparison.
    functions_.push_back(std::move(callable));
  }

  void Entry() {
    scope_.clear();
    out_.append("fun Probe() -> int {\n");
    Preamble(1);
    const int statements = 1 + static_cast<int>(
                                   random_->Below(
                                       static_cast<uint32_t>(
                                           options_.statements)));
    for (int i = 0; i < statements; ++i) {
      Statement(1, options_.depth);
    }
    out_.append("  return ");
    out_.append(Expression(Kind::kInt, options_.depth));
    out_.append("\n}\n");
  }

  const Options& options_;
  Random* random_;
  std::string out_;
  std::vector<Variable> scope_;
  std::vector<Callable> functions_;
  int next_name_ = 0;
};

}  // namespace

std::string Generate(const Options& options) {
  Random random(options.seed);
  Writer writer(options, &random);
  return writer.Run();
}

}  // namespace fuzz
}  // namespace wolang
