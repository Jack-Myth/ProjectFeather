#include <Feather/Compiler.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Feather {
namespace {

constexpr std::uint8_t Magic[8] = {'F', 'T', 'H', 'R', 'B', 'C', 0, 0};
constexpr std::uint16_t FormatVersion = 3;
constexpr std::uint16_t InstructionVersion = 1;
constexpr std::size_t HeaderSize = 16;
constexpr std::uint32_t MaxEntries = 65'536;

[[noreturn]] void Invalid(const char* Message) { throw std::invalid_argument(Message); }

class Writer {
public:
    explicit Writer(std::size_t Limit) : Limit(Limit) {}
    void U8(std::uint8_t Input) { Ensure(1); Data.push_back(Input); }
    void U16(std::uint16_t Input) {
        for (unsigned I = 0; I < 2; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I)));
    }
    void U32(std::uint32_t Input) {
        for (unsigned I = 0; I < 4; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I)));
    }
    void U64(std::uint64_t Input) {
        for (unsigned I = 0; I < 8; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I)));
    }
    void Raw(std::span<const std::uint8_t> Input) {
        Ensure(Input.size()); Data.insert(Data.end(), Input.begin(), Input.end());
    }
    void Blob(std::span<const std::uint8_t> Input) {
        if (Input.size() > std::numeric_limits<std::uint32_t>::max()) Invalid("bytecode field too large");
        U32(static_cast<std::uint32_t>(Input.size())); Raw(Input);
    }
    void String(const std::string& Input) {
        Blob({reinterpret_cast<const std::uint8_t*>(Input.data()), Input.size()});
    }
    void PatchU32(std::size_t Offset, std::uint32_t Input) {
        for (unsigned I = 0; I < 4; ++I)
            Data[Offset + I] = static_cast<std::uint8_t>(Input >> (8 * I));
    }
    std::size_t Size() const { return Data.size(); }
    std::vector<std::uint8_t> Finish() && { return std::move(Data); }
private:
    void Ensure(std::size_t Count) const {
        if (Count > Limit || Data.size() > Limit - Count) Invalid("bytecode size limit exceeded");
    }
    std::size_t Limit;
    std::vector<std::uint8_t> Data;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> Input) : Input(Input) {}
    std::uint8_t U8() { Require(1); return Input[Position++]; }
    std::uint16_t U16() {
        std::uint16_t Result = 0;
        for (unsigned I = 0; I < 2; ++I) Result |= std::uint16_t(U8()) << (I * 8);
        return Result;
    }
    std::uint32_t U32() {
        std::uint32_t Result = 0;
        for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(U8()) << (I * 8);
        return Result;
    }
    std::uint64_t U64() {
        std::uint64_t Result = 0;
        for (unsigned I = 0; I < 8; ++I) Result |= std::uint64_t(U8()) << (I * 8);
        return Result;
    }
    std::span<const std::uint8_t> Raw(std::size_t Count) {
        Require(Count);
        auto Result = Input.subspan(Position, Count);
        Position += Count;
        return Result;
    }
    std::span<const std::uint8_t> Blob() { return Raw(U32()); }
    std::string String() {
        auto Bytes = Blob();
        std::string Result(Bytes.begin(), Bytes.end());
        (void)Value::String(Result);
        return Result;
    }
    bool Done() const { return Position == Input.size(); }
private:
    void Require(std::size_t Count) const {
        if (Count > Input.size() - Position) Invalid("truncated bytecode artifact");
    }
    std::span<const std::uint8_t> Input;
    std::size_t Position = 0;
};

void WriteDefault(Writer& Output, const std::optional<Value>& Input) {
    if (!Input) { Output.U8(0); return; }
    switch (Input->GetType()) {
    case ValueType::Null: Output.U8(1); break;
    case ValueType::Bool: Output.U8(Input->AsBool() ? 3 : 2); break;
    case ValueType::Number:
        Output.U8(4); Output.U64(std::bit_cast<std::uint64_t>(Input->AsNumber())); break;
    case ValueType::String: Output.U8(5); Output.String(Input->AsString()); break;
    case ValueType::Object: Invalid("object default is not serializable");
    }
}

std::optional<Value> ReadDefault(Reader& Input) {
    switch (Input.U8()) {
    case 0: return std::nullopt;
    case 1: return Value{};
    case 2: return Value::Bool(false);
    case 3: return Value::Bool(true);
    case 4: return Value::Number(std::bit_cast<double>(Input.U64()));
    case 5: return Value::String(Input.String());
    default: Invalid("invalid default value tag");
    }
}

bool IsIdentifier(std::string_view Name) {
    if (Name.empty()) return false;
    auto Alpha = [](char C) { return (C >= 'A' && C <= 'Z') ||
                                      (C >= 'a' && C <= 'z') || C == '_'; };
    auto Digit = [](char C) { return C >= '0' && C <= '9'; };
    if (!Alpha(Name[0])) return false;
    for (char C : Name.substr(1)) if (!Alpha(C) && !Digit(C)) return false;
    return true;
}

void ValidateProgram(const CompiledProgram& Input) {
    if (!Input.Program) Invalid("missing bytecode module");
    Input.Program->Validate();
    auto FunctionAt = [&](std::uint32_t Index) {
        return Index < Input.Program->Constants.size() &&
               Input.Program->Constants[Index].Type == Constant::Kind::Function;
    };
    if (!FunctionAt(Input.Initializer) ||
        Input.Program->Constants[Input.Initializer].Function->ParameterCount != 0)
        Invalid("invalid initializer index");
    if (Input.Functions.size() > MaxEntries) Invalid("too many named functions");
    for (const auto& [Name, Index] : Input.Functions)
        if (!IsIdentifier(Name) || !FunctionAt(Index)) Invalid("invalid named function");
    if (Input.Exports.size() > MaxEntries) Invalid("too many exports");
    std::unordered_set<std::string> SeenExports;
    for (const auto& Name : Input.Exports)
        if (!IsIdentifier(Name) || !SeenExports.insert(Name).second)
            Invalid("invalid or duplicate export");
}

} // namespace

std::vector<std::uint8_t> SerializeProgram(const CompiledProgram& Input,
                                           std::size_t MaxBytes) {
    static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
    ValidateProgram(Input);
    Writer Output(MaxBytes);
    Output.Raw(Magic);
    Output.U16(FormatVersion);
    Output.U16(InstructionVersion);
    Output.U32(0);
    Output.U32(static_cast<std::uint32_t>(Input.Program->Constants.size()));
    for (const auto& Item : Input.Program->Constants) {
        switch (Item.Type) {
        case Constant::Kind::Number:
            Output.U8(0); Output.U64(std::bit_cast<std::uint64_t>(Item.Numeric)); break;
        case Constant::Kind::String:
            Output.U8(1); Output.String(Item.Text); break;
        case Constant::Kind::Function:
            Output.U8(2);
            Output.U32(Item.Function->ParameterCount);
            Output.U32(Item.Function->LocalCount);
            Output.U32(static_cast<std::uint32_t>(Item.Function->Defaults.size()));
            for (const auto& Default : Item.Function->Defaults) WriteDefault(Output, Default);
            Output.Blob(Item.Function->Code);
            break;
        }
    }
    Output.U32(Input.Initializer);
    std::vector<std::pair<std::string, std::uint32_t>> Functions(
        Input.Functions.begin(), Input.Functions.end());
    std::sort(Functions.begin(), Functions.end());
    Output.U32(static_cast<std::uint32_t>(Functions.size()));
    for (const auto& [Name, Index] : Functions) {
        Output.String(Name);
        Output.U32(Index);
    }
    Output.U8(Input.UsesQuickOperators ? 1 : 0);
    auto Exports = Input.Exports;
    std::sort(Exports.begin(), Exports.end());
    Output.U32(static_cast<std::uint32_t>(Exports.size()));
    for (const auto& Name : Exports) Output.String(Name);
    auto PayloadSize = Output.Size() - HeaderSize;
    if (PayloadSize > std::numeric_limits<std::uint32_t>::max()) Invalid("bytecode artifact too large");
    Output.PatchU32(12, static_cast<std::uint32_t>(PayloadSize));
    return std::move(Output).Finish();
}

CompiledProgram DeserializeProgram(std::span<const std::uint8_t> Bytes,
                                   std::size_t MaxBytes) {
    static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
    if (Bytes.size() > MaxBytes) Invalid("bytecode size limit exceeded");
    Reader Input(Bytes);
    for (auto Byte : Magic) if (Input.U8() != Byte) Invalid("invalid bytecode magic");
    if (Input.U16() != FormatVersion) Invalid("unsupported bytecode format version");
    if (Input.U16() != InstructionVersion) Invalid("unsupported instruction version");
    if (Bytes.size() < HeaderSize || Input.U32() != Bytes.size() - HeaderSize)
        Invalid("invalid bytecode payload size");
    CompiledProgram Result;
    Result.Program = std::make_shared<Module>();
    auto ConstantCount = Input.U32();
    if (ConstantCount > MaxEntries) Invalid("too many bytecode constants");
    Result.Program->Constants.reserve(ConstantCount);
    for (std::uint32_t I = 0; I < ConstantCount; ++I) {
        switch (Input.U8()) {
        case 0: Result.Program->AddNumber(std::bit_cast<double>(Input.U64())); break;
        case 1: Result.Program->AddString(Input.String()); break;
        case 2: {
            auto Function = std::make_shared<FunctionPrototype>();
            Function->ParameterCount = Input.U32();
            Function->LocalCount = Input.U32();
            auto DefaultCount = Input.U32();
            if (Function->ParameterCount > MaxEntries ||
                Function->LocalCount > MaxEntries ||
                DefaultCount != Function->ParameterCount)
                Invalid("invalid bytecode function slots");
            Function->Defaults.reserve(DefaultCount);
            for (std::uint32_t J = 0; J < DefaultCount; ++J)
                Function->Defaults.push_back(ReadDefault(Input));
            auto Code = Input.Blob();
            if (Code.empty() || Code.size() > 1'048'576) Invalid("invalid bytecode code size");
            Function->Code.assign(Code.begin(), Code.end());
            Result.Program->AddFunction(std::move(Function));
            break;
        }
        default: Invalid("invalid bytecode constant tag");
        }
    }
    Result.Initializer = Input.U32();
    auto FunctionCount = Input.U32();
    if (FunctionCount > MaxEntries) Invalid("too many named functions");
    for (std::uint32_t I = 0; I < FunctionCount; ++I) {
        auto Name = Input.String();
        auto Index = Input.U32();
        if (!Result.Functions.emplace(std::move(Name), Index).second)
            Invalid("duplicate named function");
    }
    auto Flag = Input.U8();
    if (Flag > 1) Invalid("invalid bytecode flags");
    Result.UsesQuickOperators = Flag == 1;
    auto Count = Input.U32();
    if (Count > MaxEntries) Invalid("invalid export count");
    Result.Exports.reserve(Count);
    for (std::uint32_t I = 0; I < Count; ++I) Result.Exports.push_back(Input.String());
    if (!std::is_sorted(Result.Exports.begin(), Result.Exports.end()))
        Invalid("unsorted exports");
    if (!Input.Done()) Invalid("trailing bytecode data");
    ValidateProgram(Result);
    return Result;
}

Value CompiledProgram::Initialize(Vm& Machine) const {
    if (!Program || !Machine.IsBuiltFrom(Program))
        throw std::invalid_argument("compiled program belongs to another VM module");
    return Machine.Run(Initializer);
}

void CompiledProgram::LoadInto(Vm& Machine, std::string Id) const {
    if (!Program) throw std::invalid_argument("missing compiled module");
    auto Identity = SerializeProgram(*this);
    Machine.LoadModule(std::move(Id), Program, Exports, Identity);
}

Value CompiledProgram::InitializeModule(Vm& Machine, std::string_view Id) const {
    if (!Program || !Machine.IsModuleBuiltFrom(Id, Program, SerializeProgram(*this)))
        throw std::invalid_argument("compiled program belongs to another VM module");
    return Machine.InitializeModule(Id, Initializer);
}

} // namespace Feather
