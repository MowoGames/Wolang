// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/database.h"

#include <utility>

#include "wolang/interp/interpreter.h"
#include "wolang/ir/lower.h"
#include "wolang/ir/passes.h"
#include "wolang/ir/tasks.h"
#include "wolang/ir/verifier.h"
#include "wolang/sema/systems.h"
#include "wolang/syntax/lexer.h"
#include "wolang/syntax/parser.h"

namespace wolang {
namespace {

uint64_t HashText(std::string_view text) {
  uint64_t hash = 0xcbf29ce484222325;
  for (char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3;
  }
  return hash;
}

QueryKey SourceTextKey(FileId file) {
  return QueryKey{QueryKind::kSourceText, file.index};
}
QueryKey TokensKey(FileId file) {
  return QueryKey{QueryKind::kTokens, file.index};
}
QueryKey ParsedFileKey(FileId file) {
  return QueryKey{QueryKind::kParsedFile, file.index};
}
QueryKey ItemTreeKey(FileId file) {
  return QueryKey{QueryKind::kItemTree, file.index};
}
QueryKey DiagnosticsKey(FileId file) {
  return QueryKey{QueryKind::kFileDiagnostics, file.index};
}
QueryKey ModuleFilesKey(ModuleId module) {
  return QueryKey{QueryKind::kModuleFiles, module.index};
}
QueryKey ModuleScopeKey(ModuleId module) {
  return QueryKey{QueryKind::kModuleScope, module.index};
}
QueryKey ImportsKey(FileId file) {
  return QueryKey{QueryKind::kImports, file.index};
}
QueryKey WorkspaceKey() {
  return QueryKey{QueryKind::kWorkspace, 0};
}
QueryKey DeclaredNamesKey(FileId file) {
  return QueryKey{QueryKind::kDeclaredNames, file.index};
}
QueryKey ResolutionKey(FileId file) {
  return QueryKey{QueryKind::kResolution, file.index};
}

using ResolvedNames = std::pair<FileResolution, std::vector<Diagnostic>>;
QueryKey LoweredFileKey(FileId file) {
  return QueryKey{QueryKind::kLoweredFile, file.index};
}
QueryKey ModuleEffectsKey(ModuleId module) {
  return QueryKey{QueryKind::kModuleEffects, module.index};
}
QueryKey EffectDiagnosticsKey(FileId file) {
  return QueryKey{QueryKind::kEffectDiagnostics, file.index};
}

constexpr std::string_view kComptimeCallFailed = "E0901";

// A call to a `comptime` function with constant arguments, worked out
// here and left as the constant it is.
//
// The other half of D103: a `comptime fun` with no arguments is evaluated
// to check that it can be, and one *with* arguments is evaluated where it
// is called -- which is this. Done on the IR rather than on the syntax so
// that every tier gets the same constant, and so that the arguments may
// be anything the passes have already worked out to be constant.
//
// Returns how many calls were folded, and reports a trap: a `comptime`
// call that divides by zero is the program telling the compiler something
// is wrong (`E0901`).
uint32_t FoldComptimeCalls(const StringInterner& interner,
                           const TypeStore& types,
                           ir::Module* module,
                           DiagnosticEngine* diagnostics,
                           const Ast* ast,
                           const ItemTree* tree) {
  uint32_t folded = 0;
  for (ir::Function& function : module->functions) {
    for (ir::Block& block : function.blocks) {
      for (const ir::InstId id : block.instructions) {
        ir::Inst& inst = function.Get(id);
        if (inst.opcode != ir::Opcode::kCall ||
            inst.payload.callee >= module->functions.size()) {
          continue;
        }
        const ir::Function& callee = module->functions[inst.payload.callee];
        if (!callee.is_comptime || callee.is_declaration()) {
          continue;
        }
        // Every argument has to be a constant, or there is nothing to
        // evaluate with.
        std::vector<interp::Value> arguments;
        bool constant = true;
        for (const ir::InstId operand : function.OperandsOf(inst)) {
          if (!function.Contains(operand)) {
            constant = false;
            break;
          }
          const ir::Inst& given = function.Get(operand);
          switch (given.opcode) {
            case ir::Opcode::kConstInt:
              arguments.push_back(interp::Value::Int(
                  given.payload.integer, types.Get(given.type).int_width,
                  types.Get(given.type).is_signed));
              break;
            case ir::Opcode::kConstBool:
              arguments.push_back(interp::Value::Bool(given.payload.boolean));
              break;
            case ir::Opcode::kConstFloat:
              arguments.push_back(interp::Value::Float(
                  given.payload.floating,
                  types.Get(given.type).float_width));
              break;
            default:
              constant = false;
              break;
          }
          if (!constant) {
            break;
          }
        }
        if (!constant) {
          continue;
        }

        // A tight budget: this runs during compilation, so a loop that
        // does not finish has to stop being the compiler's problem.
        interp::Limits limits;
        limits.steps = 1'000'000;
        const interp::Outcome outcome = interp::Run(
            interner, types, *module, callee, arguments, limits);
        if (!outcome.ok()) {
          if (diagnostics != nullptr && ast != nullptr && tree != nullptr &&
              callee.has_source()) {
            const ItemId at = ResolveItem(*ast, callee.source_item);
            if (ast->Contains(at)) {
              std::string message = "`";
              message.append(interner.Text(callee.name));
              message.append("` could not be evaluated at compile time: ");
              message.append(interp::TrapKindName(outcome.trap));
              diagnostics->Error(kComptimeCallFailed, std::move(message))
                  .Primary(ast->Get(at).name_span, outcome.message);
            }
          }
          continue;
        }

        // What came back, as the constant it is. Only a number or a bool:
        // anything else would need storage, and a constant with storage is
        // a different feature.
        const interp::Value& value = outcome.value;
        if (value.kind() == interp::Value::Kind::kInt) {
          inst.opcode = ir::Opcode::kConstInt;
          inst.payload.integer = value.integer();
        } else if (value.kind() == interp::Value::Kind::kBool) {
          inst.opcode = ir::Opcode::kConstBool;
          inst.payload.boolean = value.boolean();
        } else if (value.kind() == interp::Value::Kind::kFloat) {
          inst.opcode = ir::Opcode::kConstFloat;
          inst.payload.floating = value.floating();
        } else {
          continue;
        }
        inst.operands_count = 0;
        ++folded;
      }
    }
  }
  return folded;
}

QueryKey ModuleSystemsKey(ModuleId module) {
  return QueryKey{QueryKind::kModuleSystems, module.index};
}
QueryKey SystemDiagnosticsKey(FileId file) {
  return QueryKey{QueryKind::kSystemDiagnostics, file.index};
}
QueryKey MemberTablesKey(FileId file) {
  return QueryKey{QueryKind::kMemberTables, file.index};
}
QueryKey TypeCheckKey(FileId file) {
  return QueryKey{QueryKind::kTypeCheck, file.index};
}

constexpr std::string_view kUnresolvedModule = "E0055";
constexpr std::string_view kUnresolvedName = "E0056";
constexpr std::string_view kDuplicateDeclaration = "E0057";
constexpr std::string_view kPrivateName = "E0058";
constexpr std::string_view kUnusedName = "W0101";
constexpr std::string_view kUndeclaredEffect = "E0701";
constexpr std::string_view kUnstatedPublicEffect = "E0704";
constexpr std::string_view kForbiddenEffect = "E0702";
constexpr std::string_view kUnsafeForbidden = "E0801";
constexpr std::string_view kUngrantedCapability = "E0703";
constexpr std::string_view kComptimeFailed = "E0901";

}  // namespace

const EffectRow* ModuleEffectRows::Find(uint32_t file, uint32_t item) const {
  for (const InferredEffects& entry : rows) {
    if (entry.file == file && entry.item == item) {
      return &entry.row;
    }
  }
  return nullptr;
}

Database::Database() {
  RegisterQueries();
}

Database::~Database() = default;

void Database::RegisterQueries() {
  engine_.Register(
      QueryKind::kTokens, [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        // Asking for the fingerprint is what records the dependency on the
        // file's text; the text itself is read from the source manager.
        db.Get(SourceTextKey(file));

        DiagnosticEngine diagnostics;
        TokenizedFile result;
        result.tokens = Lex(sources_, file, &interner_, &diagnostics);
        result.diagnostics = diagnostics.diagnostics();
        return MakeResult(std::move(result));
      });

  engine_.Register(
      QueryKind::kParsedFile,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const TokenizedFile* tokens = db.GetAs<TokenizedFile>(TokensKey(file));
        if (tokens == nullptr) {
          return MakeResult(ParsedFile{});
        }

        DiagnosticEngine diagnostics;
        ParsedFile result;
        // Qualified: inside this lambda `Parse` would otherwise name the
        // Database member function.
        result.ast = ::wolang::Parse(sources_, tokens->tokens, &interner_,
                                     &diagnostics);
        result.diagnostics = diagnostics.diagnostics();
        return MakeResult(std::move(result));
      });

  // The one query whose value is compared. Everything that only needs the
  // shape of a file's declarations depends on this, so an edit inside a body
  // stops here instead of spreading.
  engine_.Register(
      QueryKind::kItemTree,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        if (parsed == nullptr) {
          return MakeComparableResult(ItemTree{});
        }
        return MakeComparableResult(BuildItemTree(interner_, parsed->ast));
      });

  engine_.Register(
      QueryKind::kModuleScope,
      [](QueryEngine& db, uint32_t arg) -> QueryResult {
        const ModuleId module{arg};
        const auto* files =
            db.GetAs<std::vector<FileId>>(ModuleFilesKey(module));
        if (files == nullptr) {
          return MakeComparableResult(ModuleScope{});
        }

        std::vector<const ItemTree*> trees;
        trees.reserve(files->size());
        for (FileId file : *files) {
          trees.push_back(db.GetAs<ItemTree>(ItemTreeKey(file)));
        }
        return MakeComparableResult(BuildModuleScope(*files, trees));
      });

  engine_.Register(
      QueryKind::kImports,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        // The set of modules is an input: resolving a name depends on it.
        db.Get(WorkspaceKey());

        ResolvedImports result;
        const ModuleId own = ModuleOf(file);
        result.outside_workspace = !own.is_valid();

        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        if (parsed == nullptr || result.outside_workspace) {
          return MakeComparableResult(std::move(result));
        }

        const Ast& ast = parsed->ast;
        uint32_t ordinal = 0;
        for (ItemId id : ast.Get(ast.root())) {
          const Item& item = ast.Get(id);
          if (item.kind != ItemKind::kNeed) {
            continue;
          }

          ResolvedImport resolved;
          resolved.ordinal = ordinal++;
          resolved.module = FindModule(ImportPathText(interner_, ast, item));

          const ModuleScope* scope =
              resolved.module.is_valid()
                  ? db.GetAs<ModuleScope>(ModuleScopeKey(resolved.module))
                  : nullptr;
          for (Symbol name : ast.Get(item.names)) {
            resolved.names.push_back(name);
            if (scope == nullptr) {
              continue;
            }
            const ScopeEntry* binding = scope->Find(name);
            if (binding == nullptr) {
              resolved.missing.push_back(name);
            } else if (!binding->is_public && resolved.module != own) {
              resolved.private_names.push_back(name);
            }
          }
          result.imports.push_back(std::move(resolved));
        }
        return MakeComparableResult(std::move(result));
      });

  // What one file declares, with no knowledge of any other file. Keeping
  // this separate from Resolution is what breaks the cycle two files of one
  // module would otherwise form: each needs the other's names.
  engine_.Register(
      QueryKind::kDeclaredNames,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        if (parsed == nullptr) {
          return MakeComparableResult(ResolvedNames{});
        }

        DiagnosticEngine diagnostics;
        FileResolution resolution = ResolveFile(&types_, &interner_,
                                                parsed->ast, arg,
                                                &diagnostics);
        // Comparable, so editing a body recomputes this but leaves
        // everything downstream of it green.
        return MakeComparableResult(
            ResolvedNames{std::move(resolution), diagnostics.diagnostics()});
      });

  // Everything the file can see: its own declarations, the other files of its
  // module, and whatever its imports selected.
  engine_.Register(
      QueryKind::kResolution,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const auto* own = db.GetAs<ResolvedNames>(DeclaredNamesKey(file));
        if (own == nullptr) {
          return MakeComparableResult(ResolvedNames{});
        }
        ResolvedNames result = *own;

        const auto add = [&result](const Binding& binding, bool public_only) {
          if (public_only && binding.kind == BindingKind::kErrorTag) {
            // Error tags are part of their module's surface, like a type.
          } else if (public_only && !binding.is_public) {
            return;
          }
          // A name the file declares itself wins over an imported one.
          if (result.first.Find(binding.name) != nullptr) {
            return;
          }
          result.first.scope.push_back(binding);
        };

        // A dimension's product travels with its name: `Velocity` is only
        // `Length/Time` because the file that declared it says so.
        const auto add_dimensions = [&result](const FileResolution& other) {
          for (const auto& entry : other.dimension_products) {
            bool known = false;
            for (const auto& mine : result.first.dimension_products) {
              if (mine.first == entry.first) {
                known = true;
                break;
              }
            }
            if (!known) {
              result.first.dimension_products.push_back(entry);
            }
          }
        };

        // Sibling files of the same module share one namespace.
        const ModuleId own_module = ModuleOf(file);
        if (own_module.is_valid()) {
          const Module* module = GetModule(own_module);
          for (FileId sibling : module->files) {
            if (sibling == file) {
              continue;
            }
            const auto* names =
                db.GetAs<ResolvedNames>(DeclaredNamesKey(sibling));
            if (names == nullptr) {
              continue;
            }
            for (const Binding& binding : names->first.scope) {
              add(binding, /*public_only=*/false);
            }
            add_dimensions(names->first);
          }
        }

        const ResolvedImports* imports =
            db.GetAs<ResolvedImports>(ImportsKey(file));
        if (imports == nullptr) {
          return MakeComparableResult(std::move(result));
        }
        for (const ResolvedImport& import : imports->imports) {
          const Module* module = GetModule(import.module);
          if (module == nullptr) {
            continue;
          }
          for (FileId source : module->files) {
            const auto* names =
                db.GetAs<ResolvedNames>(DeclaredNamesKey(source));
            if (names == nullptr) {
              continue;
            }
            for (const Binding& binding : names->first.scope) {
              // An import list selects names; a bare import brings the
              // module's whole public surface.
              if (!import.names.empty()) {
                bool selected = false;
                for (Symbol name : import.names) {
                  if (name == binding.name) {
                    selected = true;
                    break;
                  }
                }
                if (!selected) {
                  continue;
                }
              }
              add(binding, /*public_only=*/true);
            }
            add_dimensions(names->first);
          }
        }
        return MakeComparableResult(std::move(result));
      });

  // Signatures and member tables. Split from Resolution because a field's
  // type may name an imported type -- so this needs the whole scope -- while
  // Resolution must not depend on another file's signatures, or two files of
  // one module would each wait on the other.
  engine_.Register(
      QueryKind::kMemberTables,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        const auto* resolved = db.GetAs<ResolvedNames>(ResolutionKey(file));
        if (parsed == nullptr || resolved == nullptr) {
          return MakeComparableResult(FileResolution{});
        }

        FileResolution result = resolved->first;
        result.types = &types_;
        // Reports nothing: the type checker is the one reporter of an
        // annotation that does not resolve.
        DiagnosticEngine ignored;
        ResolveSignatures(&types_, &interner_, parsed->ast, arg, &result,
                          &ignored);
        return MakeComparableResult(std::move(result));
      });

  engine_.Register(
      QueryKind::kTypeCheck,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        const auto* own = db.GetAs<FileResolution>(MemberTablesKey(file));
        if (parsed == nullptr || own == nullptr) {
          return MakeResult(CheckedFile{});
        }

        // A tree the parser could not finish has error nodes in it, and every
        // type inferred around one is noise. One broken expression should
        // produce one message, not two.
        for (const Diagnostic& diagnostic : parsed->diagnostics) {
          if (diagnostic.severity == Severity::kError) {
            return MakeResult(CheckedFile{});
          }
        }

        // A field access reaches a type declared anywhere this file can see,
        // so the member tables of those files are merged in here rather than
        // in Resolution, which is what keeps module siblings acyclic.
        FileResolution resolution = *own;
        resolution.types = &types_;
        // Nothing merged in: the tables cached for `Box<int>` belong to
        // whichever resolution answered for `Box`, and after a merge there
        // may be more members to substitute.
        resolution.instances.clear();
        const auto merge = [&resolution](const FileResolution& other,
                                        uint32_t other_file) {
          for (const TypeMembers& table : other.type_members) {
            bool known = false;
            for (const TypeMembers& mine : resolution.type_members) {
              if (mine.type == table.type) {
                known = true;
                break;
              }
            }
            if (!known) {
              resolution.type_members.push_back(table);
            }
          }

          // Declared rows travel too: a caller needs to know what calling
          // an imported function does, and that is stated on its signature.
          for (const DeclaredEffects& declared : other.declared_effects) {
            bool known = false;
            for (const DeclaredEffects& mine : resolution.declared_effects) {
              if (mine.file == declared.file && mine.item == declared.item) {
                known = true;
                break;
              }
            }
            if (!known) {
              resolution.declared_effects.push_back(declared);
            }
          }
          for (const auto& effect : other.effect_names) {
            bool known = false;
            for (const auto& mine : resolution.effect_names) {
              if (mine.second == effect.second) {
                known = true;
                break;
              }
            }
            if (!known) {
              resolution.effect_names.push_back(effect);
            }
          }

          // Relations travel with the tables: a bound of `Actor` is only
          // satisfiable because the declaring file says so, and an ability may
          // be implemented in a third file.
          for (const TypeRelations& relation : other.relations) {
            TypeRelations* mine = nullptr;
            for (TypeRelations& candidate : resolution.relations) {
              if (candidate.type == relation.type) {
                mine = &candidate;
                break;
              }
            }
            if (mine == nullptr) {
              resolution.relations.push_back(relation);
              continue;
            }
            if (!mine->base.is_valid()) {
              mine->base = relation.base;
            }
            for (TypeId ability : relation.abilities) {
              bool known = false;
              for (TypeId existing : mine->abilities) {
                if (existing == ability) {
                  known = true;
                  break;
                }
              }
              if (!known) {
                mine->abilities.push_back(ability);
              }
            }
          }

          // A name imported from |other_file| gets its signature from that
          // file's own pass, because an item index only means anything
          // within the file it came from.
          for (Binding& binding : resolution.scope) {
            if (binding.file != other_file || binding.type.is_valid() ||
                binding.item == Binding::kNoItem ||
                binding.item >= other.item_types.size()) {
              continue;
            }
            binding.type = other.item_types[binding.item];
          }
        };

        const ModuleId own_module = ModuleOf(file);
        if (own_module.is_valid()) {
          for (FileId sibling : GetModule(own_module)->files) {
            if (sibling == file) {
              continue;
            }
            if (const auto* other =
                    db.GetAs<FileResolution>(MemberTablesKey(sibling));
                other != nullptr) {
              merge(*other, sibling.index);
            }
          }
        }
        if (const auto* imports = db.GetAs<ResolvedImports>(ImportsKey(file));
            imports != nullptr) {
          for (const ResolvedImport& import : imports->imports) {
            const Module* module = GetModule(import.module);
            if (module == nullptr) {
              continue;
            }
            for (FileId source : module->files) {
              if (const auto* other =
                      db.GetAs<FileResolution>(MemberTablesKey(source));
                  other != nullptr) {
                merge(*other, source.index);
              }
            }
          }
        }

        CheckedFile result;
        const Ast& ast = parsed->ast;
        // Walked through the item tree rather than the root list, so that
        // each body carries the index a caller elsewhere will name it by.
        const ItemTree* tree = db.GetAs<ItemTree>(ItemTreeKey(file));
        const size_t entry_count = tree == nullptr ? 0 : tree->entries.size();

        // A type parameter is interned against the declaration that owns it,
        // so the checker has to name that declaration the same way the
        // signature pass did. Built once per file rather than per item.
        ItemIndexMap item_indices(ast.item_count(), 0xFFFFFFFFu);
        for (uint32_t index = 0; index < entry_count; ++index) {
          const ItemId id = ResolveItem(ast, index);
          if (id.is_valid() && id.index < item_indices.size()) {
            item_indices[id.index] = index;
          }
        }

        for (uint32_t index = 0; index < entry_count; ++index) {
          if (tree->entries[index].parent != ItemTreeEntry::kNoParent) {
            continue;
          }
          const ItemId id = ResolveItem(ast, index);
          if (!ast.Contains(id)) {
            continue;
          }
          TypedBody body = TypeCheckItem(&types_, &interner_, &resolution,
                                         ast, id, index, &item_indices);
          result.diagnostics.insert(result.diagnostics.end(),
                                    body.diagnostics.begin(),
                                    body.diagnostics.end());
          // The per-item tables cover the same expression array, so they are
          // merged into one side table for the file.
          if (result.body.expr_types.size() < body.expr_types.size()) {
            result.body.expr_types.resize(body.expr_types.size(), TypeId{});
          }
          for (size_t i = 0; i < body.expr_types.size(); ++i) {
            if (body.expr_types[i].is_valid()) {
              result.body.expr_types[i] = body.expr_types[i];
            }
          }
          result.body.used_names.insert(result.body.used_names.end(),
                                        body.used_names.begin(),
                                        body.used_names.end());
          result.body.instantiations.insert(
              result.body.instantiations.end(), body.instantiations.begin(),
              body.instantiations.end());
          // Which uses were borrows rather than moves. Lowering reads
          // this to decide whether a scope exit still owns what a name
          // held, and it was the one list the merge left behind -- which
          // made every owned string handed to a `text` parameter look
          // like a move (D15b, D133).
          result.body.borrows.insert(result.body.borrows.end(),
                                     body.borrows.begin(),
                                     body.borrows.end());
          result.items.push_back(std::move(body));
        }
        result.resolution = resolution;

        // A `need` nothing refers to is dead weight, and the check belongs
        // here rather than with import resolution: only type checking knows
        // what the file actually named.
        const auto is_used = [&result](Symbol name) {
          for (Symbol used : result.body.used_names) {
            if (used == name) {
              return true;
            }
          }
          return false;
        };
        for (ItemId id : ast.Get(ast.root())) {
          const Item& item = ast.Get(id);
          if (item.kind != ItemKind::kNeed) {
            continue;
          }
          // A bare `need a.b` brings in a whole module, so there is no one
          // name to call unused.
          if (!HasFlag(item.flags, ItemFlags::kNeedList)) {
            continue;
          }
          for (Symbol name : ast.Get(item.names)) {
            if (is_used(name)) {
              continue;
            }
            std::string message = "`";
            message.append(interner_.Text(name));
            message.append("` is needed but never used");
            DiagnosticEngine unused;
            unused.Warning(kUnusedName, std::move(message))
                .Primary(item.span, "remove it from the `need` list");
            for (const Diagnostic& diagnostic : unused.diagnostics()) {
              result.diagnostics.push_back(diagnostic);
            }
          }
        }
        return MakeResult(std::move(result));
      });

  // One query per module, not per declaration: a set of mutually recursive
  // functions is a loop inside this computation rather than a cycle between
  // queries, which is what keeps the answer independent of which file was
  // asked about first.
  engine_.Register(
      QueryKind::kModuleEffects,
      [](QueryEngine& db, uint32_t arg) -> QueryResult {
        const ModuleId module{arg};
        const auto* files =
            db.GetAs<std::vector<FileId>>(ModuleFilesKey(module));
        if (files == nullptr) {
          return MakeComparableResult(ModuleEffectRows{});
        }

        // Seed every declaration with what its own body does, and remember
        // who it calls.
        struct Node {
          uint32_t file = 0;
          uint32_t item = 0;
          EffectRow row;
          std::vector<std::pair<uint32_t, uint32_t>> callees;
          std::vector<std::pair<uint32_t, uint32_t>> spawned;
        };
        std::vector<Node> nodes;
        for (FileId file : *files) {
          const auto* checked = db.GetAs<CheckedFile>(TypeCheckKey(file));
          if (checked == nullptr) {
            continue;
          }
          for (const TypedBody& body : checked->items) {
            if (body.item_index == 0xFFFFFFFFu) {
              continue;
            }
            nodes.push_back(Node{file.index, body.item_index,
                                 body.direct_effects, body.callees,
                                 body.spawned_callees});
          }
        }

        // Union along call edges until nothing grows. Bounded by the number
        // of declarations times the number of effects, and in practice two
        // or three passes.
        bool grew = true;
        for (size_t pass = 0; grew && pass <= nodes.size(); ++pass) {
          grew = false;
          for (Node& node : nodes) {
            for (const auto& [file, item] : node.callees) {
              for (const Node& other : nodes) {
                if (other.file != file || other.item != item) {
                  continue;
                }
                const bool spawned =
                    std::find(node.spawned.begin(), node.spawned.end(),
                              std::pair<uint32_t, uint32_t>(file, item)) !=
                    node.spawned.end();
                if (!spawned) {
                  grew |= node.row.Merge(other.row);
                  break;
                }
                // Everything the spawned task does carries over except the
                // suspension itself: starting work is being responsible
                // for it, but the spawner does not wait.
                for (EffectId effect : other.row.effects()) {
                  if (effect.index !=
                      static_cast<uint32_t>(BuiltinEffect::kAsync)) {
                    grew |= node.row.Add(effect);
                  }
                }
                break;
              }
            }
          }
        }

        ModuleEffectRows result;
        for (Node& node : nodes) {
          result.rows.push_back(
              InferredEffects{node.file, node.item, std::move(node.row)});
        }
        return MakeComparableResult(std::move(result));
      });

  // Reporting is its own query so that type checking -- which collects the
  // What every system of a module does to the components it names, and
  // the stages those accesses allow. A module question rather than a file
  // one: two systems that conflict may be written in different files, and
  // nothing in one file can see that.
  engine_.Register(
      QueryKind::kModuleSystems,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const ModuleId module{arg};
        const auto* files =
            db.GetAs<std::vector<FileId>>(ModuleFilesKey(module));
        if (files == nullptr) {
          return MakeComparableResult(AccessGraph{});
        }
        AccessGraph graph;
        for (FileId file : *files) {
          if (const auto* parsed =
                  db.GetAs<ParsedFile>(ParsedFileKey(file))) {
            CollectSystems(interner_, parsed->ast, file.index, &graph);
          }
        }
        // Reported through the per-file query below, so that a diagnostic
        // lands on the file that has to change.
        ScheduleSystems(interner_, &graph, nullptr);
        return MakeComparableResult(std::move(graph));
      });

  // The schedule again, this time for its complaints. Split from the graph
  // itself for the reason every other diagnostics query is: a file that
  // asks what its systems conflict with should not be rebuilt because
  // another file's stage numbering moved.
  engine_.Register(
      QueryKind::kSystemDiagnostics,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        std::vector<Diagnostic> result;
        const ModuleId module = ModuleOf(file);
        // A file compiled on its own belongs to no module, and its systems
        // are still worth scheduling against each other: that is what
        // trying something in one file looks like.
        const std::vector<FileId> alone{file};
        const std::vector<FileId>* files = &alone;
        if (module.is_valid()) {
          files = db.GetAs<std::vector<FileId>>(ModuleFilesKey(module));
        }
        if (files == nullptr) {
          return MakeComparableResult(std::move(result));
        }
        AccessGraph graph;
        for (FileId other : *files) {
          if (const auto* parsed =
                  db.GetAs<ParsedFile>(ParsedFileKey(other))) {
            CollectSystems(interner_, parsed->ast, other.index, &graph);
          }
        }
        DiagnosticEngine diagnostics;
        ScheduleSystems(interner_, &graph, &diagnostics);
        for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
          if (diagnostic.PrimarySpan().file == file) {
            result.push_back(diagnostic);
          }
        }
        return MakeComparableResult(std::move(result));
      });

  // rows -- does not depend on the module-wide answer it feeds.
  engine_.Register(
      QueryKind::kEffectDiagnostics,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        std::vector<Diagnostic> result;

        const ModuleId module = ModuleOf(file);
        if (!module.is_valid()) {
          // A file outside a package has no module to infer across, exactly
          // as it resolves no imports.
          return MakeComparableResult(std::move(result));
        }
        const auto* rows =
            db.GetAs<ModuleEffectRows>(ModuleEffectsKey(module));
        const auto* resolved = db.GetAs<FileResolution>(MemberTablesKey(file));
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        const ItemTree* tree = db.GetAs<ItemTree>(ItemTreeKey(file));
        if (rows == nullptr || resolved == nullptr || parsed == nullptr ||
            tree == nullptr) {
          return MakeComparableResult(std::move(result));
        }

        // `[build] unsafe = "deny"` is a package-wide ban, and `unsafe` is
        // an effect, so the ban is one more row the module may not have.
        const Manifest* manifest = ManifestFor(file);
        const bool unsafe_denied =
            manifest != nullptr && !manifest->allow_unsafe;

        // A sandboxed package is granted a capability set and nothing more.
        // An unlisted effect is not a mistake in the code -- it is a
        // permission the package was not given.
        EffectRow granted;
        const bool sandboxed = manifest != nullptr &&
                               manifest->has_capabilities;
        if (sandboxed) {
          for (const std::string& name : manifest->capabilities) {
            const BuiltinEffect builtin = LookupBuiltinEffect(name);
            if (builtin != BuiltinEffect::kCount) {
              granted.Add(types_.BuiltinEffectId(builtin));
              continue;
            }
            const Symbol symbol = interner_.Lookup(name);
            if (symbol.is_valid()) {
              granted.Add(resolved->FindEffect(symbol, interner_));
            }
          }
        }

        const Ast& ast = parsed->ast;
        DiagnosticEngine diagnostics;

        // A context that forbids effects: the row a declaration may not
        // have, whatever it declares. One mechanism covers determinism,
        // allocation-free hot paths and `comptime` purity, which is the
        // whole point of having effects rather than three attribute checks.
        const auto forbidden_by = [this, &ast](const Item& item) {
          EffectRow row;
          const auto add = [&row](BuiltinEffect effect) {
            row.Add(EffectId{static_cast<uint32_t>(effect)});
          };

          if (HasFlag(item.flags, ItemFlags::kComptime)) {
            // A `comptime` function runs inside the compiler, where none of
            // these exist.
            add(BuiltinEffect::kAlloc);
            add(BuiltinEffect::kIo);
            add(BuiltinEffect::kRand);
            add(BuiltinEffect::kTime);
            add(BuiltinEffect::kNet);
            add(BuiltinEffect::kGpu);
            add(BuiltinEffect::kUnsafe);
            // There is nothing to suspend into at compile time.
            add(BuiltinEffect::kAsync);
          }

          for (AttributeId id : ast.Get(item.attributes)) {
            const std::string_view name =
                interner_.Text(ast.Get(id).name);
            if (name == "Deterministic") {
              // The same input has to give the same output, on every machine
              // and every run.
              add(BuiltinEffect::kRand);
              add(BuiltinEffect::kTime);
              add(BuiltinEffect::kNet);
            } else if (name == "Hot") {
              add(BuiltinEffect::kAlloc);
              // A hot path does not suspend: a frame that yields is a frame
              // that missed. This is what putting `async` in the row buys --
              // the check is the same one that catches an allocation.
              add(BuiltinEffect::kAsync);
            }
          }
          return row;
        };

        for (uint32_t index = 0; index < tree->entries.size(); ++index) {
          if (tree->entries[index].parent != ItemTreeEntry::kNoParent) {
            continue;
          }
          const EffectRow* actual = rows->Find(arg, index);
          if (actual != nullptr && unsafe_denied &&
              actual->Contains(BuiltinEffect::kUnsafe)) {
            const ItemId id = ResolveItem(ast, index);
            if (ast.Contains(id)) {
              std::string message = "`";
              message.append(interner_.Text(ast.Get(id).name));
              message.append("` uses `unsafe`, which this package forbids");
              diagnostics.Error(kUnsafeForbidden, std::move(message))
                  .Primary(ast.Get(id).name_span,
                           "the manifest sets `unsafe = \"deny\"`");
            }
          }
          if (actual == nullptr) {
            continue;
          }
          // A forbidding context outranks whatever the signature says: a
          // `@Deterministic` function may not reach `rand` even if it
          // declares it.
          const ItemId declaration = ResolveItem(ast, index);
          if (actual != nullptr && ast.Contains(declaration)) {
            const EffectRow forbidden = forbidden_by(ast.Get(declaration));
            EffectRow offending;
            for (EffectId effect : actual->effects()) {
              if (forbidden.Contains(effect)) {
                offending.Add(effect);
              }
            }
            if (!offending.empty()) {
              std::string message = "`";
              message.append(interner_.Text(ast.Get(declaration).name));
              message.append("` does `");
              message.append(types_.Describe(interner_, offending));
              message.append("`, which is not allowed here");
              diagnostics.Error(kForbiddenEffect, std::move(message))
                  .Primary(ast.Get(declaration).name_span,
                           "this context forbids it");
            }
          }

          if (actual != nullptr && sandboxed && ast.Contains(declaration)) {
            // Declared as well as inferred: `fun Roll() uses rand {}` claims
            // a permission the package does not have, even with an empty
            // body today.
            EffectRow claimed = *actual;
            if (const EffectRow* stated = resolved->DeclaredRow(arg, index);
                stated != nullptr) {
              claimed.Merge(*stated);
            }
            const EffectRow ungranted = granted.Missing(claimed);
            if (!ungranted.empty()) {
              std::string message = "`";
              message.append(interner_.Text(ast.Get(declaration).name));
              message.append("` needs `");
              message.append(types_.Describe(interner_, ungranted));
              message.append("`, which this package was not granted");
              diagnostics.Error(kUngrantedCapability, std::move(message))
                  .Primary(ast.Get(declaration).name_span,
                           "the manifest lists what it may use");
            }
          }

          const EffectRow* declared = resolved->DeclaredRow(arg, index);

          // `task` is part of the signature: it says the function may
          // suspend, so `async` counts as declared whether or not there is
          // a `uses` clause to put it in.
          EffectRow stated;
          if (declared != nullptr) {
            stated = *declared;
          }
          const bool is_task =
              HasFlag(tree->entries[index].flags, ItemFlags::kAsync);
          if (is_task) {
            stated.Add(EffectId{
                static_cast<uint32_t>(BuiltinEffect::kAsync)});
            declared = &stated;
          }

          if (declared == nullptr) {
            // Inference stops at the module boundary, so a name other
            // modules can call has to say what it does -- otherwise its
            // callers have nothing to check against.
            if (actual->empty() ||
                !HasFlag(tree->entries[index].flags, ItemFlags::kOffer) ||
                tree->entries[index].kind != ItemKind::kFunction) {
              continue;
            }
            const ItemId offered = ResolveItem(ast, index);
            if (!ast.Contains(offered)) {
              continue;
            }
            std::string message = "`";
            message.append(interner_.Text(ast.Get(offered).name));
            message.append("` is offered to other modules and does `");
            message.append(types_.Describe(interner_, *actual));
            message.append("`, so it has to say so");
            diagnostics.Error(kUnstatedPublicEffect, std::move(message))
                .Primary(ast.Get(offered).name_span,
                         "add `uses ...` to the signature");
            continue;
          }

          const EffectRow missing = declared->Missing(*actual);
          if (missing.empty()) {
            continue;
          }
          const ItemId id = ResolveItem(ast, index);
          if (!ast.Contains(id)) {
            continue;
          }

          std::string message = "`";
          message.append(interner_.Text(ast.Get(id).name));
          message.append("` does `");
          message.append(types_.Describe(interner_, missing));
          message.append("`, which it does not declare");
          diagnostics.Error(kUndeclaredEffect, std::move(message))
              .Primary(ast.Get(id).name_span,
                       "add it to the `uses` list");
        }
        for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
          result.push_back(diagnostic);
        }
        return MakeComparableResult(std::move(result));
      });

  // Lowering and verification are one query: IR that does not verify is a
  // compiler bug, and the only thing to do with it is report it next to the
  // IR that produced it.
  engine_.Register(
      QueryKind::kLoweredFile,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        const CheckedFile* checked =
            db.GetAs<CheckedFile>(TypeCheckKey(file));
        if (parsed == nullptr || checked == nullptr) {
          return MakeResult(LoweredFile{});
        }

        // Lowering trusts the checker, so a file the checker complained
        // about is not lowered at all. Every value would otherwise have to
        // carry a "maybe" the back ends would inherit.
        for (const Diagnostic& diagnostic : checked->diagnostics) {
          if (diagnostic.severity == Severity::kError) {
            return MakeResult(LoweredFile{});
          }
        }
        for (const Diagnostic& diagnostic : parsed->diagnostics) {
          if (diagnostic.severity == Severity::kError) {
            return MakeResult(LoweredFile{});
          }
        }

        // What each declaration does, by syntax item. Effects are inferred
        // per module (D36) and lowering is per file, so the rows are
        // prepared here and handed over rather than asked for there.
        std::vector<std::pair<ItemId, EffectRow>> effects;
        const ModuleId module = ModuleOf(file);
        if (module.is_valid()) {
          if (const auto* rows =
                  db.GetAs<ModuleEffectRows>(ModuleEffectsKey(module))) {
            for (const InferredEffects& entry : rows->rows) {
              if (entry.file != file.index) {
                continue;
              }
              const ItemId item = ResolveItem(parsed->ast, entry.item);
              if (parsed->ast.Contains(item)) {
                effects.emplace_back(item, entry.row);
              }
            }
          }
        }

        // Where a generic from another module comes from. Asked for
        // through the engine, so that lowering this file depends on the
        // files whose generics it instantiates: change the body of a
        // generic in `std` and every module that used it is rebuilt.
        const auto foreign = [&db](uint32_t index) -> ir::ForeignFile {
          const FileId other{index};
          const auto* other_parsed =
              db.GetAs<ParsedFile>(ParsedFileKey(other));
          const auto* other_checked =
              db.GetAs<CheckedFile>(TypeCheckKey(other));
          if (other_parsed == nullptr || other_checked == nullptr) {
            return ir::ForeignFile{};
          }
          return ir::ForeignFile{&other_checked->resolution,
                                 &other_parsed->ast, &other_checked->body};
        };

        // The schedule, when this file's module has systems in it: which
        // of them may run at the same time is a module question, so it is
        // prepared here the way the effect rows are (D101).
        const AccessGraph* schedule =
            module.is_valid() ? db.GetAs<AccessGraph>(ModuleSystemsKey(module))
                              : nullptr;

        LoweredFile result;
        ir::LowerFile(&interner_, &types_, file.index, checked->resolution,
                      parsed->ast, checked->body, effects, foreign,
                      schedule, &result.module);

        // A `task` is not a function the way the machine means it, so it is
        // split into a state machine here -- before the verifier, so the
        // result is checked, and before the passes, so they see ordinary
        // code. Nothing below this point knows what a coroutine is.
        abi::LayoutStore layouts(types_, checked->resolution);
        const auto span_of = [parsed](uint32_t item) {
          const ItemId id = ResolveItem(parsed->ast, item);
          return parsed->ast.Contains(id) ? parsed->ast.Get(id).name_span
                                          : SourceSpan();
        };
        const size_t before_split = result.diagnostics.size();
        ir::SplitTasks(&interner_, &types_, &layouts, &result.module,
                       span_of, &result.diagnostics);
        if (result.diagnostics.size() != before_split) {
          // The same rule as everywhere else: a file with an error is not
          // lowered. A machine built around a frame that cannot exist is
          // worse than no machine.
          LoweredFile empty;
          empty.diagnostics = std::move(result.diagnostics);
          return MakeResult(std::move(empty));
        }

        // Verified on every input rather than on request: the IR is the
        // contract between the front end and every back end, so a violation
        // has to surface where it was introduced. Verified twice, because a
        // pass that breaks the invariants and a lowering that breaks them
        // are different bugs and have to be distinguishable.
        const ir::VerificationResult lowered =
            ir::Verify(interner_, types_, result.module);
        result.problems = lowered.problems;
        for (std::string& problem : result.problems) {
          problem.insert(0, "lowering: ");
        }

        // `@Test` is read from the tree rather than carried through the
        // IR: a test is a declaration the driver runs, not a property of
        // the code it generates.
        const ItemTree* tree = db.GetAs<ItemTree>(ItemTreeKey(file));
        for (uint32_t index = 0; tree != nullptr &&
                                 index < tree->entries.size();
             ++index) {
          if (tree->entries[index].kind != ItemKind::kFunction) {
            continue;
          }
          const ItemId id = ResolveItem(parsed->ast, index);
          if (!parsed->ast.Contains(id)) {
            continue;
          }
          const Item& item = parsed->ast.Get(id);
          for (AttributeId attribute : parsed->ast.Get(item.attributes)) {
            const std::string_view name =
                interner_.Text(parsed->ast.Get(attribute).name);
            if (name == "Test") {
              result.tests.push_back(item.name);
              break;
            }
            if (name == "Bench") {
              result.benches.push_back(item.name);
              break;
            }
          }
        }

        // A call to a `comptime` function with constant arguments is the
        // constant it works out to. Before the passes, so that the
        // reference tier sees the same program -- and before inlining in
        // particular, which would otherwise take the call apart and leave
        // nothing to evaluate (D103).
        DiagnosticEngine comptime;
        FoldComptimeCalls(interner_, types_, &result.module, &comptime,
                          &parsed->ast, tree);

        result.unoptimized = result.module;
        ir::Optimize(&types_, &result.module, &layouts);
        // Again, for the ones whose arguments only became constant once
        // the passes had propagated them.
        FoldComptimeCalls(interner_, types_, &result.module, nullptr,
                          nullptr, nullptr);

        const ir::VerificationResult optimized =
            ir::Verify(interner_, types_, result.module);
        for (const std::string& problem : optimized.problems) {
          result.problems.push_back("after passes: " + problem);
        }

        // A `comptime` function has no effects -- which wave 5 already
        // checks -- so it can be run here, inside the compiler. A trap is
        // the program telling the compiler something is wrong, so it
        // becomes a diagnostic rather than an internal note.

        for (uint32_t index = 0; tree != nullptr &&
                                 index < tree->entries.size();
             ++index) {
          const ItemTreeEntry& entry = tree->entries[index];
          if (entry.kind != ItemKind::kFunction ||
              !HasFlag(entry.flags, ItemFlags::kComptime)) {
            continue;
          }
          const ir::Function* function =
              result.unoptimized.Find(entry.name);
          if (function == nullptr || function->is_declaration() ||
              !function->ParamsOf(function->entry()).empty()) {
            // One that takes arguments is evaluated where it is called,
            // which needs the call site; that is a later wave.
            continue;
          }

          // A tight budget: this runs during compilation, so a loop that
          // does not finish has to stop being the compiler's problem.
          interp::Limits limits;
          limits.steps = 1'000'000;
          const interp::Outcome outcome =
              interp::Run(interner_, types_, result.unoptimized, *function,
                          {}, limits);
          if (outcome.ok()) {
            continue;
          }
          const ItemId id = ResolveItem(parsed->ast, index);
          if (!parsed->ast.Contains(id)) {
            continue;
          }
          std::string message = "`";
          message.append(interner_.Text(entry.name));
          message.append("` could not be evaluated at compile time: ");
          message.append(interp::TrapKindName(outcome.trap));
          comptime.Error(kComptimeFailed, std::move(message))
              .Primary(parsed->ast.Get(id).name_span, outcome.message);
        }
        for (const Diagnostic& diagnostic : comptime.diagnostics()) {
          result.diagnostics.push_back(diagnostic);
        }
        return MakeResult(std::move(result));
      });

  engine_.Register(
      QueryKind::kFileDiagnostics,
      [this](QueryEngine& db, uint32_t arg) -> QueryResult {
        const FileId file{arg};
        std::vector<Diagnostic> all;
        if (const TokenizedFile* tokens =
                db.GetAs<TokenizedFile>(TokensKey(file))) {
          all.insert(all.end(), tokens->diagnostics.begin(),
                     tokens->diagnostics.end());
        }

        const ParsedFile* parsed = db.GetAs<ParsedFile>(ParsedFileKey(file));
        if (parsed == nullptr) {
          return MakeResult(std::move(all));
        }
        all.insert(all.end(), parsed->diagnostics.begin(),
                   parsed->diagnostics.end());

        // Everything below resolves its positions against the syntax tree of
        // this revision, so a cached cross-file result can never contribute a
        // stale span.
        DiagnosticEngine reporter;
        const Ast& ast = parsed->ast;
        ReportImportProblems(db, file, ast, &reporter);
        ReportDuplicateDeclarations(db, file, ast, &reporter);

        all.insert(all.end(), reporter.diagnostics().begin(),
                   reporter.diagnostics().end());

        if (const auto* resolved =
                db.GetAs<ResolvedNames>(ResolutionKey(file))) {
          all.insert(all.end(), resolved->second.begin(),
                     resolved->second.end());
        }
        if (const CheckedFile* checked =
                db.GetAs<CheckedFile>(TypeCheckKey(file))) {
          all.insert(all.end(), checked->diagnostics.begin(),
                     checked->diagnostics.end());
        }
        if (const auto* effects = db.GetAs<std::vector<Diagnostic>>(
                EffectDiagnosticsKey(file))) {
          all.insert(all.end(), effects->begin(), effects->end());
        }
        if (const auto* systems = db.GetAs<std::vector<Diagnostic>>(
                SystemDiagnosticsKey(file))) {
          all.insert(all.end(), systems->begin(), systems->end());
        }
        if (const LoweredFile* lowered =
                db.GetAs<LoweredFile>(LoweredFileKey(file))) {
          all.insert(all.end(), lowered->diagnostics.begin(),
                     lowered->diagnostics.end());
        }
        ApplyLintPolicy(file, &all);
        return MakeResult(std::move(all));
      });
}

const Manifest* Database::ManifestFor(FileId file) const {
  const Module* module = GetModule(ModuleOf(file));
  if (module == nullptr) {
    return nullptr;
  }
  const Package* package = GetPackage(module->package);
  return package == nullptr ? nullptr : &package->manifest;
}

void Database::ApplyLintPolicy(FileId file,
                               std::vector<Diagnostic>* diagnostics) const {
  const Manifest* manifest = ManifestFor(file);
  if (manifest == nullptr || manifest->lints.empty()) {
    return;
  }

  std::vector<Diagnostic> kept;
  kept.reserve(diagnostics->size());
  for (Diagnostic& diagnostic : *diagnostics) {
    const Severity severity =
        manifest->SeverityFor(diagnostic.code, diagnostic.severity);
    if (severity == Severity::kOff) {
      continue;
    }
    diagnostic.severity = severity;
    kept.push_back(std::move(diagnostic));
  }
  *diagnostics = std::move(kept);
}

void Database::ReportImportProblems(QueryEngine& db,
                                    FileId file,
                                    const Ast& ast,
                                    DiagnosticEngine* reporter) {
  const ResolvedImports* imports =
      db.GetAs<ResolvedImports>(ImportsKey(file));
  if (imports == nullptr || imports->outside_workspace) {
    return;
  }

  uint32_t ordinal = 0;
  for (ItemId id : ast.Get(ast.root())) {
    const Item& item = ast.Get(id);
    if (item.kind != ItemKind::kNeed) {
      continue;
    }
    if (ordinal >= imports->imports.size()) {
      break;
    }
    const ResolvedImport& resolved = imports->imports[ordinal++];

    if (!resolved.module.is_valid()) {
      std::string message = "no module named `";
      message.append(ImportPathText(interner_, ast, item));
      message.append("`");
      reporter->Error(kUnresolvedModule, std::move(message))
          .Primary(item.span, "this import does not resolve")
          .Note("modules are named after the package and the directories "
                "below its source root");
      continue;
    }

    const Module* module = GetModule(resolved.module);
    for (Symbol name : resolved.missing) {
      std::string message = "`";
      message.append(interner_.Text(name));
      message.append("` is not declared in `");
      message.append(module != nullptr ? module->name : "");
      message.append("`");
      reporter->Error(kUnresolvedName, std::move(message))
          .Primary(item.span);
    }
    for (Symbol name : resolved.private_names) {
      std::string message = "`";
      message.append(interner_.Text(name));
      message.append("` is not public");
      reporter->Error(kPrivateName, std::move(message))
          .Primary(item.span)
          .Note("mark the declaration `pub` to export it from its module");
    }
  }
}

void Database::ReportDuplicateDeclarations(QueryEngine& db,
                                           FileId file,
                                           const Ast& ast,
                                           DiagnosticEngine* reporter) {
  const ModuleId own = ModuleOf(file);
  if (!own.is_valid()) {
    return;
  }
  const ModuleScope* scope = db.GetAs<ModuleScope>(ModuleScopeKey(own));
  const ItemTree* tree = db.GetAs<ItemTree>(ItemTreeKey(file));
  if (scope == nullptr || tree == nullptr || scope->duplicates.empty()) {
    return;
  }

  const Module* module = GetModule(own);
  for (uint32_t index = 0; index < tree->entries.size(); ++index) {
    const ItemTreeEntry& entry = tree->entries[index];
    if (entry.parent != ItemTreeEntry::kNoParent ||
        !scope->IsDuplicate(entry.name)) {
      continue;
    }
    const ItemId item_id = ResolveItem(ast, index);
    if (!ast.Contains(item_id)) {
      continue;
    }

    std::string message = "`";
    message.append(interner_.Text(entry.name));
    message.append("` is declared more than once in module `");
    message.append(module != nullptr ? module->name : "");
    message.append("`");
    reporter->Error(kDuplicateDeclaration, std::move(message))
        .Primary(ast.Get(item_id).name_span);
  }
}

PackageId Database::AddPackage(std::string name,
                               std::string directory,
                               Manifest manifest) {
  Package package;
  package.name = std::move(name);
  package.directory = std::move(directory);
  package.manifest = std::move(manifest);
  packages_.push_back(std::move(package));
  BumpWorkspace();
  return PackageId{static_cast<uint32_t>(packages_.size() - 1)};
}

const Package* Database::GetPackage(PackageId package) const {
  if (!package.is_valid() || package.index >= packages_.size()) {
    return nullptr;
  }
  return &packages_[package.index];
}

ModuleId Database::AddModule(std::string name,
                             std::string directory,
                             PackageId package) {
  const ModuleId existing = FindModule(name);
  if (existing.is_valid()) {
    return existing;
  }

  Module module;
  module.name = std::move(name);
  module.directory = std::move(directory);
  module.package = package;
  modules_.push_back(std::move(module));

  const ModuleId id{static_cast<uint32_t>(modules_.size() - 1)};
  engine_.SetInput(ModuleFilesKey(id), MakeResult(std::vector<FileId>()));
  BumpWorkspace();
  return id;
}

void Database::ShareMembers(abi::LayoutStore* layouts) {
  if (layouts == nullptr) {
    return;
  }
  // Every file that has been checked, in whatever order they come: a
  // member table is the same answer wherever it is read from, so the
  // first file that has one wins and the rest cost a pointer each.
  for (const Module& module : modules_) {
    for (FileId file : module.files) {
      if (const CheckedFile* checked = Checked(file)) {
        layouts->Also(&checked->resolution);
      }
    }
  }
}

void Database::SetModuleFiles(ModuleId module, std::vector<FileId> files) {
  if (!module.is_valid() || module.index >= modules_.size()) {
    return;
  }
  modules_[module.index].files = files;

  for (FileId file : files) {
    if (file.index >= module_of_file_.size()) {
      module_of_file_.resize(file.index + 1, ModuleId{});
    }
    module_of_file_[file.index] = module;
  }

  engine_.SetInput(ModuleFilesKey(module), MakeResult(std::move(files)));
  BumpWorkspace();
}

ModuleId Database::FindModule(std::string_view name) const {
  for (uint32_t i = 0; i < modules_.size(); ++i) {
    if (modules_[i].name == name) {
      return ModuleId{i};
    }
  }
  return ModuleId{};
}

ModuleId Database::ModuleOf(FileId file) const {
  if (!file.is_valid() || file.index >= module_of_file_.size()) {
    return ModuleId{};
  }
  return module_of_file_[file.index];
}

const Module* Database::GetModule(ModuleId module) const {
  if (!module.is_valid() || module.index >= modules_.size()) {
    return nullptr;
  }
  return &modules_[module.index];
}

void Database::BumpWorkspace() {
  ++workspace_version_;
  engine_.SetInput(WorkspaceKey(), MakeResult(workspace_version_));
}

FileId Database::AddFile(std::string path,
                         std::string text,
                         FileRole role) {
  const uint64_t hash = HashText(text);
  const auto size = static_cast<uint32_t>(text.size());
  const FileId file = sources_.AddFile(std::move(path), std::move(text));
  RecordRole(file, role);
  engine_.SetInput(SourceTextKey(file),
                   MakeResult(SourceFingerprint{hash, size}));
  return file;
}

FileId Database::LoadFile(std::string_view path,
                          std::string* error,
                          FileRole role) {
  const FileId file = sources_.LoadFile(path, error);
  if (!file.is_valid()) {
    return file;
  }
  RecordRole(file, role);
  const std::string_view text = sources_.Text(file);
  engine_.SetInput(
      SourceTextKey(file),
      MakeResult(SourceFingerprint{HashText(text),
                                   static_cast<uint32_t>(text.size())}));
  return file;
}

void Database::RecordRole(FileId file, FileRole role) {
  if (file.index >= roles_.size()) {
    roles_.resize(file.index + 1, FileRole::kSource);
  }
  roles_[file.index] = role;
}

FileId Database::FindFile(std::string_view path) const {
  for (uint32_t i = 0; i < sources_.file_count(); ++i) {
    if (sources_.Path(FileId{i}) == path) {
      return FileId{i};
    }
  }
  return FileId{};
}

void Database::SeedItemTree(FileId file, ItemTree tree) {
  // The only dependency recorded is the file's text: that is exactly what the
  // caller verified, and it is what an edit changes.
  engine_.Seed(ItemTreeKey(file), MakeComparableResult(std::move(tree)),
               {SourceTextKey(file)});
}

FileRole Database::RoleOf(FileId file) const {
  if (!file.is_valid() || file.index >= roles_.size()) {
    return FileRole::kSource;
  }
  return roles_[file.index];
}

void Database::SetFileText(FileId file, std::string text) {
  const uint64_t hash = HashText(text);
  const auto size = static_cast<uint32_t>(text.size());
  sources_.ReplaceText(file, std::move(text));
  engine_.SetInput(SourceTextKey(file),
                   MakeResult(SourceFingerprint{hash, size}));
}

const TokenizedFile* Database::Tokens(FileId file) {
  return engine_.GetAs<TokenizedFile>(TokensKey(file));
}

const ModuleScope* Database::Scope(ModuleId module) {
  return engine_.GetAs<ModuleScope>(ModuleScopeKey(module));
}

const AccessGraph* Database::Systems(ModuleId module) {
  return engine_.GetAs<AccessGraph>(ModuleSystemsKey(module));
}

const ResolvedImports* Database::Imports(FileId file) {
  return engine_.GetAs<ResolvedImports>(ImportsKey(file));
}

const FileResolution* Database::Resolution(FileId file) {
  const auto* pair = engine_.GetAs<ResolvedNames>(ResolutionKey(file));
  return pair == nullptr ? nullptr : &pair->first;
}

const CheckedFile* Database::Checked(FileId file) {
  return engine_.GetAs<CheckedFile>(TypeCheckKey(file));
}

const LoweredFile* Database::Lowered(FileId file) {
  return engine_.GetAs<LoweredFile>(LoweredFileKey(file));
}

const ParsedFile* Database::Parse(FileId file) {
  return engine_.GetAs<ParsedFile>(ParsedFileKey(file));
}

const ItemTree* Database::Items(FileId file) {
  return engine_.GetAs<ItemTree>(ItemTreeKey(file));
}

const std::vector<Diagnostic>* Database::Diagnostics(FileId file) {
  if (RoleOf(file) != FileRole::kSource) {
    // A manifest is not Wolang source; lexing it would produce nonsense.
    static const std::vector<Diagnostic> kEmpty;
    return &kEmpty;
  }
  return engine_.GetAs<std::vector<Diagnostic>>(DiagnosticsKey(file));
}

std::vector<Diagnostic> Database::AllDiagnostics() {
  std::vector<Diagnostic> all;
  for (uint32_t i = 0; i < sources_.file_count(); ++i) {
    if (RoleOf(FileId{i}) != FileRole::kSource) {
      continue;
    }
    const std::vector<Diagnostic>* file_diagnostics = Diagnostics(FileId{i});
    if (file_diagnostics != nullptr) {
      all.insert(all.end(), file_diagnostics->begin(),
                 file_diagnostics->end());
    }
  }
  return all;
}

}  // namespace wolang
