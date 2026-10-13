// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/database.h"

#include <utility>

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

}  // namespace

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

          // Relations travel with the tables: a bound of `Actor` is only
          // satisfiable because the declaring file says so, and a trait may
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
            for (TypeId trait : relation.traits) {
              bool known = false;
              for (TypeId existing : mine->traits) {
                if (existing == trait) {
                  known = true;
                  break;
                }
              }
              if (!known) {
                mine->traits.push_back(trait);
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
        for (ItemId id : ast.Get(ast.root())) {
          TypedBody body =
              TypeCheckItem(&types_, &interner_, &resolution, ast, id);
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
