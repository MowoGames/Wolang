// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wolang {
namespace ir {

static constexpr uint32_t kMaxReadIndex = 1u << 20;

class TypedReader {
 public:
  TypedReader(StringInterner* interner, TypeStore* types, std::string_view text, const ReadOptions* options) : m_Interner(interner), m_Types(types), m_Text(text), m_Options(options) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(Opcode::kCount); ++i) {
      const Opcode opcode = static_cast<Opcode>(i);
      m_Opcodes.emplace(std::string(OpcodeMnemonic(opcode)), opcode);
    }
  }

  ReadResult Run() {
    size_t start = 0;
    while (start <= m_Text.size() && !Failed()) {
      size_t end = m_Text.find('\n', start);
      const bool last = end == std::string_view::npos;
      if (last) {
        end = m_Text.size();
      }

      m_Line = m_Text.substr(start, end - start);
      if (!m_Line.empty() && m_Line.back() == '\r') {
        m_Line.remove_suffix(1);
      }
      m_Col = 0;
      ++m_LineNumber;
      ReadLine();

      if (last) {
        break;
      }
      start = end + 1;
    }

    if (m_InFunction && !Failed()) {
      Fail("the function is missing its closing }");
    }
    return std::move(m_Result);
  }

 private:
  struct Pending {
    uint32_t block = 0;
    uint32_t position = 0;
    Inst inst;
  };

  Function& Current() { return m_Result.module.functions[m_FunctionIndex]; }

  bool Failed() const { return !m_Result.errors.empty(); }

  bool Fail(const std::string& message) {
    if (!Failed()) {
      m_Result.errors.push_back("line " + std::to_string(m_LineNumber) + ": " + message);
    }
    return false;
  }

  bool AtEnd() const { return m_Col >= m_Line.size(); }

  char Peek() const { return AtEnd() ? '\0' : m_Line[m_Col]; }

  void SkipBlank() {
    while (m_Col < m_Line.size() && (m_Line[m_Col] == ' ' || m_Line[m_Col] == '\t')) {
      ++m_Col;
    }
  }

  void SkipSeparators() {
    while (m_Col < m_Line.size() && (m_Line[m_Col] == ' ' || m_Line[m_Col] == '\t' || m_Line[m_Col] == ',')) {
      ++m_Col;
    }
  }

  bool Consume(char c) {
    SkipBlank();
    if (Peek() == c) {
      ++m_Col;
      return true;
    }
    return false;
  }

  bool Expect(char c) {
    if (Consume(c)) {
      return true;
    }
    return Fail(std::string("expected '") + c + "'");
  }

  bool ExpectEnd() {
    SkipBlank();
    if (AtEnd()) {
      return true;
    }
    return Fail("unexpected `" + std::string(m_Line.substr(m_Col)) + "`");
  }

  static bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; }

  std::string ReadWord() {
    SkipBlank();
    const size_t begin = m_Col;
    while (m_Col < m_Line.size() && IsWordChar(m_Line[m_Col])) {
      ++m_Col;
    }
    return std::string(m_Line.substr(begin, m_Col - begin));
  }

  bool ConsumeWord(std::string_view word) {
    SkipBlank();
    const size_t saved = m_Col;
    if (ReadWord() == word) {
      return true;
    }
    m_Col = saved;
    return false;
  }

  bool ReadUnsigned(uint64_t* out) {
    SkipBlank();
    const size_t begin = m_Col;
    uint64_t value = 0;
    while (m_Col < m_Line.size() && m_Line[m_Col] >= '0' && m_Line[m_Col] <= '9') {
      const uint64_t digit = static_cast<uint64_t>(m_Line[m_Col] - '0');
      if (value > (UINT64_MAX - digit) / 10) {
        return Fail("the number is too large");
      }
      value = value * 10 + digit;
      ++m_Col;
    }

    if (m_Col == begin) {
      return Fail("expected a number");
    }
    *out = value;
    return true;
  }

  bool ReadU32(uint32_t* out) {
    uint64_t value = 0;
    if (!ReadUnsigned(&value)) {
      return false;
    }
    if (value > 0xFFFFFFFFull) {
      return Fail("the number does not fit 32 bits");
    }
    *out = static_cast<uint32_t>(value);
    return true;
  }

  bool ReadIndex(uint32_t* out) {
    uint64_t value = 0;
    if (!ReadUnsigned(&value)) {
      return false;
    }
    if (value > kMaxReadIndex) {
      return Fail("the index is out of range");
    }
    *out = static_cast<uint32_t>(value);
    return true;
  }

  static int HexValue(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  bool ReadQuoted(std::string* out) {
    if (!Expect('"')) {
      return false;
    }

    out->clear();
    while (m_Col < m_Line.size()) {
      const char c = m_Line[m_Col++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }

      if (AtEnd()) {
        break;
      }
      const char escape = m_Line[m_Col++];
      if (escape == 'n') {
        out->push_back('\n');
      } else if (escape == 'r') {
        out->push_back('\r');
      } else if (escape == 't') {
        out->push_back('\t');
      } else if (escape == '"' || escape == '\\') {
        out->push_back(escape);
      } else if (escape == 'x' && m_Col + 1 < m_Line.size() && HexValue(m_Line[m_Col]) >= 0 && HexValue(m_Line[m_Col + 1]) >= 0) {
        out->push_back(static_cast<char>(HexValue(m_Line[m_Col]) * 16 + HexValue(m_Line[m_Col + 1])));
        m_Col += 2;
      } else {
        return Fail("a bad escape in the text");
      }
    }
    return Fail("the text is not closed");
  }

  bool ReadTypeList(std::vector<TypeId>* out) {
    if (!Expect('(')) {
      return false;
    }

    SkipBlank();
    if (Consume(')')) {
      return true;
    }

    for (;;) {
      const TypeId element = ReadType();
      if (Failed()) {
        return false;
      }
      out->push_back(element);

      if (Consume(',')) {
        continue;
      }
      return Expect(')');
    }
  }

  TypeId ReadWrapped(const std::string& word) {
    if (!Expect('(')) {
      return TypeId{};
    }

    if (word == "array" || word == "soaarray") {
      uint64_t length = 0;
      if (!ReadUnsigned(&length) || !Expect(',')) {
        return TypeId{};
      }
      const TypeId element = ReadType();
      if (Failed() || !Expect(')')) {
        return TypeId{};
      }
      return m_Types->Array(element, length, word == "soaarray");
    }

    const TypeId element = ReadType();
    if (Failed() || !Expect(')')) {
      return TypeId{};
    }

    if (word == "opt") {
      return m_Types->Optional(element);
    }
    if (word == "ptr" || word == "ptrmut") {
      return m_Types->Pointer(element, word == "ptrmut");
    }
    if (word == "ref" || word == "refmut") {
      return m_Types->Reference(element, word == "refmut");
    }
    return m_Types->Slice(element, word == "soaslice");
  }

  TypeId ReadType() {
    SkipBlank();
    if (Peek() == '`') {
      const size_t close = m_Line.find('`', m_Col + 1);
      if (close == std::string_view::npos) {
        Fail("a type in backticks is not closed");
        return TypeId{};
      }

      const std::string key(m_Line.substr(m_Col + 1, close - m_Col - 1));
      m_Col = close + 1;
      const auto found = m_Options->named.find(key);
      if (found == m_Options->named.end()) {
        Fail("the type `" + key + "` is not one the reader was given");
        return TypeId{};
      }
      return found->second;
    }

    const std::string word = ReadWord();
    if (word.empty()) {
      Fail("expected a type");
      return TypeId{};
    }

    if (word == "invalid") {
      return TypeId{};
    }
    if (word == "void") {
      return m_Types->void_type();
    }
    if (word == "never") {
      return m_Types->never();
    }
    if (word == "bool") {
      return m_Types->bool_type();
    }
    if (word == "char") {
      return m_Types->char_type();
    }
    if (word == "text") {
      return m_Types->str();
    }
    if (word == "Text") {
      return m_Types->string_type();
    }
    if (word == "f32") {
      return m_Types->Float(FloatWidth::k32);
    }
    if (word == "f64") {
      return m_Types->Float(FloatWidth::k64);
    }

    if ((word[0] == 's' || word[0] == 'u') && word.size() > 1 && word.size() < 4) {
      const std::string bits = word.substr(1);
      const bool is_signed = word[0] == 's';
      if (bits == "8") {
        return m_Types->Int(IntWidth::k8, is_signed);
      }
      if (bits == "16") {
        return m_Types->Int(IntWidth::k16, is_signed);
      }
      if (bits == "32") {
        return m_Types->Int(IntWidth::k32, is_signed);
      }
      if (bits == "64") {
        return m_Types->Int(IntWidth::k64, is_signed);
      }
    }

    if (word == "tuple") {
      std::vector<TypeId> elements;
      if (!ReadTypeList(&elements)) {
        return TypeId{};
      }
      return m_Types->Tuple(elements);
    }

    if (word == "fn" || word == "fnkept") {
      std::vector<TypeId> params;
      if (!ReadTypeList(&params) || !Expect('-') || !Expect('>')) {
        return TypeId{};
      }
      const TypeId result = ReadType();
      if (Failed()) {
        return TypeId{};
      }
      return m_Types->Function(params, result, word == "fnkept");
    }

    if (word == "opt" || word == "ptr" || word == "ptrmut" || word == "ref" || word == "refmut" || word == "slice" || word == "soaslice" || word == "array" || word == "soaarray") {
      return ReadWrapped(word);
    }

    Fail("`" + word + "` is not a type");
    return TypeId{};
  }

  void ReadLine() {
    SkipBlank();
    if (AtEnd()) {
      return;
    }

    if (m_InFunction) {
      ReadBodyLine();
      return;
    }

    if (ConsumeWord("module")) {
      ReadModuleLine();
    } else if (ConsumeWord("string")) {
      std::string text;
      if (ReadQuoted(&text)) {
        m_Result.module.strings.push_back(text);
        ExpectEnd();
      }
    } else if (ConsumeWord("class")) {
      ReadClassLine();
    } else if (ConsumeWord("fun")) {
      ReadFunctionLine();
    } else {
      Fail("expected module, string, class or fun");
    }
  }

  void ReadModuleLine() {
    uint32_t initializer = 0;
    uint32_t runaway = 0;
    if (!ConsumeWord("initializer") || !Expect('=') || !ReadU32(&initializer)) {
      Fail("expected initializer=");
      return;
    }
    if (!ConsumeWord("runaway") || !Expect('=') || !ReadU32(&runaway)) {
      Fail("expected runaway=");
      return;
    }

    m_Result.module.initializer = initializer;
    m_Result.module.runaway_item = runaway;
    ExpectEnd();
  }

  void ReadClassLine() {
    ClassInfo info;
    if (!ReadUnsigned(&info.token)) {
      return;
    }

    info.type = ReadType();
    if (Failed() || !Expect('[')) {
      return;
    }

    for (;;) {
      SkipBlank();
      if (Consume(']')) {
        break;
      }

      ClassInfo::Method method;
      if (!ReadUnsigned(&method.selector) || !Expect(':') || !ReadU32(&method.function)) {
        return;
      }
      info.methods.push_back(method);
    }

    m_Result.module.classes.push_back(info);
    ExpectEnd();
  }

  void ReadEffects(Function* function) {
    if (!ConsumeWord("uses")) {
      return;
    }

    for (;;) {
      const std::string name = ReadWord();
      const BuiltinEffect effect = LookupBuiltinEffect(name);
      if (effect == BuiltinEffect::kCount) {
        Fail("`" + name + "` is not an effect the reader knows");
        return;
      }
      function->effects.Add(EffectId{static_cast<uint32_t>(effect)});

      if (!Consume(',')) {
        return;
      }
    }
  }

  void ReadAttributes(Function* function) {
    if (!Consume('[')) {
      return;
    }

    for (;;) {
      SkipBlank();
      if (Consume(']')) {
        return;
      }

      const std::string word = ReadWord();
      if (word == "known") {
        function->effects_known = true;
      } else if (word == "task") {
        function->is_task = true;
      } else if (word == "comptime") {
        function->is_comptime = true;
      } else if (word == "primtask") {
        function->is_primitive_task = true;
      } else if (word == "foreign") {
        std::string symbol;
        if (!Expect('=') || !ReadQuoted(&symbol)) {
          return;
        }
        function->foreign_symbol = m_Interner->Intern(symbol);
      } else if (word == "self") {
        if (!Expect('=')) {
          return;
        }
        function->self_type = ReadType();
        if (Failed()) {
          return;
        }
      } else if (word == "src") {
        if (!Expect('=') || !ReadU32(&function->source_file) || !Expect(':') || !ReadU32(&function->source_item) || !Expect(':') || !ReadU32(&function->source_slot)) {
          return;
        }
      } else {
        Fail("`" + word + "` is not an attribute");
        return;
      }
    }
  }

  void ReadFunctionLine() {
    Function function;
    std::string name;
    if (!Expect('@') || !ReadQuoted(&name) || !Expect(':')) {
      return;
    }
    function.name = m_Interner->Intern(name);

    function.type = ReadType();
    if (Failed()) {
      return;
    }

    ReadEffects(&function);
    if (Failed()) {
      return;
    }

    ReadAttributes(&function);
    if (Failed()) {
      return;
    }

    m_Result.module.functions.push_back(function);
    m_FunctionIndex = m_Result.module.functions.size() - 1;

    if (Consume('{')) {
      m_InFunction = true;
      m_Block = InstId::kInvalidIndex;
      m_Pending.clear();
      ExpectEnd();
      return;
    }
    ExpectEnd();
  }

  void EnsureInst(Function* function, uint32_t index) {
    if (index >= function->instructions.size()) {
      function->instructions.resize(index + 1);
      function->owners.resize(index + 1);
      function->spans.resize(index + 1);
    }
  }

  void ReadBlockHeader() {
    uint32_t index = 0;
    ++m_Col;
    if (!ReadIndex(&index)) {
      return;
    }

    Function& function = Current();
    if (index >= function.blocks.size()) {
      function.blocks.resize(index + 1);
    }
    m_Block = index;

    if (Consume('(')) {
      for (;;) {
        SkipBlank();
        if (Consume(')')) {
          break;
        }

        uint32_t value = 0;
        if (!Expect('%') || !ReadIndex(&value) || !Expect(':')) {
          return;
        }
        const TypeId type = ReadType();
        if (Failed()) {
          return;
        }

        Function& target = Current();
        Block& block = target.blocks[m_Block];
        if (block.instructions.size() != block.param_count) {
          Fail("a parameter after an instruction");
          return;
        }

        EnsureInst(&target, value);
        if (target.owners[value].is_valid()) {
          Fail("%" + std::to_string(value) + " is defined twice");
          return;
        }

        Inst inst;
        inst.opcode = Opcode::kParam;
        inst.type = type;
        target.instructions[value] = inst;
        target.owners[value] = BlockId{m_Block};
        block.instructions.push_back(InstId{value});
        ++block.param_count;
        Consume(',');
      }
    }

    if (!Expect(':')) {
      return;
    }

    SkipBlank();
    if (m_Line.substr(m_Col, 2) == "//") {
      m_Col += 2;
      if (Peek() == ' ') {
        ++m_Col;
      }
      Current().blocks[m_Block].label = std::string(m_Line.substr(m_Col));
      m_Col = m_Line.size();
    }
    ExpectEnd();
  }

  void FinishFunction() {
    Function& function = Current();
    for (const Pending& pending : m_Pending) {
      const uint32_t id = static_cast<uint32_t>(function.instructions.size());
      function.instructions.push_back(pending.inst);
      function.owners.push_back(BlockId{pending.block});
      function.spans.push_back(SourceSpan());
      function.blocks[pending.block].instructions[pending.position] = InstId{id};
    }
    m_Pending.clear();
    m_InFunction = false;
  }

  void ReadBodyLine() {
    SkipBlank();
    const char c = Peek();
    if (c == '}') {
      ++m_Col;
      if (ExpectEnd()) {
        FinishFunction();
      }
      return;
    }

    if (c == '^') {
      ReadBlockHeader();
      return;
    }

    uint32_t index = 0;
    bool has_index = false;
    if (c == '%') {
      ++m_Col;
      if (!ReadIndex(&index) || !Expect('=')) {
        return;
      }
      has_index = true;
    }

    const std::string mnemonic = ReadWord();
    const auto found = m_Opcodes.find(mnemonic);
    if (found == m_Opcodes.end()) {
      Fail("`" + mnemonic + "` is not an instruction");
      return;
    }

    const Opcode opcode = found->second;
    if (opcode == Opcode::kParam) {
      Fail("a param belongs in a block header");
      return;
    }
    if (ProducesValue(opcode) != has_index) {
      Fail(has_index ? "`" + mnemonic + "` produces no value" : "`" + mnemonic + "` produces a value and needs a name");
      return;
    }
    if (m_Block == InstId::kInvalidIndex) {
      Fail("an instruction before the first block");
      return;
    }

    ReadInstruction(opcode, has_index, index);
  }

  void ReadInstruction(Opcode opcode, bool has_index, uint32_t index) {
    Function& function = Current();
    Inst inst;
    inst.opcode = opcode;

    switch (opcode) {
      case Opcode::kConstInt:
        if (!ReadUnsigned(&inst.payload.integer)) {
          return;
        }
        break;
      case Opcode::kConstFloat: {
        SkipBlank();
        const size_t begin = m_Col;
        while (m_Col < m_Line.size() && m_Line[m_Col] != ' ' && m_Line[m_Col] != ',') {
          ++m_Col;
        }

        const std::string token(m_Line.substr(begin, m_Col - begin));
        char* end = nullptr;
        inst.payload.floating = std::strtod(token.c_str(), &end);
        if (token.empty() || end != token.c_str() + token.size()) {
          Fail("`" + token + "` is not a float");
          return;
        }
        break;
      }
      case Opcode::kConstBool: {
        const std::string word = ReadWord();
        if (word != "true" && word != "false") {
          Fail("expected true or false");
          return;
        }
        inst.payload.boolean = word == "true";
        break;
      }
      case Opcode::kConstString:
        if (!Expect('#') || !ReadU32(&inst.payload.string)) {
          return;
        }
        break;
      case Opcode::kSpawnCall:
      case Opcode::kWaitCall:
      case Opcode::kCall:
      case Opcode::kFuncAddress: {
        std::string ignored;
        if (!Expect('@') || !ReadQuoted(&ignored) || !Expect('#') || !ReadU32(&inst.payload.callee)) {
          return;
        }
        break;
      }
      case Opcode::kExtract:
      case Opcode::kInsert:
      case Opcode::kFieldAddress:
      case Opcode::kMakeVariant:
      case Opcode::kVariantPayload:
      case Opcode::kVirtualCall:
      case Opcode::kSoaAddress:
        if (!ReadU32(&inst.field)) {
          return;
        }
        break;
      case Opcode::kTypeSize: {
        if (!Expect(':')) {
          return;
        }
        const TypeId measured = ReadType();
        if (Failed()) {
          return;
        }
        inst.payload.integer = measured.index;
        break;
      }
      case Opcode::kAlloca:
        if (!ConsumeWord("slots") || !ReadU32(&inst.payload.slots)) {
          Fail("expected slots");
          return;
        }
        break;
      case Opcode::kClassTable:
        if (!ConsumeWord("class") || !ReadU32(&inst.payload.klass)) {
          Fail("expected class");
          return;
        }
        break;
      default:
        break;
    }

    inst.operands_begin = static_cast<uint32_t>(function.operands.size());
    for (;;) {
      SkipSeparators();
      if (Peek() != '%') {
        break;
      }

      uint32_t operand = 0;
      ++m_Col;
      if (!ReadIndex(&operand)) {
        return;
      }
      function.operands.push_back(InstId{operand});
      ++inst.operands_count;
    }

    inst.successors_begin = static_cast<uint32_t>(function.successors.size());
    inst.cases_begin = static_cast<uint32_t>(function.cases.size());
    if (opcode == Opcode::kSwitch) {
      if (!Expect('[')) {
        return;
      }

      for (;;) {
        SkipSeparators();
        if (Peek() == ']') {
          break;
        }

        uint64_t value = 0;
        uint32_t target = 0;
        if (!ReadUnsigned(&value) || !Expect('-') || !Expect('>') || !Expect('^') || !ReadIndex(&target)) {
          return;
        }
        function.cases.push_back(value);
        function.successors.push_back(BlockId{target});
        ++inst.cases_count;
        ++inst.successors_count;
      }

      uint32_t fallback = 0;
      if (!Expect(']') || !ConsumeWord("else") || !Expect('^') || !ReadIndex(&fallback)) {
        return;
      }
      function.successors.push_back(BlockId{fallback});
      ++inst.successors_count;
    } else {
      for (;;) {
        SkipBlank();
        if (Peek() != '^') {
          break;
        }

        uint32_t target = 0;
        ++m_Col;
        if (!ReadIndex(&target)) {
          return;
        }
        function.successors.push_back(BlockId{target});
        ++inst.successors_count;
      }
    }

    SkipBlank();
    if (Peek() == '!') {
      ++m_Col;
      if (ReadWord() != "unchecked") {
        Fail("expected unchecked");
        return;
      }
      inst.is_unchecked = true;
    }

    SkipBlank();
    if (Peek() == ':') {
      ++m_Col;
      inst.type = ReadType();
      if (Failed()) {
        return;
      }
    }

    if (!ExpectEnd()) {
      return;
    }

    Block& block = function.blocks[m_Block];
    if (has_index) {
      EnsureInst(&function, index);
      if (function.owners[index].is_valid()) {
        Fail("%" + std::to_string(index) + " is defined twice");
        return;
      }
      function.instructions[index] = inst;
      function.owners[index] = BlockId{m_Block};
      block.instructions.push_back(InstId{index});
      return;
    }

    Pending pending;
    pending.block = m_Block;
    pending.position = static_cast<uint32_t>(block.instructions.size());
    pending.inst = inst;
    m_Pending.push_back(pending);
    block.instructions.push_back(InstId{});
  }

  StringInterner* m_Interner;
  TypeStore* m_Types;
  std::string_view m_Text;
  const ReadOptions* m_Options;
  std::map<std::string, Opcode> m_Opcodes;
  ReadResult m_Result;
  std::string_view m_Line;
  size_t m_Col = 0;
  uint32_t m_LineNumber = 0;
  bool m_InFunction = false;
  size_t m_FunctionIndex = 0;
  uint32_t m_Block = InstId::kInvalidIndex;
  std::vector<Pending> m_Pending;
};

ReadResult ReadModule(StringInterner* interner, TypeStore* types, std::string_view text, const ReadOptions& options) {
  TypedReader reader(interner, types, text, &options);
  return reader.Run();
}

}  // namespace ir
}  // namespace wolang
