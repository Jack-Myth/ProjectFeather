#include <Feather/Runtime.hpp>
#include "../Vm/Internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <queue>
#include <span>
#include <unordered_set>
#include <utility>

namespace Feather {
namespace {

constexpr std::uint32_t MaxEntries = 65'536;
constexpr std::uint32_t NoObject = std::numeric_limits<std::uint32_t>::max();

std::uint32_t SnapshotChecksum(std::span<const std::uint8_t> Bytes) {
    static const auto Table = [] {
        std::array<std::uint32_t, 256> Values{};
        for (std::uint32_t I = 0; I < Values.size(); ++I) {
            auto Crc = I;
            for (int Bit = 0; Bit < 8; ++Bit)
                Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320u : 0u);
            Values[I] = Crc;
        }
        return Values;
    }();
    std::uint32_t Crc = 0xffffffffu;
    for (auto Byte : Bytes) Crc = Table[(Crc ^ Byte) & 0xffu] ^ (Crc >> 8);
    return ~Crc;
}

[[noreturn]] void Invalid(const char* Message) { throw std::invalid_argument(Message); }

class Writer {
public:
    explicit Writer(std::size_t Limit) : Limit(Limit) {}
    void U8(std::uint8_t Input) { Ensure(1); Data.push_back(Input); }
    void U16(std::uint16_t Input) { for (unsigned I = 0; I < 2; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I))); }
    void U32(std::uint32_t Input) { for (unsigned I = 0; I < 4; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I))); }
    void U64(std::uint64_t Input) { for (unsigned I = 0; I < 8; ++I) U8(static_cast<std::uint8_t>(Input >> (8 * I))); }
    void Raw(std::span<const std::uint8_t> Input) {
        Ensure(Input.size()); Data.insert(Data.end(), Input.begin(), Input.end());
    }
    void Blob(std::span<const std::uint8_t> Input) {
        if (Input.size() > std::numeric_limits<std::uint32_t>::max()) Invalid("snapshot field too large");
        U32(static_cast<std::uint32_t>(Input.size())); Raw(Input);
    }
    void String(const std::string& Input) {
        Blob({reinterpret_cast<const std::uint8_t*>(Input.data()), Input.size()});
    }
    const std::vector<std::uint8_t>& Bytes() const { return Data; }
    std::vector<std::uint8_t> Finish() && { return std::move(Data); }
private:
    void Ensure(std::size_t Count) const {
        if (Count > Limit || Data.size() > Limit - Count) Invalid("snapshot size limit exceeded");
    }
    std::size_t Limit;
    std::vector<std::uint8_t> Data;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> Input) : Input(Input) {}
    std::uint8_t U8() { Require(1); return Input[Position++]; }
    std::uint16_t U16() { std::uint16_t R = 0; for (unsigned I = 0; I < 2; ++I) R |= std::uint16_t(U8()) << (8 * I); return R; }
    std::uint32_t U32() { std::uint32_t R = 0; for (unsigned I = 0; I < 4; ++I) R |= std::uint32_t(U8()) << (8 * I); return R; }
    std::uint64_t U64() { std::uint64_t R = 0; for (unsigned I = 0; I < 8; ++I) R |= std::uint64_t(U8()) << (8 * I); return R; }
    std::span<const std::uint8_t> Raw(std::size_t Count) {
        Require(Count); auto Result = Input.subspan(Position, Count); Position += Count; return Result;
    }
    std::span<const std::uint8_t> Blob() { return Raw(U32()); }
    std::string String() {
        auto Data = Blob();
        std::string Result(reinterpret_cast<const char*>(Data.data()), Data.size());
        (void)Value::String(Result);
        return Result;
    }
    bool Done() const { return Position == Input.size(); }
private:
    void Require(std::size_t Count) const {
        if (Count > Input.size() - Position) Invalid("truncated snapshot");
    }
    std::span<const std::uint8_t> Input;
    std::size_t Position = 0;
};

void WritePrimitive(Writer& Output, const Value& Input) {
    switch (Input.GetType()) {
    case ValueType::Null: Output.U8(0); break;
    case ValueType::Bool: Output.U8(Input.AsBool() ? 2 : 1); break;
    case ValueType::Number:
        Output.U8(3); Output.U64(std::bit_cast<std::uint64_t>(Input.AsNumber())); break;
    case ValueType::String: Output.U8(4); Output.String(Input.AsString()); break;
    case ValueType::Object: Invalid("object is not a primitive snapshot value");
    }
}

std::vector<std::uint8_t> ModuleImage(const Module& Program, std::size_t Limit) {
    Writer Output(Limit);
    if (Program.Constants.size() > MaxEntries) Invalid("too many module constants");
    Output.U32(static_cast<std::uint32_t>(Program.Constants.size()));
    for (const auto& Item : Program.Constants) {
        switch (Item.Type) {
        case Constant::Kind::Number:
            Output.U8(0); Output.U64(std::bit_cast<std::uint64_t>(Item.Numeric)); break;
        case Constant::Kind::String: Output.U8(1); Output.String(Item.Text); break;
        case Constant::Kind::Function:
            Output.U8(2);
            if (!Item.Function) Invalid("null function in module");
            Output.U32(Item.Function->ParameterCount);
            Output.U32(Item.Function->LocalCount);
            Output.U32(static_cast<std::uint32_t>(Item.Function->Defaults.size()));
            for (const auto& Default : Item.Function->Defaults) {
                Output.U8(Default ? 1 : 0);
                if (Default) WritePrimitive(Output, *Default);
            }
            Output.Blob(Item.Function->Code);
            break;
        }
    }
    return std::move(Output).Finish();
}

std::vector<std::uint8_t> ModuleSetImage(
    const Module& Primary, const std::vector<std::shared_ptr<ModuleInstance>>& Loaded,
    std::size_t Limit) {
    Writer Output(Limit);
    if (Loaded.size() >= MaxEntries) Invalid("too many snapshot modules");
    Output.U32(static_cast<std::uint32_t>(Loaded.size() + 1));
    Output.String("");
    Output.Blob(ModuleImage(Primary, Limit));
    Output.U32(0);
    for (const auto& Instance : Loaded) {
        Output.String(Instance->Id);
        if (Instance->Identity.empty()) Output.Blob(ModuleImage(*Instance->Program, Limit));
        else Output.Blob(Instance->Identity);
        std::vector<std::string> Exports(Instance->Exports.begin(), Instance->Exports.end());
        std::sort(Exports.begin(), Exports.end());
        Output.U32(static_cast<std::uint32_t>(Exports.size()));
        for (const auto& Name : Exports) Output.String(Name);
    }
    return std::move(Output).Finish();
}

struct FunctionId {
    std::uint32_t Module = 0;
    std::uint32_t Constant = 0;
    bool operator==(const FunctionId&) const = default;
};
std::uint64_t FunctionKey(FunctionId Input) {
    return (std::uint64_t(Input.Module) << 32) | Input.Constant;
}

struct EncodedValue {
    std::uint8_t Tag = 0;
    double Number = 0;
    std::string Text;
    std::uint32_t ObjectId = 0;
};

EncodedValue ReadValue(Reader& Input, std::uint32_t ObjectCount) {
    EncodedValue Result; Result.Tag = Input.U8();
    switch (Result.Tag) {
    case 0: case 1: case 2: break;
    case 3: Result.Number = std::bit_cast<double>(Input.U64()); break;
    case 4: Result.Text = Input.String(); break;
    case 5:
        Result.ObjectId = Input.U32();
        if (Result.ObjectId >= ObjectCount) Invalid("snapshot object reference out of range");
        break;
    default: Invalid("invalid snapshot value tag");
    }
    return Result;
}
void WriteValue(Writer& Output, const Value& Input,
                const std::unordered_map<Object*, std::uint32_t>& Ids) {
    if (Input.GetType() != ValueType::Object) { WritePrimitive(Output, Input); return; }
    auto Found = Ids.find(Input.AsObject());
    if (Found == Ids.end()) Invalid("snapshot graph is incomplete");
    Output.U8(5); Output.U32(Found->second);
}

struct ObjectRecord {
    std::uint8_t Kind = 0;
    std::uint32_t Meta = NoObject;
    std::vector<std::pair<EncodedValue, EncodedValue>> Members;
    std::uint32_t Function = 0;
    std::uint32_t Module = 0;
    std::string Message;
    HostSnapshotRecord Host;
    std::vector<std::pair<std::string, EncodedValue>> Visible;
};
struct FrameRecord {
    std::uint32_t Module = 0, Function = 0, Pc = 0;
    std::vector<EncodedValue> Locals, Stack;
};

std::uint32_t CodeU32(const std::vector<std::uint8_t>& Code, std::size_t At) {
    std::uint32_t Result = 0;
    for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(Code[At + I]) << (8 * I);
    return Result;
}
std::vector<int> StackDepths(const FunctionPrototype& Function) {
    const auto& Code = Function.Code;
    std::vector<int> Heights(Code.size(), -1);
    std::queue<std::size_t> Work;
    Heights[0] = 0; Work.push(0);
    while (!Work.empty()) {
        auto Pc = Work.front(); Work.pop();
        auto Instruction = static_cast<Op>(Code[Pc]);
        std::size_t Width = 1;
        int Consumes = 0, Produces = 0;
        switch (Instruction) {
        case Op::Const: case Op::GetLocal: case Op::GetGlobal:
            Width = 5; Produces = 1; break;
        case Op::SetLocal: case Op::SetGlobal:
            Width = 5; Consumes = 1; Produces = 1; break;
        case Op::Jump: Width = 5; break;
        case Op::JumpIf: Width = 5; Consumes = 1; break;
        case Op::Call:
            Width = 3;
            Consumes = 1 + Code[Pc + 1] + (int(Code[Pc + 2]) << 8);
            Produces = 1; break;
        case Op::Null: case Op::True: case Op::False: case Op::NewObject:
            Produces = 1; break;
        case Op::Pop: case Op::Return: Consumes = 1; break;
        case Op::Negate: Consumes = 1; Produces = 1; break;
        case Op::GetMember: case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        case Op::Equal: case Op::Less: Consumes = 2; Produces = 1; break;
        case Op::SetMember: Consumes = 3; Produces = 1; break;
        }
        int NextDepth = Heights[Pc] - Consumes + Produces;
        auto Enqueue = [&](std::size_t Next) {
            if (Next >= Code.size()) return;
            if (Heights[Next] < 0) { Heights[Next] = NextDepth; Work.push(Next); }
        };
        if (Instruction == Op::Jump || Instruction == Op::JumpIf) {
            auto Offset = std::bit_cast<std::int32_t>(CodeU32(Code, Pc + 1));
            auto Destination = static_cast<std::int64_t>(Pc + Width) + Offset;
            Enqueue(static_cast<std::size_t>(Destination));
        }
        if (Instruction != Op::Jump && Instruction != Op::Return) Enqueue(Pc + Width);
    }
    return Heights;
}

} // namespace

std::vector<std::uint8_t> Vm::CaptureSnapshot(SnapshotHostCodec* Codec,
                                               std::size_t MaxBytes) const {
    if (SnapshotBusy) throw std::logic_error("snapshot operation already active");
    if (ActiveRuns != 1 || ActiveNativeCalls != 1 || !ActiveExecution ||
        !ActiveExecution->PendingCallResult || ActiveExecution->Frames.empty())
        throw std::logic_error("snapshot requires one pending script Call and native depth one");
    if (MaxBytes == 0) Invalid("snapshot size limit is zero");
    SnapshotBusy = true;
    struct ResetBusy { bool& Flag; ~ResetBusy() { Flag = false; } } Reset{SnapshotBusy};

    std::vector<std::shared_ptr<ModuleInstance>> Ordered;
    Ordered.reserve(LoadedModules.size());
    for (const auto& [_, Instance] : LoadedModules) {
        if (Instance->Initialization == ModuleInstance::State::Initializing)
            throw std::logic_error("snapshot cannot capture an initializing module");
        Ordered.push_back(Instance);
    }
    std::sort(Ordered.begin(), Ordered.end(), [](const auto& A, const auto& B) {
        return A->Id < B->Id;
    });
    std::unordered_map<const Module*, std::uint32_t> ModuleIds{{Program.get(), 0}};
    for (std::size_t I = 0; I < Ordered.size(); ++I)
        ModuleIds.emplace(Ordered[I]->Program.get(), static_cast<std::uint32_t>(I + 1));

    std::unordered_map<Object*, FunctionId> FunctionIds;
    for (std::size_t I = 0; I < Functions.size(); ++I)
        if (Functions[I]) FunctionIds.emplace(Functions[I].get(), FunctionId{0, static_cast<std::uint32_t>(I)});
    for (std::size_t M = 0; M < Ordered.size(); ++M)
        for (std::size_t I = 0; I < Ordered[M]->Functions.size(); ++I)
            if (Ordered[M]->Functions[I])
                FunctionIds.emplace(Ordered[M]->Functions[I].get(),
                                    FunctionId{static_cast<std::uint32_t>(M + 1),
                                               static_cast<std::uint32_t>(I)});
    std::vector<Value> Objects;
    std::unordered_map<Object*, std::uint32_t> Ids;
    auto Add = [&](const Value& Input) {
        if (Input.GetType() != ValueType::Object) return;
        Object* Pointer = Input.AsObject();
        if (Input.IsScriptObject() && !OwnsScript(Input.AsScriptObject()))
            Invalid("foreign ScriptObject in snapshot graph");
        if (Ids.contains(Pointer)) return;
        if (Objects.size() >= MaxEntries) Invalid("too many snapshot objects");
        Ids.emplace(Pointer, static_cast<std::uint32_t>(Objects.size()));
        Objects.push_back(Input);
    };
    Add(Value::FromScript(RootMetaObject));
    for (const auto& [_, Input] : Globals) Add(Input);
    for (const auto& Instance : Ordered) {
        for (const auto& [_, Input] : Instance->Globals) Add(Input);
        Add(Instance->InitializationError);
        Add(Value::FromObject(Instance->Namespace));
    }
    for (const auto& FrameValue : ActiveExecution->Frames) {
        Add(Value::FromObject(FrameValue.Function));
        for (const auto& Input : FrameValue.Locals) Add(Input);
        for (const auto& Input : FrameValue.Stack) Add(Input);
    }
    for (std::size_t I = 0; I < Objects.size(); ++I) {
        const auto& Input = Objects[I];
        if (Input.IsScriptObject()) {
            auto Script = Input.AsScriptObject();
            if (Script->MetaObject) Add(Value::FromScript(Script->MetaObject));
            for (const auto& [_, Member] : Script->Members) Add(Member);
        } else {
            auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject());
            if (!Native) Invalid("unsupported snapshot object");
            for (const auto& [_, Member] : Native->GcVisibleMembers) Add(Member);
        }
    }

    Writer ObjectBytes(MaxBytes);
    ObjectBytes.U32(static_cast<std::uint32_t>(Objects.size()));
    for (const auto& Input : Objects) {
        if (Input.IsScriptObject()) {
            auto Script = Input.AsScriptObject();
            ObjectBytes.U8(0);
            ObjectBytes.U32(Script->MetaObject ? Ids.at(Script->MetaObject) : NoObject);
            if (Script->Members.size() > MaxEntries) Invalid("too many script members");
            ObjectBytes.U32(static_cast<std::uint32_t>(Script->Members.size()));
            for (const auto& [Key, Member] : Script->Members) {
                auto KeyValue = Key.Type == ValueType::String ?
                    Value::String(Key.Text) : Value::Number(Key.Number);
                WriteValue(ObjectBytes, KeyValue, Ids);
                WriteValue(ObjectBytes, Member, Ids);
            }
            continue;
        }
        auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject());
        switch (Native->GetObjectType()) {
        case ObjectType::Function: {
            ObjectBytes.U8(1);
            auto Found = FunctionIds.find(Native.get());
            if (Found == FunctionIds.end()) Invalid("foreign function in snapshot graph");
            ObjectBytes.U32(Found->second.Module);
            ObjectBytes.U32(Found->second.Constant); break;
        }
        case ObjectType::Error: {
            ObjectBytes.U8(2);
            auto ErrorValue = std::dynamic_pointer_cast<ErrorObject>(Native);
            if (!ErrorValue) Invalid("invalid ErrorObject in snapshot graph");
            ObjectBytes.String(ErrorValue->GetMessage()); break;
        }
        case ObjectType::Host: {
            if (auto Namespace = std::dynamic_pointer_cast<ModuleNamespace>(Native)) {
                auto Found = LoadedModules.find(Namespace->GetModuleId());
                if (Found == LoadedModules.end() || Found->second->Namespace.get() != Native.get())
                    Invalid("foreign module namespace in snapshot graph");
                ObjectBytes.U8(4);
                ObjectBytes.U32(ModuleIds.at(Found->second->Program.get()));
                break;
            }
            ObjectBytes.U8(3);
            if (!Codec) Invalid("host snapshot codec required");
            auto Record = Codec->Encode(Native);
            if (Record.TypeId.empty()) Invalid("empty host snapshot type ID");
            (void)Value::String(Record.TypeId);
            ObjectBytes.String(Record.TypeId);
            ObjectBytes.Blob(Record.Payload); break;
        }
        case ObjectType::Script: Invalid("invalid native ScriptObject");
        }
        if (Native->GcVisibleMembers.size() > MaxEntries) Invalid("too many native visible members");
        ObjectBytes.U32(static_cast<std::uint32_t>(Native->GcVisibleMembers.size()));
        for (const auto& [Name, Member] : Native->GcVisibleMembers) {
            ObjectBytes.String(Name);
            WriteValue(ObjectBytes, Member, Ids);
        }
    }

    Writer GlobalBytes(MaxBytes);
    GlobalBytes.U32(static_cast<std::uint32_t>(Ordered.size() + 1));
    if (Globals.size() > MaxEntries) Invalid("too many snapshot globals");
    GlobalBytes.U32(static_cast<std::uint32_t>(Globals.size()));
    for (const auto& [Name, Input] : Globals) {
        GlobalBytes.String(Name);
        WriteValue(GlobalBytes, Input, Ids);
    }
    for (const auto& Instance : Ordered) {
        GlobalBytes.U8(Instance->Initialization == ModuleInstance::State::Loaded ? 0 :
                       Instance->Initialization == ModuleInstance::State::Initialized ? 1 : 2);
        WriteValue(GlobalBytes, Instance->InitializationError, Ids);
        if (Instance->Globals.size() > MaxEntries) Invalid("too many snapshot globals");
        GlobalBytes.U32(static_cast<std::uint32_t>(Instance->Globals.size()));
        for (const auto& [Name, Input] : Instance->Globals) {
            GlobalBytes.String(Name);
            WriteValue(GlobalBytes, Input, Ids);
        }
    }

    Writer FrameBytes(MaxBytes);
    if (ActiveExecution->Frames.size() > 1024) Invalid("too many snapshot frames");
    FrameBytes.U32(static_cast<std::uint32_t>(ActiveExecution->Frames.size()));
    FrameBytes.U8(1); // The top frame awaits the result of the native Call.
    for (const auto& Current : ActiveExecution->Frames) {
        auto Found = FunctionIds.find(Current.Function.get());
        if (Found == FunctionIds.end()) Invalid("foreign frame function");
        if (Current.Pc > std::numeric_limits<std::uint32_t>::max() ||
            Current.Locals.size() > MaxEntries || Current.Stack.size() > MaxEntries)
            Invalid("snapshot frame limit exceeded");
        FrameBytes.U32(Found->second.Module);
        FrameBytes.U32(Found->second.Constant);
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Pc));
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Locals.size()));
        for (const auto& Input : Current.Locals) WriteValue(FrameBytes, Input, Ids);
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Stack.size()));
        for (const auto& Input : Current.Stack) WriteValue(FrameBytes, Input, Ids);
    }

    Writer Output(MaxBytes);
    Output.U8('F'); Output.U8('T'); Output.U8('H'); Output.U8('S');
    Output.U16(3); Output.U16(0);
    auto ModuleBytes = ModuleSetImage(*Program, Ordered, MaxBytes);
    Output.Blob(ModuleBytes);
    Output.Blob(ObjectBytes.Bytes());
    Output.Blob(GlobalBytes.Bytes());
    Output.Blob(FrameBytes.Bytes());
    Output.U32(SnapshotChecksum(std::span<const std::uint8_t>(Output.Bytes())));
    return std::move(Output).Finish();
}

Value Vm::ResumeSnapshot(const std::vector<std::uint8_t>& Bytes,
                         SnapshotHostCodec* Codec, std::size_t MaxBytes) {
    if (SnapshotBusy || HasRun || ActiveRuns != 0 || ActiveNativeCalls != 0 || ActiveExecution ||
        !HostRoots.empty() || ScriptHeap.size() != 1 || !RootMetaObject->Members.empty())
        throw std::logic_error("snapshot restore requires a fresh idle VM");
    if (Bytes.size() > MaxBytes) Invalid("snapshot size limit exceeded");
    if (Bytes.size() < 12) Invalid("truncated snapshot");
    SnapshotBusy = true;
    struct ResetBusy { bool& Flag; ~ResetBusy() { Flag = false; } } Reset{SnapshotBusy};
    Program->Validate();
    std::vector<std::shared_ptr<ModuleInstance>> Ordered;
    Ordered.reserve(LoadedModules.size());
    for (const auto& [_, Instance] : LoadedModules) {
        Instance->Program->Validate();
        if (Instance->Initialization != ModuleInstance::State::Loaded)
            throw std::logic_error("snapshot restore requires fresh loaded modules");
        Ordered.push_back(Instance);
    }
    std::sort(Ordered.begin(), Ordered.end(), [](const auto& A, const auto& B) {
        return A->Id < B->Id;
    });
    auto FunctionAt = [&](std::uint32_t ModuleId, std::uint32_t ConstantId)
        -> std::shared_ptr<Object> {
        if (ModuleId == 0)
            return ConstantId < Functions.size() ? Functions[ConstantId] : nullptr;
        if (ModuleId > Ordered.size()) return nullptr;
        const auto& List = Ordered[ModuleId - 1]->Functions;
        return ConstantId < List.size() ? List[ConstantId] : nullptr;
    };
    auto PrototypeAt = [&](std::uint32_t ModuleId, std::uint32_t ConstantId)
        -> std::shared_ptr<FunctionPrototype> {
        if (!FunctionAt(ModuleId, ConstantId)) return nullptr;
        const auto& Owner = ModuleId == 0 ? Program : Ordered[ModuleId - 1]->Program;
        return Owner->Constants[ConstantId].Function;
    };

    std::span<const std::uint8_t> Content(Bytes.data(), Bytes.size() - 4);
    Reader Input(Content);
    if (Input.U8() != 'F' || Input.U8() != 'T' || Input.U8() != 'H' || Input.U8() != 'S')
        Invalid("invalid snapshot magic");
    auto Major = Input.U16(), Minor = Input.U16();
    if (Major != 3 || Minor != 0) Invalid("unsupported snapshot version");
    Reader ChecksumInput(std::span<const std::uint8_t>(Bytes.data() + Bytes.size() - 4, 4));
    if (ChecksumInput.U32() != SnapshotChecksum(Content)) Invalid("snapshot checksum mismatch");
    auto SavedModule = Input.Blob();
    auto CurrentModule = ModuleSetImage(*Program, Ordered, MaxBytes);
    if (SavedModule.size() != CurrentModule.size() ||
        !std::equal(SavedModule.begin(), SavedModule.end(), CurrentModule.begin()))
        Invalid("snapshot module mismatch");
    auto ObjectSection = Input.Blob();
    auto GlobalSection = Input.Blob();
    auto FrameSection = Input.Blob();
    if (!Input.Done()) Invalid("trailing snapshot bytes");

    Reader ObjectInput(ObjectSection);
    auto ObjectCount = ObjectInput.U32();
    if (ObjectCount == 0 || ObjectCount > MaxEntries) Invalid("invalid snapshot object count");
    // Every object record occupies at least a kind byte and two u32 fields.
    if (ObjectCount > (ObjectSection.size() - sizeof(std::uint32_t)) / 9)
        Invalid("snapshot object count exceeds section size");
    std::vector<ObjectRecord> Records(ObjectCount);
    std::unordered_set<std::uint64_t> SeenFunctions;
    std::unordered_set<std::uint32_t> SeenNamespaces;
    std::size_t ScriptCount = 0;
    for (std::uint32_t I = 0; I < ObjectCount; ++I) {
        auto& Record = Records[I];
        Record.Kind = ObjectInput.U8();
        if (Record.Kind == 0) {
            ++ScriptCount;
            if (ScriptCount > ScriptObjectLimit) Invalid("snapshot ScriptObject limit exceeded");
            Record.Meta = ObjectInput.U32();
            auto Count = ObjectInput.U32();
            if (Count > MaxEntries) Invalid("too many snapshot script members");
            Record.Members.reserve(Count);
            for (std::uint32_t J = 0; J < Count; ++J) {
                auto Key = ReadValue(ObjectInput, ObjectCount);
                if ((Key.Tag != 3 && Key.Tag != 4) ||
                    (Key.Tag == 3 && std::isnan(Key.Number))) Invalid("invalid snapshot member key");
                Record.Members.emplace_back(std::move(Key), ReadValue(ObjectInput, ObjectCount));
            }
        } else if (Record.Kind == 1) {
            Record.Module = ObjectInput.U32();
            Record.Function = ObjectInput.U32();
            if (!FunctionAt(Record.Module, Record.Function) ||
                !SeenFunctions.insert(FunctionKey({Record.Module, Record.Function})).second)
                Invalid("invalid or duplicate snapshot function");
        } else if (Record.Kind == 2) {
            Record.Message = ObjectInput.String();
        } else if (Record.Kind == 3) {
            Record.Host.TypeId = ObjectInput.String();
            if (Record.Host.TypeId.empty()) Invalid("empty host snapshot type ID");
            auto Payload = ObjectInput.Blob();
            Record.Host.Payload.assign(Payload.begin(), Payload.end());
            if (!Codec) Invalid("host snapshot codec required");
        } else if (Record.Kind == 4) {
            Record.Module = ObjectInput.U32();
            if (Record.Module == 0 || Record.Module > Ordered.size() ||
                !SeenNamespaces.insert(Record.Module).second)
                Invalid("invalid or duplicate snapshot module namespace");
        } else Invalid("invalid snapshot object kind");
        if (Record.Kind != 0) {
            auto Count = ObjectInput.U32();
            if (Count > MaxEntries) Invalid("too many snapshot native members");
            std::unordered_set<std::string> Names;
            Record.Visible.reserve(Count);
            for (std::uint32_t J = 0; J < Count; ++J) {
                auto Name = ObjectInput.String();
                if (!Names.insert(Name).second) Invalid("duplicate native member name");
                Record.Visible.emplace_back(std::move(Name), ReadValue(ObjectInput, ObjectCount));
            }
        }
    }
    if (!ObjectInput.Done() || Records[0].Kind != 0 || Records[0].Meta != NoObject)
        Invalid("invalid snapshot root object");
    for (std::size_t I = 1; I < Records.size(); ++I)
        if (Records[I].Kind == 0 &&
            (Records[I].Meta >= Records.size() || Records[Records[I].Meta].Kind != 0))
            Invalid("invalid snapshot MetaObject reference");

    Reader GlobalInput(GlobalSection);
    if (GlobalInput.U32() != Ordered.size() + 1)
        Invalid("snapshot global module count mismatch");
    std::vector<std::vector<std::pair<std::string, EncodedValue>>> SavedGlobals(Ordered.size() + 1);
    std::vector<std::uint8_t> SavedStates(Ordered.size() + 1, 0);
    std::vector<EncodedValue> SavedFailures(Ordered.size() + 1);
    for (std::size_t ModuleId = 0; ModuleId < SavedGlobals.size(); ++ModuleId) {
        if (ModuleId != 0) {
            SavedStates[ModuleId] = GlobalInput.U8();
            if (SavedStates[ModuleId] > 2) Invalid("invalid snapshot module state");
            SavedFailures[ModuleId] = ReadValue(GlobalInput, ObjectCount);
            if (SavedStates[ModuleId] == 2 &&
                (SavedFailures[ModuleId].Tag != 5 ||
                 Records[SavedFailures[ModuleId].ObjectId].Kind != 2))
                Invalid("failed module needs an Error value");
            if (SavedStates[ModuleId] != 2 && SavedFailures[ModuleId].Tag != 0)
                Invalid("unexpected module initialization Error");
        }
        auto GlobalCount = GlobalInput.U32();
        if (GlobalCount > MaxEntries) Invalid("too many snapshot globals");
        SavedGlobals[ModuleId].reserve(GlobalCount);
        std::unordered_set<std::string> GlobalNames;
        for (std::uint32_t I = 0; I < GlobalCount; ++I) {
            auto Name = GlobalInput.String();
            if (!GlobalNames.insert(Name).second) Invalid("duplicate snapshot global");
            SavedGlobals[ModuleId].emplace_back(std::move(Name), ReadValue(GlobalInput, ObjectCount));
        }
    }
    if (!GlobalInput.Done()) Invalid("trailing global data");

    Reader FrameInput(FrameSection);
    auto FrameCount = FrameInput.U32();
    if (FrameCount == 0 || FrameCount > 1024 || FrameInput.U8() != 1)
        Invalid("invalid snapshot continuation");
    std::vector<FrameRecord> SavedFrames(FrameCount);
    std::unordered_map<std::uint64_t, std::vector<int>> DepthCache;
    for (auto& Record : SavedFrames) {
        Record.Module = FrameInput.U32();
        Record.Function = FrameInput.U32();
        Record.Pc = FrameInput.U32();
        auto Body = PrototypeAt(Record.Module, Record.Function);
        if (!Body)
            Invalid("invalid snapshot frame function");
        auto [FoundDepths, _] = DepthCache.try_emplace(FunctionKey({Record.Module, Record.Function}));
        if (FoundDepths->second.empty()) FoundDepths->second = StackDepths(*Body);
        auto ExpectedDepth = Record.Pc < FoundDepths->second.size() ?
            FoundDepths->second[Record.Pc] : -1;
        if (ExpectedDepth < 1) Invalid("snapshot frame is not awaiting a result");
        auto LocalCount = FrameInput.U32();
        if (LocalCount != Body->LocalCount || LocalCount > MaxEntries)
            Invalid("invalid snapshot local count");
        Record.Locals.reserve(LocalCount);
        for (std::uint32_t I = 0; I < LocalCount; ++I)
            Record.Locals.push_back(ReadValue(FrameInput, ObjectCount));
        auto StackCount = FrameInput.U32();
        if (StackCount > MaxEntries || StackCount != static_cast<std::uint32_t>(ExpectedDepth - 1))
            Invalid("invalid snapshot stack count");
        Record.Stack.reserve(StackCount);
        for (std::uint32_t I = 0; I < StackCount; ++I)
            Record.Stack.push_back(ReadValue(FrameInput, ObjectCount));
    }
    if (!FrameInput.Done()) Invalid("trailing frame data");

    std::vector<Value> Decoded(ObjectCount);
    ExecutionState Execution;
    std::vector<std::unordered_map<std::string, Value>> NewGlobals(Ordered.size() + 1);
    std::vector<Value> NewErrors(Ordered.size());
    auto PreviousNativeRegistry = NativeRegistry;
    auto PreviousGlobals = Globals;
    std::unordered_set<NativeObject*> DecodedHosts;
    try {
        NativeRegistry.clear();
        Decoded[0] = Value::FromScript(RootMetaObject);
        for (std::size_t I = 1; I < Records.size(); ++I) {
            const auto& Record = Records[I];
            switch (Record.Kind) {
            case 0: Decoded[I] = Value::FromScript(CreateScriptObject()); break;
            case 1: Decoded[I] = Value::FromObject(FunctionAt(Record.Module, Record.Function)); break;
            case 2: Decoded[I] = Value::FromObject(std::make_shared<ErrorObject>(Record.Message)); break;
            case 3: {
                auto Native = Codec->Decode(*this, Record.Host);
                if (!Native || Native->GetObjectType() != ObjectType::Host)
                    Invalid("host snapshot codec returned invalid object");
                if (!DecodedHosts.insert(Native.get()).second)
                    Invalid("host snapshot codec reused object identity");
                Decoded[I] = Value::FromObject(Native);
                RegisterNativeObject(Native);
                break;
            }
            case 4:
                Decoded[I] = Value::FromObject(Ordered[Record.Module - 1]->Namespace);
                break;
            default: Invalid("invalid snapshot object kind");
            }
        }
        auto Materialize = [&](const EncodedValue& InputValue) -> Value {
            switch (InputValue.Tag) {
            case 0: return {};
            case 1: return Value::Bool(false);
            case 2: return Value::Bool(true);
            case 3: return Value::Number(InputValue.Number);
            case 4: return Value::String(InputValue.Text);
            case 5: return Decoded[InputValue.ObjectId];
            default: Invalid("invalid snapshot value tag");
            }
        };
        for (std::size_t I = 0; I < Records.size(); ++I) {
            const auto& Record = Records[I];
            if (Record.Kind == 0) {
                auto Script = Decoded[I].AsScriptObject();
                Script->MetaObject = Record.Meta == NoObject ? nullptr : Decoded[Record.Meta].AsScriptObject();
                for (const auto& [Key, Member] : Record.Members)
                    Script->SetRaw(Materialize(Key), Materialize(Member));
            } else {
                auto Native = std::dynamic_pointer_cast<NativeObject>(Decoded[I].AsNativeObject());
                for (const auto& [Name, Member] : Record.Visible)
                    Native->GcVisibleMembers.emplace(Name, Materialize(Member));
            }
        }
        for (std::size_t ModuleId = 0; ModuleId < SavedGlobals.size(); ++ModuleId)
            for (const auto& [Name, InputValue] : SavedGlobals[ModuleId])
                NewGlobals[ModuleId].emplace(Name, Materialize(InputValue));
        for (std::size_t I = 0; I < Ordered.size(); ++I)
            if (SavedStates[I + 1] == 2)
                NewErrors[I] = Decoded[SavedFailures[I + 1].ObjectId];
        Execution.Frames.reserve(SavedFrames.size());
        for (const auto& Record : SavedFrames) {
            Frame Next;
            Next.Function = std::static_pointer_cast<FunctionObject>(
                FunctionAt(Record.Module, Record.Function));
            Next.Pc = Record.Pc;
            for (const auto& InputValue : Record.Locals) Next.Locals.push_back(Materialize(InputValue));
            for (const auto& InputValue : Record.Stack) Next.Stack.push_back(Materialize(InputValue));
            Execution.Frames.push_back(std::move(Next));
        }
        Execution.Frames.back().Stack.push_back(Value::Bool(true));
    } catch (...) {
        RootMetaObject->Members.clear();
        for (auto& Input : Decoded)
            if (Input.GetType() == ValueType::Object && !Input.IsScriptObject())
                if (auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject()))
                    Native->GcVisibleMembers.clear();
        for (auto& Function : Functions)
            if (auto Native = std::dynamic_pointer_cast<NativeObject>(Function))
                Native->GcVisibleMembers.clear();
        for (const auto& Instance : Ordered)
            for (auto& Function : Instance->Functions)
                if (auto Native = std::dynamic_pointer_cast<NativeObject>(Function))
                    Native->GcVisibleMembers.clear();
        Globals = std::move(PreviousGlobals);
        NativeRegistry = std::move(PreviousNativeRegistry);
        ScriptHeap.resize(1);
        throw;
    }
    Globals.swap(NewGlobals[0]);
    for (std::size_t I = 0; I < Ordered.size(); ++I) {
        auto& Instance = *Ordered[I];
        Instance.Globals.swap(NewGlobals[I + 1]);
        if (SavedStates[I + 1] == 2) {
            Instance.InitializationError = std::move(NewErrors[I]);
            Instance.Initialization = ModuleInstance::State::Failed;
        } else Instance.Initialization = SavedStates[I + 1] == 1 ?
            ModuleInstance::State::Initialized : ModuleInstance::State::Loaded;
    }
    SnapshotBusy = false;
    return Execute(Execution);
}

} // namespace Feather
