#include <Feather/Runtime.hpp>
#include "../Vm/Internal.hpp"

#include <algorithm>
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
    std::string Message;
    HostSnapshotRecord Host;
    std::vector<std::pair<std::string, EncodedValue>> Visible;
};
struct FrameRecord {
    std::uint32_t Function = 0, Pc = 0;
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

    std::unordered_map<Object*, std::uint32_t> FunctionIds;
    for (std::size_t I = 0; I < Functions.size(); ++I)
        if (Functions[I]) FunctionIds.emplace(Functions[I].get(), static_cast<std::uint32_t>(I));
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
            ObjectBytes.U32(Found->second); break;
        }
        case ObjectType::Error: {
            ObjectBytes.U8(2);
            auto ErrorValue = std::dynamic_pointer_cast<ErrorObject>(Native);
            if (!ErrorValue) Invalid("invalid ErrorObject in snapshot graph");
            ObjectBytes.String(ErrorValue->GetMessage()); break;
        }
        case ObjectType::Host: {
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
    if (Globals.size() > MaxEntries) Invalid("too many snapshot globals");
    GlobalBytes.U32(static_cast<std::uint32_t>(Globals.size()));
    for (const auto& [Name, Input] : Globals) {
        GlobalBytes.String(Name);
        WriteValue(GlobalBytes, Input, Ids);
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
        FrameBytes.U32(Found->second);
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Pc));
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Locals.size()));
        for (const auto& Input : Current.Locals) WriteValue(FrameBytes, Input, Ids);
        FrameBytes.U32(static_cast<std::uint32_t>(Current.Stack.size()));
        for (const auto& Input : Current.Stack) WriteValue(FrameBytes, Input, Ids);
    }

    Writer Output(MaxBytes);
    Output.U8('F'); Output.U8('T'); Output.U8('H'); Output.U8('S');
    Output.U16(1); Output.U16(0);
    auto ModuleBytes = ModuleImage(*Program, MaxBytes);
    Output.Blob(ModuleBytes);
    Output.Blob(ObjectBytes.Bytes());
    Output.Blob(GlobalBytes.Bytes());
    Output.Blob(FrameBytes.Bytes());
    return std::move(Output).Finish();
}

Value Vm::ResumeSnapshot(const std::vector<std::uint8_t>& Bytes,
                         SnapshotHostCodec* Codec, std::size_t MaxBytes) {
    if (SnapshotBusy || HasRun || ActiveRuns != 0 || ActiveNativeCalls != 0 || ActiveExecution ||
        !HostRoots.empty() || ScriptHeap.size() != 1 || !RootMetaObject->Members.empty())
        throw std::logic_error("snapshot restore requires a fresh idle VM");
    if (Bytes.size() > MaxBytes) Invalid("snapshot size limit exceeded");
    SnapshotBusy = true;
    struct ResetBusy { bool& Flag; ~ResetBusy() { Flag = false; } } Reset{SnapshotBusy};
    Program->Validate();

    Reader Input(Bytes);
    if (Input.U8() != 'F' || Input.U8() != 'T' || Input.U8() != 'H' || Input.U8() != 'S')
        Invalid("invalid snapshot magic");
    auto Major = Input.U16(), Minor = Input.U16();
    if (Major != 1 || Minor != 0) Invalid("unsupported snapshot version");
    auto SavedModule = Input.Blob();
    auto CurrentModule = ModuleImage(*Program, MaxBytes);
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
    std::vector<ObjectRecord> Records(ObjectCount);
    std::unordered_set<std::uint32_t> SeenFunctions;
    std::size_t ScriptCount = 0;
    for (std::uint32_t I = 0; I < ObjectCount; ++I) {
        auto& Record = Records[I];
        Record.Kind = ObjectInput.U8();
        if (Record.Kind == 0) {
            ++ScriptCount;
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
            Record.Function = ObjectInput.U32();
            if (Record.Function >= Functions.size() || !Functions[Record.Function] ||
                !SeenFunctions.insert(Record.Function).second)
                Invalid("invalid or duplicate snapshot function");
        } else if (Record.Kind == 2) {
            Record.Message = ObjectInput.String();
        } else if (Record.Kind == 3) {
            Record.Host.TypeId = ObjectInput.String();
            if (Record.Host.TypeId.empty()) Invalid("empty host snapshot type ID");
            auto Payload = ObjectInput.Blob();
            Record.Host.Payload.assign(Payload.begin(), Payload.end());
            if (!Codec) Invalid("host snapshot codec required");
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
    if (!ObjectInput.Done() || Records[0].Kind != 0 || Records[0].Meta != NoObject ||
        ScriptCount > ScriptObjectLimit) Invalid("invalid snapshot root object");
    for (std::size_t I = 1; I < Records.size(); ++I)
        if (Records[I].Kind == 0 &&
            (Records[I].Meta >= Records.size() || Records[Records[I].Meta].Kind != 0))
            Invalid("invalid snapshot MetaObject reference");

    Reader GlobalInput(GlobalSection);
    auto GlobalCount = GlobalInput.U32();
    if (GlobalCount > MaxEntries) Invalid("too many snapshot globals");
    std::vector<std::pair<std::string, EncodedValue>> SavedGlobals;
    SavedGlobals.reserve(GlobalCount);
    std::unordered_set<std::string> GlobalNames;
    for (std::uint32_t I = 0; I < GlobalCount; ++I) {
        auto Name = GlobalInput.String();
        if (!GlobalNames.insert(Name).second) Invalid("duplicate snapshot global");
        SavedGlobals.emplace_back(std::move(Name), ReadValue(GlobalInput, ObjectCount));
    }
    if (!GlobalInput.Done()) Invalid("trailing global data");

    Reader FrameInput(FrameSection);
    auto FrameCount = FrameInput.U32();
    if (FrameCount == 0 || FrameCount > 1024 || FrameInput.U8() != 1)
        Invalid("invalid snapshot continuation");
    std::vector<FrameRecord> SavedFrames(FrameCount);
    std::unordered_map<std::uint32_t, std::vector<int>> DepthCache;
    for (auto& Record : SavedFrames) {
        Record.Function = FrameInput.U32();
        Record.Pc = FrameInput.U32();
        if (Record.Function >= Functions.size() || !Functions[Record.Function])
            Invalid("invalid snapshot frame function");
        const auto& Body = Program->Constants[Record.Function].Function;
        auto [FoundDepths, _] = DepthCache.try_emplace(Record.Function);
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
    std::unordered_map<std::string, Value> NewGlobals;
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
            case 1: Decoded[I] = Value::FromObject(Functions[Record.Function]); break;
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
        for (const auto& [Name, InputValue] : SavedGlobals)
            NewGlobals.emplace(Name, Materialize(InputValue));
        Execution.Frames.reserve(SavedFrames.size());
        for (const auto& Record : SavedFrames) {
            Frame Next;
            Next.Function = std::static_pointer_cast<FunctionObject>(Functions[Record.Function]);
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
        Globals = std::move(PreviousGlobals);
        NativeRegistry = std::move(PreviousNativeRegistry);
        ScriptHeap.resize(1);
        throw;
    }
    Globals.swap(NewGlobals);
    SnapshotBusy = false;
    return Execute(Execution);
}

} // namespace Feather
