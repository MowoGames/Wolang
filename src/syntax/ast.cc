// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/ast.h"

namespace wolang {

std::string_view ItemKindName(ItemKind kind) {
  switch (kind) {
    case ItemKind::kError:
      return "error";
    case ItemKind::kNeed:
      return "need";
    case ItemKind::kFunction:
      return "fun";
    case ItemKind::kClass:
      return "class";
    case ItemKind::kStruct:
      return "struct";
    case ItemKind::kEnum:
      return "enum";
    case ItemKind::kVariant:
      return "variant";
    case ItemKind::kAbility:
      return "ability";
    case ItemKind::kExtend:
      return "extend";
    case ItemKind::kComponent:
      return "component";
    case ItemKind::kTag:
      return "tag";
    case ItemKind::kSystem:
      return "system";
    case ItemKind::kDeinit:
      return "deinit";
    case ItemKind::kSignal:
      return "signal";
    case ItemKind::kOn:
      return "on";
    case ItemKind::kBinding:
      return "binding";
    case ItemKind::kTypeAlias:
      return "type";
    case ItemKind::kDimension:
      return "dimension";
    case ItemKind::kUnit:
      return "unit";
    case ItemKind::kEffect:
      return "effect";
    case ItemKind::kErrorSet:
      return "error";
    case ItemKind::kMachine:
      return "machine";
  }
  return "error";
}

std::string_view ExprKindName(ExprKind kind) {
  switch (kind) {
    case ExprKind::kError:
      return "error";
    case ExprKind::kIntLiteral:
      return "int";
    case ExprKind::kFloatLiteral:
      return "float";
    case ExprKind::kStringLiteral:
      return "string";
    case ExprKind::kStringText:
      return "text";
    case ExprKind::kCharLiteral:
      return "char";
    case ExprKind::kBoolLiteral:
      return "bool";
    case ExprKind::kNullLiteral:
      return "null";
    case ExprKind::kPath:
      return "path";
    case ExprKind::kSelf:
      return "self";
    case ExprKind::kUnary:
      return "unary";
    case ExprKind::kBinary:
      return "binary";
    case ExprKind::kAssign:
      return "assign";
    case ExprKind::kRange:
      return "range";
    case ExprKind::kCast:
      return "cast";
    case ExprKind::kCall:
      return "call";
    case ExprKind::kIndex:
      return "index";
    case ExprKind::kField:
      return "field";
    case ExprKind::kOptionalField:
      return "optional-field";
    case ExprKind::kTry:
      return "try";
    case ExprKind::kAwait:
      return "wait";
    case ExprKind::kSpawn:
      return "spawn";
    case ExprKind::kStructLiteral:
      return "struct-literal";
    case ExprKind::kArrayLiteral:
      return "array";
    case ExprKind::kTupleLiteral:
      return "tuple";
    case ExprKind::kClosure:
      return "closure";
    case ExprKind::kBlock:
      return "block";
    case ExprKind::kIf:
      return "if";
    case ExprKind::kMatch:
      return "match";
    case ExprKind::kLoop:
      return "loop";
    case ExprKind::kWhile:
      return "while";
    case ExprKind::kFor:
      return "for";
    case ExprKind::kForeach:
      return "foreach";
    case ExprKind::kUnsafe:
      return "unsafe";
    case ExprKind::kRegion:
      return "arena";
  }
  return "error";
}

std::string_view StmtKindName(StmtKind kind) {
  switch (kind) {
    case StmtKind::kError:
      return "error";
    case StmtKind::kLet:
      return "const";
    case StmtKind::kVar:
      return "var";
    case StmtKind::kExpr:
      return "expr";
    case StmtKind::kReturn:
      return "return";
    case StmtKind::kBreak:
      return "break";
    case StmtKind::kContinue:
      return "continue";
    case StmtKind::kDefer:
      return "defer";
    case StmtKind::kItem:
      return "item";
  }
  return "error";
}

std::string_view TypeRefKindName(TypeRefKind kind) {
  switch (kind) {
    case TypeRefKind::kError:
      return "error";
    case TypeRefKind::kPath:
      return "type";
    case TypeRefKind::kOptional:
      return "optional";
    case TypeRefKind::kFallible:
      return "fallible";
    case TypeRefKind::kFunction:
      return "fun-type";
    case TypeRefKind::kPointer:
      return "pointer";
    case TypeRefKind::kReference:
      return "reference";
    case TypeRefKind::kTuple:
      return "tuple-type";
    case TypeRefKind::kSlice:
      return "slice";
    case TypeRefKind::kArray:
      return "array-type";
    case TypeRefKind::kMut:
      return "mut";
    case TypeRefKind::kWith:
      return "with";
    case TypeRefKind::kWithout:
      return "without";
  }
  return "error";
}

std::string_view PatternKindName(PatternKind kind) {
  switch (kind) {
    case PatternKind::kError:
      return "error";
    case PatternKind::kWildcard:
      return "wildcard";
    case PatternKind::kBinding:
      return "bind";
    case PatternKind::kLiteral:
      return "literal";
    case PatternKind::kTuple:
      return "tuple-pattern";
    case PatternKind::kVariant:
      return "variant-pattern";
    case PatternKind::kStruct:
      return "struct-pattern";
    case PatternKind::kRest:
      return "rest";
    case PatternKind::kPresent:
      return "present";
  }
  return "error";
}

}  // namespace wolang
