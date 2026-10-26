// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The single source of truth for Wolang's token vocabulary. Include this file
// after defining any of the macros below; every macro defaults to expanding to
// nothing, so a client can pick exactly the categories it cares about.
//
//   WOLANG_TOKEN(Name, Spelling)       specials and literals
//   WOLANG_PUNCT(Name, Spelling)       punctuation and operators
//   WOLANG_KEYWORD(Name, Spelling)     reserved keywords
//   WOLANG_CONTEXTUAL(Name, Spelling)  contextual keywords, lexed as
//                                      identifiers and recognized by the
//                                      parser only in declaration position
//   WOLANG_RESERVED(Spelling)          words reserved for future use
//
// Keeping declaration-introducing words such as `state` and `unit` contextual
// is deliberate: gameplay code names variables `state`, `unit` and `machine`
// constantly, and stealing those spellings would be a daily cost.

#ifndef WOLANG_TOKEN
#define WOLANG_TOKEN(Name, Spelling)
#endif
#ifndef WOLANG_PUNCT
#define WOLANG_PUNCT(Name, Spelling) WOLANG_TOKEN(Name, Spelling)
#endif
#ifndef WOLANG_KEYWORD
#define WOLANG_KEYWORD(Name, Spelling) WOLANG_TOKEN(Name, Spelling)
#endif
#ifndef WOLANG_CONTEXTUAL
#define WOLANG_CONTEXTUAL(Name, Spelling)
#endif
#ifndef WOLANG_RESERVED
#define WOLANG_RESERVED(Spelling)
#endif

// ---------------------------------------------------------------- specials --

WOLANG_TOKEN(EndOfFile, "<end of file>")
WOLANG_TOKEN(Unknown, "<unknown>")
WOLANG_TOKEN(Identifier, "<identifier>")
WOLANG_TOKEN(IntLiteral, "<integer literal>")
WOLANG_TOKEN(FloatLiteral, "<float literal>")
WOLANG_TOKEN(StringLiteral, "<string literal>")
WOLANG_TOKEN(CharLiteral, "<character literal>")
WOLANG_TOKEN(DocComment, "<doc comment>")

// ------------------------------------------------------------- punctuation --

WOLANG_PUNCT(LeftParen, "(")
WOLANG_PUNCT(RightParen, ")")
WOLANG_PUNCT(LeftBrace, "{")
WOLANG_PUNCT(RightBrace, "}")
WOLANG_PUNCT(LeftBracket, "[")
WOLANG_PUNCT(RightBracket, "]")
WOLANG_PUNCT(Comma, ",")
WOLANG_PUNCT(Semicolon, ";")
WOLANG_PUNCT(Colon, ":")
WOLANG_PUNCT(Dot, ".")
WOLANG_PUNCT(DotDot, "..")
WOLANG_PUNCT(DotDotEq, "..=")
WOLANG_PUNCT(Arrow, "->")
WOLANG_PUNCT(FatArrow, "=>")
WOLANG_PUNCT(At, "@")
WOLANG_PUNCT(Underscore, "_")
WOLANG_PUNCT(Plus, "+")
WOLANG_PUNCT(PlusEq, "+=")
WOLANG_PUNCT(Minus, "-")
WOLANG_PUNCT(MinusEq, "-=")
WOLANG_PUNCT(Star, "*")
WOLANG_PUNCT(StarEq, "*=")
WOLANG_PUNCT(Slash, "/")
WOLANG_PUNCT(SlashEq, "/=")
WOLANG_PUNCT(Percent, "%")
WOLANG_PUNCT(PercentEq, "%=")
WOLANG_PUNCT(Eq, "=")
WOLANG_PUNCT(EqEq, "==")
WOLANG_PUNCT(Bang, "!")
WOLANG_PUNCT(BangEq, "!=")
WOLANG_PUNCT(Less, "<")
WOLANG_PUNCT(LessEq, "<=")
WOLANG_PUNCT(Greater, ">")
WOLANG_PUNCT(GreaterEq, ">=")
WOLANG_PUNCT(Shl, "<<")
WOLANG_PUNCT(ShlEq, "<<=")
WOLANG_PUNCT(Shr, ">>")
WOLANG_PUNCT(ShrEq, ">>=")
WOLANG_PUNCT(Amp, "&")
WOLANG_PUNCT(AmpEq, "&=")
WOLANG_PUNCT(AmpAmp, "&&")
WOLANG_PUNCT(Pipe, "|")
WOLANG_PUNCT(PipeEq, "|=")
WOLANG_PUNCT(PipePipe, "||")
WOLANG_PUNCT(Caret, "^")
WOLANG_PUNCT(CaretEq, "^=")
WOLANG_PUNCT(Tilde, "~")
WOLANG_PUNCT(Question, "?")
WOLANG_PUNCT(QuestionDot, "?.")
WOLANG_PUNCT(QuestionQuestion, "??")
// The backslash keeps "??=" from being read as a trigraph for "#".
WOLANG_PUNCT(QuestionQuestionEq, "?\?=")

// ---------------------------------------------------------------- keywords --

WOLANG_KEYWORD(KwAbility, "ability")
WOLANG_KEYWORD(KwAfter, "after")
WOLANG_KEYWORD(KwArena, "arena")
WOLANG_KEYWORD(KwAs, "as")
WOLANG_KEYWORD(KwBefore, "before")
WOLANG_KEYWORD(KwBreak, "break")
WOLANG_KEYWORD(KwClass, "class")
WOLANG_KEYWORD(KwComponent, "component")
WOLANG_KEYWORD(KwConst, "const")
WOLANG_KEYWORD(KwComptime, "comptime")
WOLANG_KEYWORD(KwContinue, "continue")
WOLANG_KEYWORD(KwDefer, "defer")
WOLANG_KEYWORD(KwElse, "else")
WOLANG_KEYWORD(KwEnum, "enum")
WOLANG_KEYWORD(KwError, "error")
WOLANG_KEYWORD(KwExtend, "extend")
WOLANG_KEYWORD(KwExtern, "extern")
WOLANG_KEYWORD(KwFalse, "false")
WOLANG_KEYWORD(KwFor, "for")
WOLANG_KEYWORD(KwForeach, "foreach")
WOLANG_KEYWORD(KwFun, "fun")
WOLANG_KEYWORD(KwIf, "if")
WOLANG_KEYWORD(KwIn, "in")
WOLANG_KEYWORD(KwLoop, "loop")
WOLANG_KEYWORD(KwMatch, "match")
WOLANG_KEYWORD(KwMut, "mut")
WOLANG_KEYWORD(KwNeed, "need")
WOLANG_KEYWORD(KwNull, "null")
WOLANG_KEYWORD(KwOn, "on")
WOLANG_KEYWORD(KwOffer, "offer")
WOLANG_KEYWORD(KwOverride, "override")
WOLANG_KEYWORD(KwPtrNull, "ptrnull")
WOLANG_KEYWORD(KwPriv, "priv")
WOLANG_KEYWORD(KwReturn, "return")
WOLANG_KEYWORD(KwSelf, "self")
WOLANG_KEYWORD(KwSignal, "signal")
WOLANG_KEYWORD(KwSpawn, "spawn")
WOLANG_KEYWORD(KwStruct, "struct")
WOLANG_KEYWORD(KwSystem, "system")
WOLANG_KEYWORD(KwTag, "tag")
WOLANG_KEYWORD(KwTask, "task")
WOLANG_KEYWORD(KwTrue, "true")
WOLANG_KEYWORD(KwType, "type")
WOLANG_KEYWORD(KwUnsafe, "unsafe")
WOLANG_KEYWORD(KwUses, "uses")
WOLANG_KEYWORD(KwWait, "wait")
WOLANG_KEYWORD(KwVar, "var")
WOLANG_KEYWORD(KwVirtual, "virtual")
WOLANG_KEYWORD(KwWhere, "where")
WOLANG_KEYWORD(KwWhile, "while")

// ------------------------------------------------------ contextual keywords --

WOLANG_CONTEXTUAL(Always, "always")
// What a type does when a value of it goes away: `deinit { ... }` inside a
// declaration. Contextual because `deinit` is a perfectly good name for a
// function elsewhere, and because the word is only a declaration where a
// member can begin (D64).
WOLANG_CONTEXTUAL(Deinit, "deinit")
WOLANG_CONTEXTUAL(Derived, "derived")
WOLANG_CONTEXTUAL(Dimension, "dimension")
WOLANG_CONTEXTUAL(Effect, "effect")
WOLANG_CONTEXTUAL(Enter, "enter")
WOLANG_CONTEXTUAL(Exit, "exit")
WOLANG_CONTEXTUAL(Machine, "machine")
// Storage layout: `soa [Particle]` keeps one array per field.
WOLANG_CONTEXTUAL(Aos, "aos")
WOLANG_CONTEXTUAL(Soa, "soa")
WOLANG_CONTEXTUAL(State, "state")
WOLANG_CONTEXTUAL(Unit, "unit")
WOLANG_CONTEXTUAL(Weak, "weak")
// Entity query filters: `Query<mut Velocity, with Falling, without Dead>`.
WOLANG_CONTEXTUAL(With, "with")
WOLANG_CONTEXTUAL(Without, "without")

// --------------------------------------------------------- reserved words --

WOLANG_RESERVED("abstract")
WOLANG_RESERVED("actor")
WOLANG_RESERVED("await")
WOLANG_RESERVED("case")
WOLANG_RESERVED("catch")
WOLANG_RESERVED("delete")
WOLANG_RESERVED("do")
WOLANG_RESERVED("final")
WOLANG_RESERVED("fn")
WOLANG_RESERVED("goto")
WOLANG_RESERVED("impl")
WOLANG_RESERVED("import")
WOLANG_RESERVED("interface")
WOLANG_RESERVED("is")
WOLANG_RESERVED("kernel")
WOLANG_RESERVED("lazy")
WOLANG_RESERVED("let")
WOLANG_RESERVED("macro")
WOLANG_RESERVED("module")
WOLANG_RESERVED("namespace")
WOLANG_RESERVED("new")
WOLANG_RESERVED("nil")
WOLANG_RESERVED("operator")
WOLANG_RESERVED("package")
WOLANG_RESERVED("private")
WOLANG_RESERVED("protected")
WOLANG_RESERVED("pub")
WOLANG_RESERVED("public")
WOLANG_RESERVED("region")
WOLANG_RESERVED("rollback")
WOLANG_RESERVED("shader")
WOLANG_RESERVED("sizeof")
WOLANG_RESERVED("static")
WOLANG_RESERVED("super")
WOLANG_RESERVED("switch")
WOLANG_RESERVED("template")
WOLANG_RESERVED("this")
WOLANG_RESERVED("throw")
// The old spelling of `ability`, kept reserved so that a file written
// before the rename says so rather than failing to parse (D104).
WOLANG_RESERVED("trait")
WOLANG_RESERVED("transactional")
WOLANG_RESERVED("try")
WOLANG_RESERVED("typedef")
WOLANG_RESERVED("typeof")
WOLANG_RESERVED("union")
WOLANG_RESERVED("using")
WOLANG_RESERVED("void")
WOLANG_RESERVED("yield")

#undef WOLANG_TOKEN
#undef WOLANG_PUNCT
#undef WOLANG_KEYWORD
#undef WOLANG_CONTEXTUAL
#undef WOLANG_RESERVED
