#include <Feather/Compiler.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace Feather {
namespace {

constexpr std::uint8_t Magic[8] = {'F', 'T', 'H', 'R', 'S', 'Y', 0, 0};
constexpr std::uint16_t FormatVersion = 3;
constexpr std::uint16_t InstructionVersion = 1;
constexpr std::uint32_t MaxFunctions = 65'536;

[[noreturn]] void Invalid(const char* Message) { throw std::invalid_argument(Message); }

std::uint64_t Fingerprint(std::span<const std::uint8_t> Bytes) {
    std::uint64_t Hash = 14'695'981'039'346'656'037ull;
    for (auto Byte : Bytes) { Hash ^= Byte; Hash *= 1'099'511'628'211ull; }
    return Hash;
}

std::uint32_t Checksum(std::span<const std::uint8_t> Bytes) {
    std::uint32_t Crc = 0xffff'ffffu;
    for (auto Byte : Bytes) {
        Crc ^= Byte;
        for (unsigned Bit = 0; Bit < 8; ++Bit)
            Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb8'8320u : 0u);
    }
    return ~Crc;
}

class Writer {
public:
    explicit Writer(std::size_t MaxBytes) : MaxBytes(MaxBytes) {}
    void U8(std::uint8_t Value) {
        if (Data.size() >= MaxBytes) Invalid("symbol size limit exceeded");
        Data.push_back(Value);
    }
    void U16(std::uint16_t Value) { for (unsigned I = 0; I < 2; ++I) U8(static_cast<std::uint8_t>(Value >> (8 * I))); }
    void U32(std::uint32_t Value) { for (unsigned I = 0; I < 4; ++I) U8(static_cast<std::uint8_t>(Value >> (8 * I))); }
    void U64(std::uint64_t Value) { for (unsigned I = 0; I < 8; ++I) U8(static_cast<std::uint8_t>(Value >> (8 * I))); }
    void String(std::string_view Value) {
        if (Value.size() > std::numeric_limits<std::uint32_t>::max())
            Invalid("symbol string too large");
        U32(static_cast<std::uint32_t>(Value.size()));
        for (unsigned char Byte : Value) U8(Byte);
    }
    void MagicBytes() { for (auto Byte : Magic) U8(Byte); }
    void PatchU32(std::size_t At, std::uint32_t Value) {
        for (unsigned I = 0; I < 4; ++I) Data[At + I] = Value >> (8 * I);
    }
    std::size_t Size() const { return Data.size(); }
    std::span<const std::uint8_t> View() const { return Data; }
    std::vector<std::uint8_t> Finish() && { return std::move(Data); }
private:
    std::size_t MaxBytes;
    std::vector<std::uint8_t> Data;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> Bytes) : Bytes(Bytes) {}
    std::uint8_t U8() { Require(1); return Bytes[Position++]; }
    std::uint16_t U16() {
        std::uint16_t Result = 0;
        for (unsigned I = 0; I < 2; ++I) Result |= std::uint16_t(U8()) << (8 * I);
        return Result;
    }
    std::uint32_t U32() {
        std::uint32_t Result = 0;
        for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(U8()) << (8 * I);
        return Result;
    }
    std::uint64_t U64() {
        std::uint64_t Result = 0;
        for (unsigned I = 0; I < 8; ++I) Result |= std::uint64_t(U8()) << (8 * I);
        return Result;
    }
    std::string String() {
        auto Count = U32();
        Require(Count);
        std::string Result(reinterpret_cast<const char*>(Bytes.data() + Position), Count);
        Position += Count;
        (void)Value::String(Result);
        return Result;
    }
    bool Done() const { return Position == Bytes.size(); }
private:
    void Require(std::size_t Count) const {
        if (Count > Bytes.size() - Position) Invalid("truncated symbol artifact");
    }
    std::span<const std::uint8_t> Bytes;
    std::size_t Position = 0;
};

std::uint32_t Narrow(std::size_t Value) {
    if (Value > std::numeric_limits<std::uint32_t>::max()) Invalid("symbol field too large");
    return static_cast<std::uint32_t>(Value);
}

std::vector<std::uint32_t> FunctionIndices(const Module& Program) {
    std::vector<std::uint32_t> Result;
    for (std::size_t I = 0; I < Program.Constants.size(); ++I)
        if (Program.Constants[I].Type == Constant::Kind::Function)
            Result.push_back(Narrow(I));
    if (Result.size() > MaxFunctions) Invalid("too many symbol functions");
    return Result;
}

} // namespace

std::vector<std::uint8_t> SerializeSymbols(const CompiledProgram& Input,
                                           std::size_t MaxBytes) {
    auto ProgramBytes = SerializeProgram(Input);
    auto Functions = FunctionIndices(*Input.Program);
    Writer Output(MaxBytes);
    Output.MagicBytes();
    Output.U16(FormatVersion);
    Output.U16(InstructionVersion);
    Output.U32(0); // Payload size, including the final checksum.
    Output.U32(Narrow(ProgramBytes.size()));
    Output.U64(Fingerprint(ProgramBytes));
    Output.U32(Narrow(Functions.size()));
    for (auto Index : Functions) {
        const auto& Function = *Input.Program->Constants[Index].Function;
        Output.U32(Index);
        Output.String(Function.DebugName);
        Output.U32(Narrow(Function.Locations.size()));
        for (const auto& Entry : Function.Locations) {
            Output.U32(Narrow(Entry.Pc));
            Output.U32(Narrow(Entry.Source.ByteOffset));
            Output.U32(Narrow(Entry.Source.Line));
            Output.U32(Narrow(Entry.Source.Column));
            Output.U8(Entry.Breakable ? 1 : 0);
        }
        Output.U32(Narrow(Function.LocalVariables.size()));
        for (const auto& Variable : Function.LocalVariables) {
            Output.U32(Variable.Slot);
            Output.U32(Narrow(Variable.StartPc));
            Output.U32(Narrow(Variable.EndPc));
            Output.String(Variable.Name);
        }
    }
    auto PayloadSize = Narrow(Output.Size() - 16 + 4);
    Output.PatchU32(12, PayloadSize);
    Output.U32(Checksum(Output.View()));
    return std::move(Output).Finish();
}

void AttachSymbols(CompiledProgram& Input, std::span<const std::uint8_t> Bytes,
                   std::size_t MaxBytes) {
    if (Bytes.size() > MaxBytes) Invalid("symbol size limit exceeded");
    if (Bytes.size() < 36) Invalid("truncated symbol artifact");
    auto Payload = Bytes.first(Bytes.size() - 4);
    Reader Tail(Bytes.last(4));
    if (Checksum(Payload) != Tail.U32()) Invalid("symbol checksum mismatch");
    Reader Source(Payload);
    for (auto Byte : Magic) if (Source.U8() != Byte) Invalid("invalid symbol magic");
    if (Source.U16() != FormatVersion) Invalid("unsupported symbol format version");
    if (Source.U16() != InstructionVersion) Invalid("unsupported symbol instruction version");
    if (Source.U32() != Bytes.size() - 16) Invalid("invalid symbol payload size");
    auto ProgramBytes = SerializeProgram(Input);
    if (Source.U32() != ProgramBytes.size() || Source.U64() != Fingerprint(ProgramBytes))
        Invalid("symbols do not match bytecode");
    auto Indices = FunctionIndices(*Input.Program);
    if (Source.U32() != Indices.size()) Invalid("symbol function count mismatch");
    struct FunctionSymbols {
        std::string Name;
        std::vector<InstructionLocation> Locations;
        std::vector<LocalVariableInfo> Locals;
    };
    std::vector<FunctionSymbols> Pending;
    Pending.reserve(Indices.size());
    for (auto Index : Indices) {
        if (Source.U32() != Index) Invalid("invalid symbol function index");
        auto& Symbols = Pending.emplace_back();
        Symbols.Name = Source.String();
        auto Count = Source.U32();
        if (Count > Input.Program->Constants[Index].Function->Code.size())
            Invalid("symbol location count exceeds code size");
        Symbols.Locations.reserve(Count);
        for (std::uint32_t I = 0; I < Count; ++I) {
            InstructionLocation Entry;
            Entry.Pc = Source.U32();
            Entry.Source.ByteOffset = Source.U32();
            Entry.Source.Line = Source.U32();
            Entry.Source.Column = Source.U32();
            auto Breakable = Source.U8();
            if (Breakable > 1) Invalid("invalid symbol breakable flag");
            Entry.Breakable = Breakable != 0;
            Symbols.Locations.push_back(Entry);
        }
        auto LocalCount = Source.U32();
        if (LocalCount > Input.Program->Constants[Index].Function->LocalCount)
            Invalid("symbol local variable count exceeds local slots");
        Symbols.Locals.reserve(LocalCount);
        for (std::uint32_t I = 0; I < LocalCount; ++I) {
            LocalVariableInfo Variable;
            Variable.Slot = Source.U32();
            Variable.StartPc = Source.U32();
            Variable.EndPc = Source.U32();
            Variable.Name = Source.String();
            Symbols.Locals.push_back(std::move(Variable));
        }
    }
    if (!Source.Done()) Invalid("trailing symbol data");

    // Validate a separate copy before modifying prototypes shared by callers.
    Module Trial = *Input.Program;
    for (std::size_t I = 0; I < Indices.size(); ++I) {
        auto Index = Indices[I];
        auto Prototype = std::make_shared<FunctionPrototype>(*Trial.Constants[Index].Function);
        Prototype->DebugName = std::move(Pending[I].Name);
        Prototype->Locations = std::move(Pending[I].Locations);
        Prototype->LocalVariables = std::move(Pending[I].Locals);
        Trial.Constants[Index].Function = std::move(Prototype);
    }
    Trial.Validate();
    for (auto Index : Indices)
        Input.Program->Constants[Index].Function = std::move(Trial.Constants[Index].Function);
}

} // namespace Feather
