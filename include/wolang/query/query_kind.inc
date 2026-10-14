// Copyright 2026 The Wolang Authors. All rights reserved.
//
// Every fact the compiler caches is a query. Include this file after defining
// either macro; both default to expanding to nothing.
//
//   WOLANG_QUERY_INPUT(Name, Argument, Description)
//       A fact supplied from outside: the compiler never computes it, it is
//       set, and setting it is what starts invalidation.
//   WOLANG_QUERY(Name, Argument, Description)
//       A fact derived from other facts. Recomputed only when one of its
//       dependencies actually changed.
//
// `Argument` documents what the key's 32-bit argument identifies.

#ifndef WOLANG_QUERY_INPUT
#define WOLANG_QUERY_INPUT(Name, Argument, Description)
#endif
#ifndef WOLANG_QUERY
#define WOLANG_QUERY(Name, Argument, Description)
#endif

WOLANG_QUERY_INPUT(SourceText, "file", "the text of a source file")
WOLANG_QUERY_INPUT(ModuleFiles, "module", "the files that make up a module")
WOLANG_QUERY_INPUT(Workspace,
                   "ignored",
                   "the set of modules and the names they are known by")

WOLANG_QUERY(Tokens, "file", "the token stream of a file")
WOLANG_QUERY(ParsedFile, "file", "the syntax tree of a file")
WOLANG_QUERY(ItemTree,
             "file",
             "the declarations of a file with their signatures but not their "
             "bodies")
WOLANG_QUERY(ModuleScope, "module", "the names a module declares")
WOLANG_QUERY(Imports, "file", "the resolved imports of a file")
WOLANG_QUERY(DeclaredNames,
             "file",
             "the names one file declares, without imports")
WOLANG_QUERY(Resolution,
             "file",
             "every name a file can see, imports included")
WOLANG_QUERY(MemberTables,
             "file",
             "the signature of every declaration of a file and the members "
             "of every type it declares")
WOLANG_QUERY(TypeCheck,
             "file",
             "the type of every expression in a file")
WOLANG_QUERY(ModuleEffects,
             "module",
             "the effect row of every declaration in a module, inferred to a "
             "fixpoint")
WOLANG_QUERY(EffectDiagnostics,
             "file",
             "what a file's declarations do against what they declare")
WOLANG_QUERY(ModuleSystems,
             "module",
             "what every system of a module does to the components it "
             "names, and the stages it can run in")
WOLANG_QUERY(SystemDiagnostics,
             "file",
             "the systems of a file that cannot be scheduled against each "
             "other")
WOLANG_QUERY(LoweredFile,
             "file",
             "the IR of a file's declarations, with the verifier's verdict")
WOLANG_QUERY(FileDiagnostics, "file", "every diagnostic reported for a file")

#undef WOLANG_QUERY_INPUT
#undef WOLANG_QUERY
