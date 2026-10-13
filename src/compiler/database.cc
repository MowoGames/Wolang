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
          if (item.kind != ItemKind::kImport) {
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
    if (item.kind != ItemKind::kImport) {
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
