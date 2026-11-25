#include <Feather/Runtime.hpp>
#include "Internal.hpp"
#include "Optimize.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Feather {
namespace {
constexpr std::size_t MaxOperandStackSize = 65'536;

void InitializeParameterLocals(Frame& Next) {
    const auto& Body = Next.Function->GetBody();
    Next.Locals.resize(Body.LocalCount);
    for (std::size_t I = 0; I < Body.ParameterCount; ++I) {
        if (I < Next.Arguments.size()) Next.Locals[I] = Next.Arguments[I];
        else if (Body.Defaults[I]) Next.Locals[I] = *Body.Defaults[I];
    }
}
void InitializeFrame(Frame& Next, std::shared_ptr<FunctionObject> Function, const std::vector<Value>& Args) {
    Next.Function = std::move(Function);
    Next.Pc = 0;
    Next.Arguments = Args;
    InitializeParameterLocals(Next);
}
void InitializeFrameFromStack(Frame& Next, std::shared_ptr<FunctionObject> Function,
                              std::vector<Value>& Stack, std::size_t Count) {
    const auto& Body = Function->GetBody();
    const auto Start = Stack.size() - Count;
    Next.Function = std::move(Function);
    Next.Pc = 0;
    Next.Arguments.assign(Stack.begin() + Start, Stack.end());
    Next.Locals.resize(Body.LocalCount);
    for (std::size_t I = 0; I < Body.ParameterCount; ++I) {
        if (I < Count) Next.Locals[I] = std::move(Stack[Start + I]);
        else if (Body.Defaults[I]) Next.Locals[I] = *Body.Defaults[I];
    }
    // The callee frame now owns the arguments.
    Stack.resize(Start - 1);
}
double ArithmeticResult(Op Instruction, double A, double B) {
    switch (Instruction) {
    case Op::Add: case Op::AddConst: return A + B;
    case Op::Sub: case Op::SubConst: return A - B;
    case Op::Mul: case Op::MulConst: return A * B;
    case Op::Div:
        // Preserve explicit IEEE-754 behavior with host divide-by-zero traps.
        if (B == 0) {
            if (A == 0 || std::isnan(A))
                return std::numeric_limits<double>::quiet_NaN();
            return std::copysign(std::numeric_limits<double>::infinity(), A * std::copysign(1.0, B));
        }
        return A / B;
    default: throw std::logic_error("invalid arithmetic opcode");
    }
}
Value NonNumericSum(const Value& Left, const Value& Right) {
    if (Left.GetType() == ValueType::String && Right.GetType() == ValueType::String)
        return Value::String(Left.AsString() + Right.AsString());
    return Value::Error("invalid operands");
}
struct ActiveRunGuard {
    explicit ActiveRunGuard(std::uint32_t& Count, std::function<void()> Finish = {})
        : Count(Count), Finish(std::move(Finish)) { ++Count; }
    ~ActiveRunGuard() { --Count; if (Finish) Finish(); }
    std::uint32_t& Count;
    std::function<void()> Finish;
};
// Core builtin type identities belong to this VM.
constexpr NativeModuleGuid CoreBuiltinsGuid{{0x46,0x65,0x61,0x74,0x68,0x65,0x72,0x00,
                                             0xb1,0x17,0x49,0x81,0x96,0x27,0xa5,0x01}};
class ArrayFunction final : public NativeObject {
public:
    ArrayFunction(NativeObjectType& Type, Vm& Machine) : NativeObject(Type), Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (!Arguments.empty()) return Value::Error("array expects no arguments");
        return Value::FromScript(Machine.CreateArray());
    }
private:
    Vm& Machine;
};

class GetTypeFunction final : public NativeObject {
public:
    explicit GetTypeFunction(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1) return Value::Error("get_type requires exactly one argument");
        const auto& Input = Arguments.front();
        switch (Input.GetType()) {
        case ValueType::Null: return Value::String("null");
        case ValueType::Bool: return Value::String("bool");
        case ValueType::Byte: return Value::String("byte");
        case ValueType::Number: return Value::String("number");
        case ValueType::String: return Value::String("string");
        case ValueType::Object:
            switch (Input.GetObjectType()) {
            case ObjectType::Script:
                return Value::String(Input.AsScriptObject()->IsArray() ? "Array" : "ScriptObject");
            case ObjectType::Function: return Value::String("Function");
            case ObjectType::Error: return Value::String("Error");
            case ObjectType::Host: return Value::String("NativeObject");
            }
        }
        return Value::Error("unrecognized value type");
    }
};
class ModulePathsFunction final : public NativeObject {
public:
    ModulePathsFunction(NativeObjectType& Type, Vm& Machine, bool Write)
        : NativeObject(Type), Machine(Machine), Write(Write) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (!Write) {
            if (!Arguments.empty()) return Value::Error("GetModulePaths expects no arguments");
            const auto& Paths = Machine.GetModulePaths();
            auto* List = Machine.CreateArray();
            for (const auto& Path : Paths) List->AppendArray(Value::String(Path));
            return Value::FromScript(List);
        }
        if (Arguments.size() != 1 || !Arguments[0].IsScriptObject())
            return Value::Error("SetModulePaths expects a list of strings");
        const auto* List = Arguments[0].AsScriptObject();
        if (!List->IsArray()) return Value::Error("SetModulePaths expects an array of strings");
        const auto Elements = List->GetArrayElements();
        if (Elements.size() > 65536) return Value::Error("too many module paths");
        std::vector<std::string> Paths;
        Paths.reserve(Elements.size());
        for (const auto& Entry : Elements) {
            if (Entry.GetType() != ValueType::String)
                return Value::Error("module path array entries must be strings");
            Paths.push_back(Entry.AsString());
        }
        try { Machine.SetModulePaths(std::move(Paths)); }
        catch (const std::invalid_argument& Failure) { return Value::Error(Failure.what()); }
        return Value::Bool(true);
    }
private:
    Vm& Machine;
    bool Write;
};

class FunctionArgumentsFunction final : public NativeObject {
public:
    FunctionArgumentsFunction(NativeObjectType& Type, ExecutionState* const& Execution, bool Count)
        : NativeObject(Type), Execution(Execution), Count(Count) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (!Execution || Execution->Frames.empty())
            return Value::Error("no active Feather function");
        const auto& Values = Execution->Frames.back().Arguments;
        if (Count) {
            if (!Arguments.empty()) return Value::Error("GetArgumentCount expects no arguments");
            return Value::Number(static_cast<double>(Values.size()));
        }
        if (Arguments.size() != 1 || !Arguments[0].IsNumeric())
            return Value::Error("GetArgument expects one numeric index");
        const auto Index = Arguments[0].AsNumber();
        if (!std::isfinite(Index) || Index < 0 || std::trunc(Index) != Index ||
            Index >= static_cast<double>(Values.size()))
            return Value::Error("function argument index out of range");
        return Values[static_cast<std::size_t>(Index)];
    }
private:
    ExecutionState* const& Execution;
    bool Count;
};

class InvokeFunction final : public NativeObject {
public:
    InvokeFunction(NativeObjectType& Type, Vm& Machine) : NativeObject(Type), Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        Value Target;
        std::vector<Value> Expanded;
        if (auto Failure = Expand(Arguments, Target, Expanded); Failure.IsError()) return Failure;
        return Machine.Call(std::move(Target), std::move(Expanded));
    }

    static Value Expand(const std::vector<Value>& Arguments, Value& Target, std::vector<Value>& Expanded) {
        if (Arguments.size() != 2 || !Arguments[1].IsScriptObject() ||
            !Arguments[1].AsScriptObject()->IsArray())
            return Value::Error("Invoke expects a callable and an array");
        const auto Elements = Arguments[1].AsScriptObject()->GetArrayElements();
        Target = Arguments[0];
        // Snapshot the values before replacing Arguments, which may alias Expanded.
        Expanded = std::vector<Value>(Elements.begin(), Elements.end());
        return {};
    }
private:
    Vm& Machine;
};

class SetMetaObjectFunction final : public NativeObject {
public:
    SetMetaObjectFunction(NativeObjectType& Type, Vm& Machine) : NativeObject(Type), Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 2 || !Arguments[0].IsScriptObject() || !Arguments[1].IsScriptObject() ||
            Arguments[0].AsScriptObject()->IsArray() || Arguments[1].AsScriptObject()->IsArray())
            return Value::Error("SetMetaObject expects two script objects");
        try { Machine.SetMetaObject(Arguments[0].AsScriptObject(), Arguments[1].AsScriptObject()); }
        catch (const std::invalid_argument& Failure) { return Value::Error(Failure.what()); }
        return Arguments[0];
    }
private:
    Vm& Machine;
};

class RawSetFunction final : public NativeObject {
public:
    explicit RawSetFunction(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 3 || !Arguments[0].IsScriptObject())
            return Value::Error("RawSet expects a script object, key and value");
        return Arguments[0].AsScriptObject()->SetRaw(Arguments[1], Arguments[2]);
    }
};

class VmModule final : public NativeObject {
public:
    VmModule(NativeObjectType& Type, Value SetPaths, Value GetPaths,
             Value GetArgc, Value GetArgv, Value Invoke, Value SetMeta, Value RawSet)
        : NativeObject(Type), SetPaths(std::move(SetPaths)), GetPaths(std::move(GetPaths)),
          GetArgc(std::move(GetArgc)), GetArgv(std::move(GetArgv)),
          Invoke(std::move(Invoke)), SetMeta(std::move(SetMeta)), RawSet(std::move(RawSet)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String) {
            if (Key.AsString() == "SetModulePaths") return SetPaths;
            if (Key.AsString() == "GetModulePaths") return GetPaths;
            if (Key.AsString() == "GetArgumentCount") return GetArgc;
            if (Key.AsString() == "GetArgument") return GetArgv;
            if (Key.AsString() == "Invoke") return Invoke;
            if (Key.AsString() == "SetMetaObject") return SetMeta;
            if (Key.AsString() == "RawSet") return RawSet;
        }
        return Value::Error("unknown vm module member");
    }
    std::vector<std::string> Paths;
private:
    Value SetPaths;
    Value GetPaths;
    Value GetArgc;
    Value GetArgv;
    Value Invoke;
    Value SetMeta;
    Value RawSet;
};

bool Equal(const Value& Left, const Value& Right) {
    if (Left.IsNumeric() && Right.IsNumeric()) return Left.AsNumber() == Right.AsNumber();
    if (Left.GetType() != Right.GetType()) return false;
    switch (Left.GetType()) {
    case ValueType::Null: return true;
    case ValueType::Bool: return Left.AsBool() == Right.AsBool();
    case ValueType::Number: return Left.AsNumber() == Right.AsNumber();
    case ValueType::Byte: return Left.AsByte() == Right.AsByte();
    case ValueType::String: return Left.AsString() == Right.AsString();
    case ValueType::Object: return Left.AsObject() == Right.AsObject();
    }
    return false;
}
std::uint32_t ReadU32(const std::vector<std::uint8_t>& Code, std::size_t& Pc) {
    const auto At = Pc;
    const auto Result = std::uint32_t(Code[At]) |
        (std::uint32_t(Code[At + 1]) << 8) |
        (std::uint32_t(Code[At + 2]) << 16) |
        (std::uint32_t(Code[At + 3]) << 24);
    Pc = At + 4;
    return Result;
}
class OptimizedFunction final : public FunctionObject {
public:
    OptimizedFunction(const std::shared_ptr<Module>& Owner, const std::shared_ptr<FunctionPrototype>& Body,
                      SlotPlan Plan) : FunctionObject(Owner, Body), Plan(std::move(Plan)) {}
    const SlotPlan Plan;
};

std::vector<Value> BindConstants(const std::shared_ptr<Module>& Program) {
    std::vector<Value> Values;
    Values.reserve(Program->Constants.size());
    for (const auto& Item : Program->Constants) {
        switch (Item.Type) {
        case Constant::Kind::Number: Values.push_back(Value::Number(Item.Numeric)); break;
        case Constant::Kind::Byte: Values.push_back(Value::Byte(Item.ByteValue)); break;
        case Constant::Kind::String: Values.push_back(Value::String(Item.Text)); break;
        case Constant::Kind::Function: {
            auto Plan = OptimizeFunction(*Item.Function, *Program);
            if (Plan.Blocks.empty() && Plan.Calls.empty())
                Values.push_back(Value::FromObject(std::make_shared<FunctionObject>(Program, Item.Function)));
            else
                Values.push_back(Value::FromObject(std::make_shared<OptimizedFunction>(Program, Item.Function, std::move(Plan))));
            break;
        }
        }
    }
    return Values;
}
const InstructionLocation* LocationEntryAt(const FunctionPrototype& Body, std::size_t Pc) {
    auto Found = std::lower_bound(Body.Locations.begin(), Body.Locations.end(), Pc,
        [](const InstructionLocation& Location, std::size_t Target) { return Location.Pc < Target; });
    return Found == Body.Locations.end() || Found->Pc != Pc ? nullptr : &*Found;
}
std::optional<SourceLocation> LocationAt(const FunctionPrototype& Body, std::size_t Pc) {
    auto* Found = LocationEntryAt(Body, Pc);
    return Found ? std::optional<SourceLocation>(Found->Source) : std::nullopt;
}

bool IsIdentifier(std::string_view Name) {
    if (Name.empty()) return false;
    auto Alpha = [](char C) { return (C >= 'A' && C <= 'Z') ||
                                      (C >= 'a' && C <= 'z') || C == '_'; };
    if (!Alpha(Name[0])) return false;
    for (char C : Name.substr(1))
        if (!Alpha(C) && (C < '0' || C > '9')) return false;
    return true;
}
} // namespace

class ScriptObject::ArrayMethod final : public NativeObject {
public:
    enum class Operation { Add, Remove, RemoveAt };

    ArrayMethod(ScriptObject& Receiver, Operation Action)
        : Receiver(&Receiver), Machine(Receiver.Owner),
          Lifetime(Machine->Lifetime), Action(Action) {
        // Bound methods trace their receiver only while the method is reachable.
        SetGcVisibleMember("receiver", Value::FromScript(&Receiver));
    }

    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1) return Value::Error("array method expects exactly one argument");
        if (Lifetime.expired() || !Machine->OwnsScript(Receiver))
            return Value::Error("array receiver is no longer available");

        return Invoke(*Receiver, Action, Arguments.front());
    }

    static std::optional<Operation> Find(std::string_view Name) {
        if (Name == "Add") return Operation::Add;
        if (Name == "Remove") return Operation::Remove;
        if (Name == "RemoveAt") return Operation::RemoveAt;
        return std::nullopt;
    }

    static Value Invoke(ScriptObject& Receiver, Operation Action, const Value& Input) {
        if (Action == Operation::Add) {
            Receiver.AppendArray(Input);
            return Value::Bool(true);
        }
        if (Action == Operation::RemoveAt) {
            auto Index = Receiver.GetArrayIndex(Input);
            if (!Index || !Receiver.RemoveArrayAt(*Index)) return Value::Error("invalid array index");
            return Value::Bool(true);
        }
        auto& Elements = *Receiver.Elements;
        auto Found = std::find_if(Elements.begin(), Elements.end(),
            [&](const Value& Entry) { return Equal(Entry, Input); });
        if (Found == Elements.end()) return Value::Bool(false);
        Elements.erase(Found);
        return Value::Bool(true);
    }

private:
    ScriptObject* Receiver;
    Vm* Machine;
    std::weak_ptr<int> Lifetime;
    Operation Action;
};

std::optional<Value> ScriptObject::GetArrayMethod(std::string_view Name) const {
    auto Action = ArrayMethod::Find(Name);
    if (!Action) return std::nullopt;
    return Value::FromObject(std::make_shared<ArrayMethod>(
        *const_cast<ScriptObject*>(this), *Action));
}

Value ModuleNamespace::GetMember(const Value& Key) {
    auto Instance = Owner.lock();
    if (!Instance) throw std::logic_error("module namespace has expired");
    if (Instance->Initialization != ModuleInstance::State::Initialized)
        return Instance->Initialization == ModuleInstance::State::Failed ?
            Instance->InitializationError : Value::Error("module is not initialized");
    if (Key.GetType() != ValueType::String || !Instance->Exports.contains(Key.AsString()))
        return Value::Error("unknown module export");
    auto Found = Instance->Globals.find(Key.AsString());
    return Found == Instance->Globals.end() ? Value::Error("module export is not initialized") : Found->second;
}

Value ModuleNamespace::SetMember(const Value&, const Value&) {
    return Value::Error("module exports are read-only");
}

std::string ModuleNamespace::GetModuleId() const {
    auto Instance = Owner.lock();
    if (!Instance) throw std::logic_error("module namespace has expired");
    return Instance->Id;
}

Vm::~Vm() = default;

void Vm::SetDebugController(std::shared_ptr<VmDebugController> Input) {
    if (ActiveRuns != 0 || ActiveNativeCalls != 0)
        throw std::logic_error("debug controller can only be replaced while the VM is idle");
    DebugController = std::move(Input);
}

std::size_t VmDebugContext::GetFrameCount() const {
    return Execution->Frames.size();
}

DebugFrameView VmDebugContext::GetFrame(std::size_t FrameId) const {
    if (FrameId >= Execution->Frames.size()) throw std::out_of_range("debug frame ID out of range");
    const auto& Item = Execution->Frames[Execution->Frames.size() - 1 - FrameId];
    auto Pc = FrameId == 0 && TopFramePc ? *TopFramePc : Item.Pc;
    const auto& Owner = Item.Function->GetOwner();
    std::string ModuleId;
    if (Owner != Machine->Program) {
        auto Instance = Machine->ModuleByProgram.find(Owner.get());
        if (Instance == Machine->ModuleByProgram.end())
            throw std::logic_error("debug frame has no module instance");
        ModuleId = Instance->second->Id;
    }
    std::uint32_t FunctionId = 0;
    bool FoundFunction = false;
    for (std::size_t I = 0; I < Owner->Constants.size(); ++I) {
        const auto& Constant = Owner->Constants[I];
        if (Constant.Type == Constant::Kind::Function &&
            Constant.Function.get() == &Item.Function->GetBody()) {
            FunctionId = static_cast<std::uint32_t>(I);
            FoundFunction = true;
            break;
        }
    }
    if (!FoundFunction) throw std::logic_error("debug frame function is not in its module");
    auto* Location = LocationEntryAt(Item.Function->GetBody(), Pc);
    auto Source = Location ? std::optional<SourceLocation>(Location->Source) : std::nullopt;
    if (Source) Source->ModuleId = std::move(ModuleId);
    const auto& Body = Item.Function->GetBody();
    return {FrameId, FunctionId, Body.DebugName, Pc, std::move(Source),
            !Location || Location->Breakable, Item.Locals, Item.Stack, Body.LocalVariables};
}

std::vector<DebugNamedValue> VmDebugContext::GetGlobals(std::size_t FrameId) const {
    (void)GetFrame(FrameId);
    const auto& Internal = Execution->Frames[Execution->Frames.size() - 1 - FrameId];
    const auto& Owner = Internal.Function->GetOwner();
    const auto* Values = &Machine->Globals;
    if (Owner != Machine->Program) {
        auto Instance = Machine->ModuleByProgram.find(Owner.get());
        if (Instance == Machine->ModuleByProgram.end())
            throw std::logic_error("debug frame has no module instance");
        Values = &Instance->second->Globals;
    }
    std::vector<DebugNamedValue> Result;
    Result.reserve(Values->size());
    for (const auto& [Name, Data] : *Values) Result.push_back({Name, Data});
    std::sort(Result.begin(), Result.end(),
              [](const auto& A, const auto& B) { return A.Name < B.Name; });
    return Result;
}

std::vector<DebugProperty> VmDebugContext::GetProperties(const Value& Input) const {
    Machine->ValidateOwnedValue(Input);
    if (Input.GetType() != ValueType::Object)
        throw std::invalid_argument("debug properties require an object");
    std::vector<DebugProperty> Result;
    if (auto* Script = dynamic_cast<ScriptObject*>(Input.AsObject())) {
        if (Script->IsArray()) {
            const auto Elements = Script->GetArrayElements();
            Result.reserve(Elements.size() + 1);
            Result.push_back({Value::String("Length"), Value::Number(static_cast<double>(Elements.size()))});
            for (std::size_t I = 0; I < Elements.size(); ++I)
                Result.push_back({Value::Number(static_cast<double>(I)), Elements[I]});
            return Result;
        }
        Result.reserve(Script->Members.size() + (Script->MetaObject ? 1 : 0));
        for (const auto& [Key, Data] : Script->Members) {
            auto PublicKey = Key.Type == ValueType::String ? Value::String(Key.Text) : Value::Number(Key.Number);
            Result.push_back({std::move(PublicKey), Data});
        }
        if (Script->MetaObject)
            Result.push_back({Value::String("[[MetaObject]]"), Value::FromScript(Script->MetaObject)});
        return Result;
    }
    auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject());
    if (!Native) return Result;
    if (auto Error = std::dynamic_pointer_cast<ErrorObject>(Native))
        Result.push_back({Value::String("message"), Value::String(Error->GetMessage())});
    for (const auto& [Name, Data] : Native->GcVisibleMembers)
        Result.push_back({Value::String(Name), Data});
    std::sort(Result.begin(), Result.end(), [](const auto& A, const auto& B) {
        return A.Key.AsString() < B.Key.AsString();
    });
    return Result;
}

Value VmDebugContext::Evaluate(std::size_t FrameId,
                               const std::shared_ptr<Module>& EvaluationProgram,
                               std::uint32_t FunctionConstant,
                               const std::vector<Value>& Arguments) const {
    return Machine->EvaluateDebugExpression(*Execution, FrameId, EvaluationProgram,
                                            FunctionConstant, Arguments);
}

void VmDebugContext::SetLocal(std::size_t FrameId, std::size_t Slot, Value Input) const {
    Machine->ValidateOwnedValue(Input);
    if (FrameId >= Execution->Frames.size()) throw std::out_of_range("debug frame ID out of range");
    auto& Frame = Execution->Frames[Execution->Frames.size() - 1 - FrameId];
    if (Slot >= Frame.Locals.size()) throw std::out_of_range("debug local slot out of range");
    Frame.Locals[Slot] = std::move(Input);
}

void VmDebugContext::SetStack(std::size_t FrameId, std::size_t Slot, Value Input) const {
    Machine->ValidateOwnedValue(Input);
    if (FrameId >= Execution->Frames.size()) throw std::out_of_range("debug frame ID out of range");
    auto& Frame = Execution->Frames[Execution->Frames.size() - 1 - FrameId];
    if (Slot >= Frame.Stack.size()) throw std::out_of_range("debug stack slot out of range");
    Frame.Stack[Slot] = std::move(Input);
}

void VmDebugContext::SetGlobal(std::size_t FrameId, std::string Name, Value Input) const {
    Machine->ValidateOwnedValue(Input);
    (void)Value::String(Name);
    if (FrameId >= Execution->Frames.size()) throw std::out_of_range("debug frame ID out of range");
    const auto& Frame = Execution->Frames[Execution->Frames.size() - 1 - FrameId];
    const auto& Owner = Frame.Function->GetOwner();
    if (Owner == Machine->Program)
        Machine->Globals.insert_or_assign(std::move(Name), std::move(Input));
    else {
        auto Found = Machine->ModuleByProgram.find(Owner.get());
        if (Found == Machine->ModuleByProgram.end())
            throw std::logic_error("debug frame has no module instance");
        Found->second->Globals.insert_or_assign(std::move(Name), std::move(Input));
    }
}

Value VmDebugContext::SetProperty(const Value& Target, const Value& Key, Value Input) const {
    Machine->ValidateOwnedValue(Target);
    Machine->ValidateOwnedValue(Input);
    if (Target.GetType() != ValueType::Object)
        throw std::invalid_argument("debug property target must be an object");
    Value Result;
    if (auto* Script = dynamic_cast<ScriptObject*>(Target.AsObject()))
        Result = Script->SetRaw(Key, Input);
    else if (auto* Native = dynamic_cast<NativeObject*>(Target.AsObject()))
        Result = Native->SetMember(Key, Input);
    else throw std::invalid_argument("debug property target has no writable members");
    if (Result.IsError()) {
        auto* ErrorValue = dynamic_cast<ErrorObject*>(Result.AsObject());
        throw std::invalid_argument(ErrorValue ? ErrorValue->GetMessage() : "property write failed");
    }
    return Result;
}

Vm::Vm(std::shared_ptr<Module> Input, std::size_t MaxScriptObjects,
       std::size_t MaxInstructionsPerInvocation)
    : ScriptObjectLimit(MaxScriptObjects), InstructionLimit(MaxInstructionsPerInvocation) {
    if (!Input) throw std::invalid_argument("null module");
    if (ScriptObjectLimit == 0) throw std::invalid_argument("ScriptObject limit must include RootMetaObject");
    if (InstructionLimit == 0) throw std::invalid_argument("instruction limit must be positive");
    Input->Validate();
    SourceProgram = Input;
    RootMetaObject = AllocateScriptObject(nullptr);
    UpdateCollectionThresholds();
    auto& Type = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "get_type");
    BuiltinGetType = Value::FromObject(Type.Create<GetTypeFunction>());
    Globals.emplace("get_type", BuiltinGetType);
    auto& ArrayType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "array");
    BuiltinArray = Value::FromObject(ArrayType.Create<ArrayFunction>(*this));
    Globals.emplace("array", BuiltinArray);
    auto& SetPathsType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "SetModulePaths");
    auto& GetPathsType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "GetModulePaths");
    auto SetPaths = Value::FromObject(SetPathsType.Create<ModulePathsFunction>(*this, true));
    auto GetPaths = Value::FromObject(GetPathsType.Create<ModulePathsFunction>(*this, false));
    auto& ArgcType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "GetArgumentCount");
    auto& ArgvType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "GetArgument");
    auto GetArgc = Value::FromObject(ArgcType.Create<FunctionArgumentsFunction>(ActiveExecution, true));
    auto GetArgv = Value::FromObject(ArgvType.Create<FunctionArgumentsFunction>(ActiveExecution, false));
    auto& InvokeType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "Invoke");
    auto Invoke = Value::FromObject(InvokeType.Create<InvokeFunction>(*this));
    auto& SetMetaType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "SetMetaObject");
    auto SetMeta = Value::FromObject(SetMetaType.Create<SetMetaObjectFunction>(*this));
    auto& RawSetType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "RawSet");
    auto& VmModuleType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "vm");
    BuiltinVmModule = Value::FromObject(VmModuleType.Create<VmModule>(
        std::move(SetPaths), std::move(GetPaths),
        std::move(GetArgc), std::move(GetArgv), std::move(Invoke), std::move(SetMeta),
        Value::FromObject(RawSetType.Create<RawSetFunction>())));
    // Freeze the mutable builder's module before any execution.
    Program = std::make_shared<Module>(*Input);
    for (auto& Item : Program->Constants) {
        if (Item.Type == Constant::Kind::Function)
            Item.Function = std::make_shared<FunctionPrototype>(*Item.Function);
    }
    ConstantValues = BindConstants(Program);
}

void Vm::LoadModule(std::string Id, std::shared_ptr<Module> Source,
                    std::vector<std::string> ExportNames,
                    std::span<const std::uint8_t> Identity) {
    if (Id.empty()) throw std::invalid_argument("loaded module ID must be nonempty");
    (void)Value::String(Id);
    if (!Source) throw std::invalid_argument("null loaded module");
    std::unordered_set<std::string> Exports;
    for (const auto& Name : ExportNames)
        if (!IsIdentifier(Name) || !Exports.insert(Name).second)
            throw std::invalid_argument("invalid loaded module export");
    if (auto Found = LoadedModules.find(Id); Found != LoadedModules.end()) {
        if (IsModuleBuiltFrom(Id, Source, Identity) && Found->second->Exports == Exports) return;
        throw std::invalid_argument("module ID already belongs to another program");
    }
    if (LoadedModules.size() >= 65'536) throw std::invalid_argument("too many loaded modules");
    Source->Validate();
    auto Instance = std::make_shared<ModuleInstance>();
    Instance->Globals.emplace("get_type", BuiltinGetType);
    Instance->Globals.emplace("array", BuiltinArray);
    Instance->Id = Id;
    Instance->Source = Source;
    if (!Identity.empty())
        Instance->Identity = std::make_shared<const std::vector<std::uint8_t>>(Identity.begin(), Identity.end());
    Instance->Program = std::make_shared<Module>(*Source);
    Instance->Exports = std::move(Exports);
    for (auto& Item : Instance->Program->Constants)
        if (Item.Type == Constant::Kind::Function)
            Item.Function = std::make_shared<FunctionPrototype>(*Item.Function);
    Instance->ConstantValues = BindConstants(Instance->Program);
    Instance->Namespace = std::make_shared<ModuleNamespace>(Instance);
    auto [Inserted, _] = LoadedModules.emplace(Id, Instance);
    try { ModuleByProgram.emplace(Instance->Program.get(), Instance); }
    catch (...) { LoadedModules.erase(Inserted); throw; }
}

bool Vm::IsModuleBuiltFrom(std::string_view Id, const std::shared_ptr<Module>& Source,
                           std::span<const std::uint8_t> Identity) const {
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) return false;
    const auto& Recorded = Found->second->Identity;
    if (Identity.empty()) return !Recorded && Found->second->Source == Source;
    return Recorded && std::equal(Identity.begin(), Identity.end(), Recorded->begin(), Recorded->end());
}

Value Vm::GetModuleGlobal(std::string_view Id, const std::string& Name) const {
    (void)Value::String(Name);
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    auto Global = Found->second->Globals.find(Name);
    return Global == Found->second->Globals.end() ? Value::Error("undefined global") : Global->second;
}

void Vm::SetModuleGlobal(std::string_view Id, std::string Name, Value Input) {
    (void)Value::String(Name);
    ValidateOwnedValue(Input);
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    Found->second->Globals.insert_or_assign(std::move(Name), std::move(Input));
}

Value Vm::GetModuleNamespace(std::string_view Id) const {
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    return Value::FromObject(Found->second->Namespace);
}

std::string Vm::GetActiveModuleId() const {
    if (!ActiveExecution || ActiveExecution->Frames.empty()) return {};
    const auto& Owner = ActiveExecution->Frames.back().Function->GetOwner();
    if (Owner == Program) return {};
    auto Found = ModuleByProgram.find(Owner.get());
    if (Found == ModuleByProgram.end()) throw std::logic_error("active function has no module instance");
    return Found->second->Id;
}

ScriptObject* Vm::AllocateScriptObject(ScriptObject* MetaObject) {
    MaybeCollectGarbage();
    if (ScriptHeap.size() >= ScriptObjectLimit) throw std::bad_alloc();
    auto NewObject = std::unique_ptr<ScriptObject>(new ScriptObject(this, MetaObject));
    auto* Result = NewObject.get();
    ScriptHeap.push_back(std::move(NewObject));
    try { ScriptObjects.insert(Result); }
    catch (...) { ScriptHeap.pop_back(); throw; }
    ++TotalAllocatedScriptObjects;
    if (ScriptHeap.size() >= NextCollectionObjectCount)
        CollectionPending = true;
    return Result;
}
ScriptObject* Vm::CreateScriptObject() { return AllocateScriptObject(RootMetaObject); }
ScriptObject* Vm::CreateArray() {
    auto Elements = std::make_unique<std::vector<Value>>();
    auto* Result = AllocateScriptObject(nullptr);
    Result->Elements = std::move(Elements);
    return Result;
}
ScriptObject* Vm::CreateMetaObject() { return CreateScriptObject(); }
bool Vm::OwnsScript(ScriptObject* Input) const {
    return ScriptObjects.contains(Input);
}
void Vm::SetMetaObject(ScriptObject* Target, ScriptObject* MetaObject) {
    if (!Target || !MetaObject || Target == RootMetaObject ||
        !OwnsScript(Target) || !OwnsScript(MetaObject))
        throw std::invalid_argument("invalid MetaObject replacement");
    Target->MetaObject = MetaObject;
}
Value Vm::GetGlobal(const std::string& Name) const {
    Value::String(Name); // Validate the host-supplied UTF-8 key.
    auto Found = Globals.find(Name);
    return Found == Globals.end() ? Value::Error("undefined global") : Found->second;
}
void Vm::ValidateOwnedValue(const Value& Input) const {
    if (Input.IsScriptObject() && !OwnsScript(Input.AsScriptObject()))
        throw std::invalid_argument("ScriptObject belongs to another VM");
    if (Input.GetType() == ValueType::Object &&
        Input.GetObjectType() == ObjectType::Function) {
        auto* Function = FunctionObject::FromObject(Input.AsObject());
        if (!Function || (Function->GetOwner() != Program &&
                          !ModuleByProgram.contains(Function->GetOwner().get())))
            throw std::invalid_argument("function belongs to another VM module");
    }
}
void Vm::SetGlobal(std::string Name, Value Input) {
    Value::String(Name);
    ValidateOwnedValue(Input);
    Globals.insert_or_assign(std::move(Name), std::move(Input));
}
void Vm::RegisterNativeFunction(std::string Name, std::shared_ptr<NativeObject> Input) {
    if (!Input || !Input->IsCallable()) throw std::invalid_argument("native function must be callable");
    Value::String(Name);
    RegisterNativeObject(Input);
    SetGlobal(std::move(Name), Value::FromObject(std::move(Input)));
}

RootHandle::RootHandle(Vm* InputOwner, std::weak_ptr<int> InputLifetime, std::uint64_t InputToken)
    : Owner(InputOwner), Lifetime(std::move(InputLifetime)), Token(InputToken) {}
RootHandle::RootHandle(RootHandle&& Other) noexcept
    : Owner(std::exchange(Other.Owner, nullptr)), Lifetime(std::move(Other.Lifetime)),
      Token(std::exchange(Other.Token, 0)) {}
RootHandle& RootHandle::operator=(RootHandle&& Other) noexcept {
    if (this != &Other) {
        if (Token && !Lifetime.expired()) Owner->RemoveFromRoot(Token);
        Owner = std::exchange(Other.Owner, nullptr);
        Lifetime = std::move(Other.Lifetime);
        Token = std::exchange(Other.Token, 0);
    }
    return *this;
}
RootHandle::~RootHandle() {
    if (Token && !Lifetime.expired()) Owner->RemoveFromRoot(Token);
}
Value RootHandle::Get() const {
    if (!Token || Lifetime.expired()) throw std::logic_error("RootHandle is invalid");
    return Owner->HostRoots.at(Token);
}
void RootHandle::Reset() {
    if (!Token) throw std::logic_error("RootHandle already released");
    if (!Lifetime.expired()) Owner->RemoveFromRoot(Token);
    Owner = nullptr;
    Token = 0;
    Lifetime.reset();
}
RootHandle Vm::AddToRoot(Value Input) {
    ValidateOwnedValue(Input);
    if (NextRootToken == 0) throw std::overflow_error("RootHandle token space exhausted");
    auto Token = NextRootToken++;
    HostRoots.emplace(Token, std::move(Input));
    return RootHandle(this, Lifetime, Token);
}
void Vm::RemoveFromRoot(std::uint64_t Token) {
    if (HostRoots.erase(Token) != 1) throw std::logic_error("RootHandle already released");
}
void Vm::RegisterNativeObjectType(std::shared_ptr<NativeObjectType> Input) {
    if (!Input) throw std::invalid_argument("null NativeObjectType");
    if (&Input->GetVm() != this)
        throw std::invalid_argument("NativeObjectType belongs to another VM");
    bool Any = false;
    for (auto Byte : Input->GetId().Module.Bytes) Any = Any || Byte != 0;
    if (!Any) throw std::invalid_argument("native module GUID is zero");
    if (NativeTypes.contains(Input->GetId()))
        throw std::invalid_argument("duplicate native object type");
    OwnedNativeTypes.push_back(Input);
    try { NativeTypes.emplace(Input->GetId(), Input.get()); }
    catch (...) {
        OwnedNativeTypes.pop_back();
        throw;
    }
}

NativeObjectType* Vm::FindNativeObjectType(const NativeTypeId& Id) const {
    auto Found = NativeTypes.find(Id);
    return Found == NativeTypes.end() ? nullptr : Found->second;
}
Value Vm::Run(std::uint32_t FunctionConstant, const std::vector<Value>& Arguments) {
    if (ActiveRuns == 0) { FaultLocation.reset(); FaultException = {}; FaultObject = nullptr; }
    if (FunctionConstant >= ConstantValues.size() || Program->Constants[FunctionConstant].Type != Constant::Kind::Function)
        throw std::invalid_argument("entry is not a function constant");
    for (const auto& Input : Arguments) ValidateOwnedValue(Input);
    ExecutionState Execution;
    InitializeFrame(Execution.Frames.Enter(),
        std::static_pointer_cast<FunctionObject>(ConstantValues[FunctionConstant].AsNativeObject()), Arguments);
    return Execute(Execution);
}

ScriptFunctionAddress Vm::GetFunctionAddress(const Value& Input) const {
    if (Input.GetType() != ValueType::Object || Input.GetObjectType() != ObjectType::Function)
        throw std::invalid_argument("expected a script function");
    ValidateOwnedValue(Input);
    const auto* Function = static_cast<const FunctionObject*>(Input.AsObject());
    const auto* Table = &ConstantValues;
    ScriptFunctionAddress Address{};
    if (Function->GetOwner() != Program) {
        const auto& Instance = *ModuleByProgram.at(Function->GetOwner().get());
        if (!Instance.Identity)
            throw std::invalid_argument("function module has no transferable program identity");
        Table = &Instance.ConstantValues;
        Address.ModuleId = Instance.Id;
        Address.ModuleIdentity = Instance.Identity;
    }
    auto Found = std::find_if(Table->begin(), Table->end(), [&](const Value& Entry) {
        return Entry.GetType() == ValueType::Object && Entry.AsObject() == Function;
    });
    if (Found == Table->end()) throw std::invalid_argument("function has no address in its module");
    Address.Constant = static_cast<std::uint32_t>(Found - Table->begin());
    return Address;
}

bool Vm::IsCallable(const Value& Input) const {
    Value Target = Input;
    for (unsigned Depth = 0; Depth < 64; ++Depth) {
        ValidateOwnedValue(Target);
        if (Target.GetType() != ValueType::Object) return false;
        if (Target.IsScriptObject()) {
            auto* Meta = Target.AsScriptObject()->GetMetaObject();
            auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
            if (!Method) return false;
            Target = *Method;
        } else {
            auto* Native = dynamic_cast<NativeObject*>(Target.AsObject());
            return Native && Native->IsCallable();
        }
    }
    return false;
}
Value Vm::Call(Value Target, std::vector<Value> Arguments) {
    if (ActiveRuns >= 64) throw std::runtime_error("VM reentrant call depth exceeded");
    std::vector<RootHandle> Roots;
    Roots.push_back(AddToRoot(Target));
    for (const auto& Argument : Arguments) Roots.push_back(AddToRoot(Argument));
    for (unsigned Depth = 0; Depth < 64; ++Depth) {
        ValidateOwnedValue(Target);
        if (Target.GetType() != ValueType::Object) return Value::Error("value is not callable");
        if (Target.IsScriptObject()) {
            auto* Meta = Target.AsScriptObject()->GetMetaObject();
            auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
            if (!Method) return Value::Error("__call is missing");
            Arguments.insert(Arguments.begin(), Target);
            Target = *Method;
            Roots.push_back(AddToRoot(Target));
            continue;
        }
        auto* Native = dynamic_cast<NativeObject*>(Target.AsObject());
        if (!Native || !Native->IsCallable()) return Value::Error("value is not callable");
        if (dynamic_cast<InvokeFunction*>(Native)) {
            if (Depth == 63) return Value::Error("Invoke forwarding limit exceeded");
            if (auto Failure = InvokeFunction::Expand(Arguments, Target, Arguments); Failure.IsError())
                return Failure;
            Roots.push_back(AddToRoot(Target));
            for (const auto& Argument : Arguments) Roots.push_back(AddToRoot(Argument));
            continue;
        }
        if (Native->HasFlag(Object::Flag::ScriptFunction)) {
            ExecutionState Execution;
            InitializeFrame(Execution.Frames.Enter(),
                std::static_pointer_cast<FunctionObject>(Target.AsNativeObject()), Arguments);
            return Execute(Execution);
        }
        ActiveRunGuard Guard(ActiveNativeCalls);
        auto Result = Native->Call(Arguments);
        ValidateOwnedValue(Result);
        return Result;
    }
    return Value::Error("MetaObject call cycle");
}

Value Vm::RunModule(std::string_view Id, std::uint32_t FunctionConstant,
                    const std::vector<Value>& Arguments) {
    if (ActiveRuns == 0) { FaultLocation.reset(); FaultException = {}; FaultObject = nullptr; }
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    const auto& Instance = *Found->second;
    if (FunctionConstant >= Instance.ConstantValues.size() || Instance.Program->Constants[FunctionConstant].Type != Constant::Kind::Function)
        throw std::invalid_argument("entry is not a function constant");
    for (const auto& Input : Arguments) ValidateOwnedValue(Input);
    ExecutionState Execution;
    InitializeFrame(Execution.Frames.Enter(),
        std::static_pointer_cast<FunctionObject>(Instance.ConstantValues[FunctionConstant].AsNativeObject()), Arguments);
    return Execute(Execution);
}

const std::vector<std::string>& Vm::GetModulePaths() const {
    return static_cast<VmModule*>(BuiltinVmModule.AsObject())->Paths;
}

void Vm::SetModulePaths(std::vector<std::string> Paths) {
    if (Paths.size() > 65536) throw std::invalid_argument("too many module paths");
    for (const auto& Path : Paths) {
        if (Path.empty() || Path.find('\0') != std::string::npos)
            throw std::invalid_argument("module paths must be non-empty strings without NUL");
        (void)Value::String(Path);
    }
    static_cast<VmModule*>(BuiltinVmModule.AsObject())->Paths = std::move(Paths);
}

Value Vm::EvaluateDebugExpression(ExecutionState& PausedExecution, std::size_t FrameId,
                                  const std::shared_ptr<Module>& EvaluationProgram,
                                  std::uint32_t FunctionConstant,
                                  const std::vector<Value>& Arguments) {
    if (!EvaluationProgram) throw std::invalid_argument("missing debug expression program");
    if (FrameId >= PausedExecution.Frames.size())
        throw std::out_of_range("debug frame ID out of range");
    EvaluationProgram->Validate();
    for (const auto& Input : Arguments) ValidateOwnedValue(Input);

    const auto& PausedFrame = PausedExecution.Frames[PausedExecution.Frames.size() - 1 - FrameId];
    const auto& PausedOwner = PausedFrame.Function->GetOwner();
    const std::unordered_map<std::string, Value>* PausedGlobals = &Globals;
    if (PausedOwner != Program) {
        auto Found = ModuleByProgram.find(PausedOwner.get());
        if (Found == ModuleByProgram.end()) throw std::logic_error("debug frame has no module instance");
        PausedGlobals = &Found->second->Globals;
    }

    auto Instance = std::make_shared<ModuleInstance>();
    Instance->Id = "<debug-expression>";
    Instance->Source = EvaluationProgram;
    Instance->Program = std::make_shared<Module>(*EvaluationProgram);
    Instance->Globals = *PausedGlobals;
    for (auto& Item : Instance->Program->Constants)
        if (Item.Type == Constant::Kind::Function)
            Item.Function = std::make_shared<FunctionPrototype>(*Item.Function);
    Instance->ConstantValues = BindConstants(Instance->Program);
    if (FunctionConstant >= Instance->ConstantValues.size() || Instance->Program->Constants[FunctionConstant].Type != Constant::Kind::Function)
        throw std::invalid_argument("debug expression entry is not a function constant");
    auto [Registration, Inserted] = ModuleByProgram.emplace(Instance->Program.get(), Instance);
    if (!Inserted) throw std::logic_error("debug expression module identity collision");
    struct EvaluationGuard {
        Vm& Machine;
        const Module* Program;
        explicit EvaluationGuard(Vm& Input, const Module* Program)
            : Machine(Input), Program(Program) { ++Machine.DebugCallbackSuppression; }
        ~EvaluationGuard() {
            --Machine.DebugCallbackSuppression;
            Machine.ModuleByProgram.erase(Program);
        }
    } Guard(*this, Instance->Program.get());

    ExecutionState Evaluation;
    InitializeFrame(Evaluation.Frames.Enter(),
        std::static_pointer_cast<FunctionObject>(Instance->ConstantValues[FunctionConstant].AsNativeObject()), Arguments);
    auto Result = Execute(Evaluation);
    if (Result.GetType() == ValueType::Object && Result.GetObjectType() == ObjectType::Function) {
        auto* Function = FunctionObject::FromObject(Result.AsObject());
        if (Function && Function->GetOwner() == Instance->Program)
            throw std::invalid_argument("debug expression cannot return a temporary function");
    }
    return Result;
}

Value Vm::InitializeModule(std::string_view Id, std::uint32_t Initializer) {
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    auto& Instance = *Found->second;
    if (Instance.Initialization == ModuleInstance::State::Initializing)
        throw std::runtime_error("circular module initialization");
    if (Instance.Initialization == ModuleInstance::State::Initialized) return {};
    if (Instance.Initialization == ModuleInstance::State::Failed)
        return Instance.InitializationError;
    if (Initializer >= Instance.ConstantValues.size() || Instance.Program->Constants[Initializer].Type != Constant::Kind::Function ||
        Instance.Program->Constants[Initializer].Function->ParameterCount != 0)
        throw std::invalid_argument("invalid loaded module initializer");
    Instance.Initialization = ModuleInstance::State::Initializing;
    try {
        auto Result = RunModule(Id, Initializer);
        if (Result.IsError()) {
            Instance.InitializationError = Result;
            Instance.Initialization = ModuleInstance::State::Failed;
        } else Instance.Initialization = ModuleInstance::State::Initialized;
        return Result;
    } catch (...) {
        Instance.Initialization = ModuleInstance::State::Loaded;
        throw;
    }
}

std::optional<SourceLocation> Vm::GetErrorLocation(const Value& Input) const {
    if (!Input.IsError()) return std::nullopt;
    auto* Error = dynamic_cast<ErrorObject*>(Input.AsObject());
    if (!Error) return std::nullopt;
    auto Found = ErrorLocations.find(Error);
    if (Found == ErrorLocations.end()) return std::nullopt;
    auto Owner = Found->second.Lifetime.lock();
    if (!Owner || Owner.get() != Error) return std::nullopt;
    return Found->second.Source;
}

// Keep uncommon dispatch handlers out of the interpreter loop, including in LTO builds.
#if defined(_MSC_VER)
#define FEATHER_VM_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define FEATHER_VM_NOINLINE __attribute__((noinline))
#else
#define FEATHER_VM_NOINLINE
#endif

FEATHER_VM_NOINLINE bool Vm::TryLoadLocalField(Frame& Current, Op Instruction, const Value& Receiver,
                                              const std::vector<Value>& Constants, std::size_t& FaultPc) {
    if (DebugController || !Receiver.IsScriptObject() || Current.Stack.size() > MaxOperandStackSize - 2)
        return false;
    auto* Object = Receiver.AsScriptObject();
    if (Object->IsArray()) {
        if (Instruction != ReadLocalField) return false;
        return TryCallArrayMethod(Current, *Object, Constants, FaultPc);
    }
    auto Resume = Current.Pc + 1;
    const auto& Key = Constants[ReadU32(Current.Function->GetBody().Code, Resume)];
    if (Instruction == PrepareLocalField) {
        // Keep the original receiver and key rooted across arbitrary RHS calls.
        Current.Stack.push_back(Receiver);
        Current.Stack.push_back(Key);
    } else {
        const auto Found = Object->Members.find(std::string_view(Key.AsString()));
        if (Found == Object->Members.end() || Found->second.IsError()) return false;
        Current.Stack.push_back(Found->second);
    }
    Current.Pc = Resume;
    return true;
}

bool Vm::TryCallArrayMethod(Frame& Current, ScriptObject& Receiver,
                            const std::vector<Value>& Constants, std::size_t& FaultPc) {
    // Only consume a read that immediately becomes a call. An extracted method
    // keeps its identity and owns a GC-visible receiver through the normal path.
    constexpr std::size_t SkippedInstructions = 3; // field, argument, call
    const auto& Function = *Current.Function;
    const auto& Code = Function.GetBody().Code;
    const auto FieldPc = Current.Pc + 1; // GetField operand
    constexpr std::size_t OperandBytes = 4;
    constexpr std::size_t CallBytes = 3;
    const auto ArgumentPc = FieldPc + OperandBytes;
    const auto CallPc = ArgumentPc + 1 + OperandBytes;
    if (CallPc > Code.size() || Code.size() - CallPc < CallBytes)
        return false;
    const auto ArgumentOp = static_cast<Op>(Code[ArgumentPc]);
    if ((ArgumentOp != Op::GetLocal && ArgumentOp != Op::Const) ||
        static_cast<Op>(Code[CallPc]) != Op::Call || Code[CallPc + 1] != 1 || Code[CallPc + 2] != 0)
        return false;
    if (CollectionPending || InstructionsRemaining < SkippedInstructions)
        return false;
    auto Pc = FieldPc;
    const auto& Key = Constants[ReadU32(Code, Pc)];
    auto Operation = ScriptObject::ArrayMethod::Find(Key.AsString());
    if (!Operation || !OwnsScript(&Receiver)) return false;
    Pc = ArgumentPc + 1;
    const Value* Argument;
    if (ArgumentOp == Op::GetLocal) {
        Argument = &Current.Locals[ReadU32(Code, Pc)];
        if (Argument->GetType() == ValueType::Object &&
            !(Argument->IsScriptObject() && OwnsScript(Argument->AsScriptObject())))
            return false; // Keep ownership and Error diagnostics on the generic path.
    } else if (ArgumentOp == Op::Const) {
        const auto Index = ReadU32(Code, Pc);
        const auto Kind = Function.GetOwner()->Constants[Index].Type;
        if (Kind != Constant::Kind::Number && Kind != Constant::Kind::Byte && Kind != Constant::Kind::String)
            return false; // Buffer constants, for example, have allocation side effects.
        Argument = &Constants[Index];
    } else {
        return false;
    }
    if (*Operation == ScriptObject::ArrayMethod::Operation::RemoveAt && !Receiver.GetArrayIndex(*Argument))
        return false; // Generic path attaches the Error to the original call site.
    FaultPc = Pc;
    InstructionsRemaining -= SkippedInstructions;
    Current.Pc = Pc + 3;
    ActiveRunGuard NativeGuard(ActiveNativeCalls);
    Current.Stack.push_back(ScriptObject::ArrayMethod::Invoke(Receiver, *Operation, *Argument));
    return true;
}

FEATHER_VM_NOINLINE bool Vm::TryLocalIndex(Frame& Current, Op Instruction, const Value& Receiver,
                                          const std::vector<Value>& Constants) {
    if (DebugController || !Receiver.IsScriptObject() || Current.Stack.size() > MaxOperandStackSize - 2)
        return false;
    auto* Array = Receiver.AsScriptObject();
    if (!Array->IsArray()) return false;
    const auto& Code = Current.Function->GetBody().Code;
    auto Pc = Current.Pc;
    const auto KeyOp = static_cast<Op>(Code[Pc++]);
    const auto Id = ReadU32(Code, Pc);
    const auto& Source = KeyOp == Op::GetLocal ? Current.Locals[Id] : Constants[Id];
    if (!Source.IsNumeric()) return false;
    const auto Adjustment = static_cast<Op>(Code[Pc]);
    Value Adjusted;
    const Value* Key = &Source;
    if (Adjustment == Op::SubConst || Adjustment == Op::AddConst) {
        // Replay byte arithmetic through its normal promotion path.
        if (Source.GetType() != ValueType::Number) return false;
        ++Pc;
        const auto Delta = Constants[ReadU32(Code, Pc)].AsNumber();
        Adjusted = Value::Number(Adjustment == Op::SubConst ? Source.AsNumber() - Delta : Source.AsNumber() + Delta);
        Key = &Adjusted;
    }
    if (Instruction == ReadLocalIndex) {
        const auto Index = Array->GetArrayIndex(*Key);
        if (!Index) return false;
        const auto& Result = (*Array->Elements)[*Index];
        if (Result.IsError()) return false;
        Current.Stack.push_back(Result);
        ++Pc; // GetMember
    } else {
        // Snapshot the key, but validate bounds at the store: the RHS can resize the array.
        Current.Stack.push_back(Receiver);
        Current.Stack.push_back(*Key);
    }
    Current.Pc = Pc;
    return true;
}

FEATHER_VM_NOINLINE bool Vm::TryStoreLocalMember(Frame& Current) {
    auto& Stack = Current.Stack;
    const auto Base = Stack.size() - 3;
    const auto& Receiver = Stack[Base];
    const auto& Key = Stack[Base + 1];
    auto& Input = Stack.back();
    if (DebugController || !Receiver.IsScriptObject() || Input.IsError())
        return false;
    auto* Object = Receiver.AsScriptObject();
    // All three operands are validated VM stack values.
    if (Object->IsArray()) {
        const auto Index = Object->GetArrayIndex(Key);
        if (!Index) return false;
        (*Object->Elements)[*Index] = Input;
    } else {
        if (Key.GetType() != ValueType::String) return false;
        auto Found = Object->Members.find(std::string_view(Key.AsString()));
        if (Found == Object->Members.end()) return false;
        Found->second = Input;
    }
    const auto& Code = Current.Function->GetBody().Code;
    if (Current.Pc < Code.size() && static_cast<Op>(Code[Current.Pc]) == Op::Pop) {
        ++Current.Pc;
        Stack.resize(Base);
    } else {
        Stack[Base] = std::move(Input);
        Stack.resize(Base + 1);
    }
    return true;
}

FEATHER_VM_NOINLINE bool Vm::TryEnterSlotCall(ExecutionState& Execution, const Value*& Target) {
    auto& Frames = Execution.Frames;
    auto& Caller = Frames.back();
    const auto& Function = static_cast<const OptimizedFunction&>(*Caller.Function);
    const auto& Call = Function.Plan.Calls[ReadU32(Function.GetBody().Code, Caller.Pc)];
    auto* Instance = Caller.Instance;
    if (Call.Global) {
        auto& Scope = Instance ? Instance->Globals : Globals;
        const auto Found = Scope.find(Function.GetOwner()->Constants[Call.Target].Text);
        if (Found == Scope.end()) return false;
        Target = &Found->second;
    } else {
        Target = &Caller.Locals[Call.Target];
    }

    const auto Count = Call.Arguments.size();
    if (DebugController || Frames.size() >= 1024 ||
        Count + 1 > MaxOperandStackSize - Caller.Stack.size() || Target->GetType() != ValueType::Object)
        return false;
    auto* Callee = FunctionObject::FromObject(Target->AsObject());
    if (!Callee || Callee->GetOwner() != Function.GetOwner()) return false;
    for (auto Slot : Call.Arguments) {
        const auto& Arg = Caller.Locals[Slot];
        // Replay loads that need native ownership checks or error-location tracking at their own PC.
        if (Arg.GetType() == ValueType::Object && !(Arg.IsScriptObject() && OwnsScript(Arg.AsScriptObject())))
            return false;
    }

    auto Owner = std::static_pointer_cast<FunctionObject>(Target->AsNativeObject());
    Caller.Pc = Call.ResumePc;
    auto& Next = Frames.Enter();
    const auto& Locals = Frames[Frames.size() - 2].Locals; // Enter can relocate Caller.
    Next.Function = std::move(Owner);
    Next.Pc = 0;
    Next.Instance = Instance;
    Next.Arguments.reserve(Count);
    for (auto Slot : Call.Arguments) Next.Arguments.push_back(Locals[Slot]);
    InitializeParameterLocals(Next);
    return true;
}

Value Vm::Execute(ExecutionState& Execution) {
    if (ActiveRuns == 0) {
        FaultLocation.reset();
        FaultException = {};
        FaultObject = nullptr;
        for (auto It = ErrorLocations.begin(); It != ErrorLocations.end(); ) {
            if (It->second.Lifetime.expired()) It = ErrorLocations.erase(It);
            else ++It;
        }
    }
    const bool OuterRun = ActiveRuns == 0;
    if (OuterRun) {
        InstructionsRemaining = InstructionLimit;
    }
    ActiveRunGuard Guard(ActiveRuns);
    auto* PreviousExecution = ActiveExecution;
    ActiveExecution = &Execution;
    struct RestoreExecution {
        ExecutionState*& Target;
        ExecutionState* Previous;
        ~RestoreExecution() { Target = Previous; }
    } Restore{ActiveExecution, PreviousExecution};
    auto& Frames = Execution.Frames;
    auto BindFrame = [&](Frame& Target) {
        const auto& Owner = Target.Function->GetOwner();
        Target.Instance = Owner == Program ? nullptr : ModuleByProgram.at(Owner.get()).get();
    };
    BindFrame(Frames.back());
    auto PushFrame = [&](const std::shared_ptr<FunctionObject>& Function,
                         const std::vector<Value>& Args) {
        if (Frames.size() >= 1024) throw std::runtime_error("VM call depth exceeded");
        InitializeFrame(Frames.Enter(), Function, Args);
        BindFrame(Frames.back());
    };
    const FunctionPrototype* FaultBody = nullptr;
    std::size_t FaultPc = 0;
    std::string_view FaultModuleId;
    try {
    while (!Frames.empty()) {
        MaybeCollectGarbage();
        if (DebugController && DebugCallbackSuppression == 0)
            ReachDebugSafePoint(Execution);
        Frame& Next = Frames.back();
        FaultBody = &Next.Function->GetBody();
        FaultPc = Next.Pc;
        // Bound constants retain the function across frame moves/pops.
        // Borrow its owner only for this instruction.
        const auto& FrameOwner = Next.Function->GetOwner();
        auto* FrameInstance = Next.Instance;
        FaultModuleId = FrameInstance ? std::string_view(FrameInstance->Id) : std::string_view{};
        const auto& FrameConstants = FrameInstance ? FrameInstance->ConstantValues : ConstantValues;
        auto& FrameGlobals = FrameInstance ? FrameInstance->Globals : Globals;
        if (InstructionsRemaining == 0)
            throw std::runtime_error("VM instruction budget exceeded");
        --InstructionsRemaining;
        Frame& Current = Next;
        const auto& Code = Current.Function->GetBody().Code;
        if (Current.Pc >= Code.size()) throw std::runtime_error("VM PC out of range");
        auto InstructionStart = Current.Pc;
        auto Instruction = static_cast<Op>(Code[Current.Pc++]);
        auto Pop = [&]() -> Value {
            if (Current.Stack.empty()) throw std::runtime_error("VM stack underflow");
            Value Result = std::move(Current.Stack.back());
            Current.Stack.pop_back();
            return Result;
        };
        auto Push = [&](Value Input,
                        std::optional<DebugErrorOrigin> DebugOrigin = std::nullopt) {
            ValidateOwnedValue(Input);
            if (Current.Stack.size() >= MaxOperandStackSize) throw std::runtime_error("VM stack limit exceeded");
            if (Input.IsError()) {
                auto* Error = dynamic_cast<ErrorObject*>(Input.AsObject());
                if (Error) {
                    auto Found = ErrorLocations.find(Error);
                    if (Found == ErrorLocations.end() || Found->second.Lifetime.expired()) {
                        if (auto Source = LocationAt(Current.Function->GetBody(), InstructionStart)) {
                            Source->ModuleId = FaultModuleId;
                            ErrorLocations.insert_or_assign(Error, ErrorOrigin{Input.AsNativeObject(), *Source});
                        }
                    }
                }
            }
            Current.Stack.push_back(std::move(Input));
            if (DebugOrigin && Current.Stack.back().IsError())
                ReachDebugError(Execution, Current.Stack.back(), *DebugOrigin, InstructionStart);
        };
        auto DispatchCall = [&](auto&& Self, Value Target, std::vector<Value> Args,
                                unsigned Depth, bool MetaCall) -> void {
            if (Depth >= 64) {
                Push(Value::Error("MetaObject call cycle"), DebugErrorOrigin::Operation); return;
            }
            if (Target.GetType() != ValueType::Object) {
                Push(Value::Error(MetaCall ? "metafunction is not callable" : "value is not callable"),
                     DebugErrorOrigin::Operation); return;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue)) {
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
                if (!Method) { Push(Value::Error("__call is missing"), DebugErrorOrigin::Operation); return; }
                Args.insert(Args.begin(), Target);
                Self(Self, *Method, std::move(Args), Depth + 1, true);
                return;
            }
            auto Native = dynamic_cast<NativeObject*>(ObjectValue);
            if (!Native || !Native->IsCallable()) {
                Push(Value::Error(MetaCall ? "metafunction is not callable" : "value is not callable"),
                     DebugErrorOrigin::Operation); return;
            }
            if (dynamic_cast<InvokeFunction*>(Native)) {
                if (Depth == 63) {
                    Push(Value::Error("Invoke forwarding limit exceeded"), DebugErrorOrigin::Operation);
                    return;
                }
                if (auto Failure = InvokeFunction::Expand(Args, Target, Args); Failure.IsError()) {
                    Push(std::move(Failure), DebugErrorOrigin::FunctionReturn);
                    return;
                }
                Self(Self, std::move(Target), std::move(Args), Depth + 1, false);
            } else if (Native->HasFlag(Object::Flag::ScriptFunction)) {
                auto Function = std::static_pointer_cast<FunctionObject>(Target.AsNativeObject());
                if (Function->GetOwner() != Program &&
                    !ModuleByProgram.contains(Function->GetOwner().get()))
                    throw std::invalid_argument("function belongs to another VM module");
                PushFrame(Function, Args);
            } else {
                ActiveRunGuard NativeGuard(ActiveNativeCalls);
                Push(Native->Call(Args), DebugErrorOrigin::FunctionReturn);
            }
        };
        switch (Instruction) {
        case Op::Const: {
            auto Id = ReadU32(Code, Current.Pc);
            Push(FrameConstants[Id]);
            break;
        }
        case Op::Null: Push({}); break;
        case Op::True: Push(Value::Bool(true)); break;
        case Op::False: Push(Value::Bool(false)); break;
        case Op::Pop: Pop(); break;
        case OptimizedLocal: {
            const auto& Plan = static_cast<const OptimizedFunction&>(*Current.Function).Plan;
            const auto& Block = Plan.Blocks[ReadU32(Code, Current.Pc)];
            const auto& Source = Current.Locals[Block.Source];
            if (!DebugController && Source.GetType() == ValueType::Number) {
                if (Block.Operation == SlotBlock::Kind::Arithmetic) {
                    if (Current.Stack.size() < MaxOperandStackSize &&
                        Current.Locals[Block.Operand].GetType() == ValueType::Number) {
                        double Result = Source.Data.Numeric;
                        for (std::size_t I = 0; I < Block.StepCount; ++I) {
                            const auto& Step = Plan.Steps[Block.FirstStep + I];
                            const double Right = FrameConstants[Step.Constant].Data.Numeric;
                            switch (Step.Operation) {
                            case Op::MulConst: Result *= Right; break;
                            case Op::AddConst: Result += Right; break;
                            case Op::SubConst: Result -= Right; break;
                            default: throw std::logic_error("invalid optimized arithmetic operation");
                            }
                        }
                        Current.Locals[Block.Operand].Data.Numeric = Result;
                        Current.Pc = Block.EndPc;
                        break;
                    }
                } else if (Current.Stack.size() <= MaxOperandStackSize - 2) {
                    const auto& Right = (Block.Operation == SlotBlock::Kind::CompareLocal ||
                                         Block.Operation == SlotBlock::Kind::BinaryLocal) ?
                        Current.Locals[Block.Operand] : FrameConstants[Block.Operand];
                    if (Right.GetType() == ValueType::Number) {
                        if (Block.Operation == SlotBlock::Kind::CompareLocal ||
                            Block.Operation == SlotBlock::Kind::CompareConstant) {
                            const bool Taken = Source.Data.Numeric < Right.Data.Numeric;
                            Current.Pc = Taken ? Block.EndPc : Block.FalsePc;
                            break;
                        }
                        auto& Destination = Current.Locals[Block.Destination];
                        if (Destination.GetType() == ValueType::Number) {
                            Destination.Data.Numeric = ArithmeticResult(Block.BinaryOp, Source.Data.Numeric,
                                                                       Right.Data.Numeric);
                            Current.Pc = Block.EndPc;
                            break;
                        }
                    }
                }
            }
            // Replay the original GetLocal; subsequent instructions still have their original PCs.
            Push(Source);
            break;
        }
        case CallFromSlots: {
            const Value* Target = nullptr;
            if (!TryEnterSlotCall(Execution, Target)) {
                if (Target) Push(*Target);
                else Push(Value::Error("undefined global"), DebugErrorOrigin::Operation);
            }
            break;
        }
        case Op::GetLocal: {
            const auto& Local = Current.Locals[ReadU32(Code, Current.Pc)];
            // Numbers need neither ownership validation nor error tracking.
            if (Local.GetType() == ValueType::Number && Current.Stack.size() < MaxOperandStackSize)
                Current.Stack.push_back(Local);
            else
                Push(Local);
            break;
        }
        case Op::StoreLocal: {
            auto Id = ReadU32(Code, Current.Pc);
            Current.Locals[Id] = std::move(Current.Stack.back());
            Current.Stack.pop_back();
            break;
        }
        case ReturnDestination: case Op::SetLocal: {
            auto Id = ReadU32(Code, Current.Pc);
            Current.Locals[Id] = Current.Stack.back(); break;
        }
        case Op::GetGlobal: {
            auto Id = ReadU32(Code, Current.Pc);
            auto Found = FrameGlobals.find(FrameOwner->Constants[Id].Text);
            if (Found == FrameGlobals.end())
                Push(Value::Error("undefined global"), DebugErrorOrigin::Operation);
            else Push(Found->second);
            break;
        }
        case Op::SetGlobal: {
            auto Id = ReadU32(Code, Current.Pc);
            FrameGlobals[FrameOwner->Constants[Id].Text] = Current.Stack.back(); break;
        }
        case Op::AddToLocal: case Op::AddConstToLocal: {
            const auto Slots = ReadU32(Code, Current.Pc);
            auto& Left = Current.Locals[Slots & 0xffff];
            const auto& Right = Instruction == Op::AddToLocal ?
                Current.Locals[Slots >> 16] : FrameConstants[Slots >> 16];
            if (Left.Type == ValueType::Number && Right.Type == ValueType::Number) {
                Left.Data.Numeric += Right.Data.Numeric;
            } else if (Left.IsNumeric() && Right.IsNumeric()) {
                Left = Value::Number(Left.AsNumber() + Right.AsNumber());
            } else {
                // Keep the operation result visible to the debugger before assigning it.
                Push(NonNumericSum(Left, Right), DebugErrorOrigin::Operation);
                Left = Pop();
            }
            break;
        }
        case Op::AddConst: case Op::SubConst: case Op::MulConst: {
            const auto Right = FrameConstants[ReadU32(Code, Current.Pc)].Data.Numeric;
            auto& Left = Current.Stack.back();
            if (Left.Type == ValueType::Number) {
                Left.Data.Numeric = ArithmeticResult(Instruction, Left.Data.Numeric, Right);
            } else if (Left.IsNumeric()) {
                Left = Value::Number(ArithmeticResult(Instruction, Left.AsNumber(), Right));
            } else {
                Pop();
                Push(Value::Error("invalid operands"), DebugErrorOrigin::Operation);
            }
            break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: {
            if (Current.Stack.size() < 2) throw std::runtime_error("VM stack underflow");
            auto& Left = Current.Stack[Current.Stack.size() - 2];
            const auto& Right = Current.Stack.back();
            if (Left.Type == ValueType::Number && Right.Type == ValueType::Number) {
                Left.Data.Numeric = ArithmeticResult(Instruction, Left.Data.Numeric, Right.Data.Numeric);
            } else if (Left.IsNumeric() && Right.IsNumeric()) {
                Left = Value::Number(ArithmeticResult(Instruction, Left.AsNumber(), Right.AsNumber()));
            } else {
                auto B = Pop();
                auto A = Pop();
                Push(Instruction == Op::Add ? NonNumericSum(A, B) : Value::Error("invalid operands"),
                     DebugErrorOrigin::Operation);
                break;
            }
            Current.Stack.pop_back();
            break;
        }
        case BranchEqual: case BranchLess: {
            Instruction = Instruction == BranchLess ? Op::Less : Op::Equal;
            if (!DebugController) {
                const auto Size = Current.Stack.size();
                const auto& Left = Current.Stack[Size - 2];
                const auto& Right = Current.Stack[Size - 1];
                if (Left.IsNumeric() && Right.IsNumeric()) {
                    const double A = Left.AsNumber(), B = Right.AsNumber();
                    const bool Taken = Instruction == Op::Less ? A < B : A == B;
                    Current.Stack.pop_back();
                    Current.Stack.pop_back();
                    Current.Pc += JumpSize; // JumpIf skips the unconditional jump when true.
                    if (Taken) Current.Pc += JumpSize;
                    else {
                        ++Current.Pc;
                        const auto Offset = std::bit_cast<std::int32_t>(ReadU32(Code, Current.Pc));
                        Current.Pc = static_cast<std::size_t>(static_cast<std::int64_t>(Current.Pc) + Offset);
                    }
                    break;
                }
            }
            [[fallthrough]];
        }
        case Op::Equal: case Op::Less: {
            const auto Size = Current.Stack.size();
            if (Size >= 2 && Current.Stack[Size - 2].IsNumeric() &&
                Current.Stack[Size - 1].IsNumeric()) {
                const double A = Current.Stack[Size - 2].AsNumber();
                const double B = Current.Stack[Size - 1].AsNumber();
                // Numeric/bool results cannot need ownership or Error hooks;
                // replacing two values with one also cannot exceed the stack limit.
                Current.Stack[Size - 2] = Value::Bool(Instruction == Op::Equal ? A == B : A < B);
                Current.Stack.pop_back();
                break;
            }
            auto Right = Pop(); auto Left = Pop();
            if (Instruction == Op::Equal) { Push(Value::Bool(Equal(Left, Right))); break; }
            Push(Value::Error("invalid operands"), DebugErrorOrigin::Operation);
            break;
        }
        case Op::ToByte: {
            auto Input = Pop();
            if (!Input.IsNumeric()) {
                Push(Value::Error("byte requires a numeric value"), DebugErrorOrigin::Operation);
            } else {
                try { Push(Value::Byte(Input.AsNumber())); }
                catch (const std::invalid_argument& Failure) {
                    Push(Value::Error(Failure.what()), DebugErrorOrigin::Operation);
                }
            }
            break;
        }
        case Op::Negate: {
            auto Input = Pop();
            if (Input.IsNumeric()) Push(Value::Number(-Input.AsNumber()));
            else Push(Value::Error("invalid operand"), DebugErrorOrigin::Operation);
            break;
        }
        case Op::Jump: case Op::JumpIf: {
            auto Bits = ReadU32(Code, Current.Pc);
            bool Take = Instruction == Op::Jump || Pop().IsTruthy();
            if (Take) Current.Pc = static_cast<std::size_t>(static_cast<std::int64_t>(Current.Pc) +
                                                           std::bit_cast<std::int32_t>(Bits));
            break;
        }
        case Op::Call: {
            std::uint16_t Count = std::uint16_t(Code[Current.Pc]) |
                                  (std::uint16_t(Code[Current.Pc + 1]) << 8);
            Current.Pc += 2;
            if (Current.Stack.size() > Count && Frames.size() < 1024) {
                const auto& Target = Current.Stack[Current.Stack.size() - Count - 1];
                auto* Function = Target.GetType() == ValueType::Object ?
                    FunctionObject::FromObject(Target.AsObject()) : nullptr;
                if (Function) {
                    const auto& CalleeOwner = Function->GetOwner();
                    ModuleInstance* CalleeInstance = nullptr;
                    if (CalleeOwner == FrameOwner) CalleeInstance = FrameInstance;
                    else if (CalleeOwner != Program) {
                        auto Found = ModuleByProgram.find(CalleeOwner.get());
                        if (Found == ModuleByProgram.end())
                            throw std::invalid_argument("function belongs to another VM module");
                        CalleeInstance = Found->second.get();
                    }
                    auto Owner = std::static_pointer_cast<FunctionObject>(Target.AsNativeObject());
                    auto& Callee = Frames.Enter();
                    // Enter may relocate Current. Reacquire the caller before moving arguments.
                    auto& Caller = Frames[Frames.size() - 2];
                    InitializeFrameFromStack(Callee, std::move(Owner), Caller.Stack, Count);
                    Callee.Instance = CalleeInstance;
                    break;
                }
            }
            std::vector<Value> Args(Count);
            for (std::size_t I = Count; I > 0; --I) Args[I - 1] = Pop();
            Value Target = Pop();
            DispatchCall(DispatchCall, std::move(Target), std::move(Args), 0, false);

            break;
        }
        case Op::Return: {
            auto Result = Pop();
            if (Result.IsError())
                ReachDebugError(Execution, Result, DebugErrorOrigin::FunctionReturn, InstructionStart);
            Frames.Leave();
            if (Frames.empty()) {
                if (ActiveRuns == 1) {
                    FaultLocation.reset(); FaultException = {}; FaultObject = nullptr;
                    NotifyDebugExecutionFinished(false);
                }
                return Result;
            }
            auto& Caller = Frames.back();
            const auto& Bytes = Caller.Function->GetBody().Code;
            if (!DebugController && Caller.Pc < Bytes.size()) {
                const auto Resume = static_cast<Op>(Bytes[Caller.Pc]);
                if (Resume == Op::StoreLocal || Resume == ReturnDestination) {
                    ++Caller.Pc;
                    const auto Slot = ReadU32(Bytes, Caller.Pc);
                    if (Resume == ReturnDestination) ++Caller.Pc; // Pop after SetLocal
                    Caller.Locals[Slot] = std::move(Result);
                    break;
                }
            }
            Caller.Stack.push_back(std::move(Result));
            break;
        }
        case Op::NewObject:
            Push(Value::FromScript(CreateScriptObject()));
            break;
        case Op::GetMember: case Op::GetField: {
            auto Key = Instruction == Op::GetField ? FrameConstants[ReadU32(Code, Current.Pc)] : Pop();
            auto Target = Pop();
            if (Target.GetType() != ValueType::Object) {
                Push(Value::Error("value has no members"), DebugErrorOrigin::Operation); break;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = Target.IsScriptObject() ? Target.AsScriptObject() : nullptr) {
                if ((Key.GetType() != ValueType::String && !Key.IsNumeric()) ||
                    (Key.GetType() == ValueType::Number && std::isnan(Key.AsNumber()))) {
                    Push(Value::Error("invalid ScriptObject key"), DebugErrorOrigin::Operation); break;
                }
                auto Found = Script->GetRaw(Key);
                if (Found) { Push(*Found, DebugErrorOrigin::Operation); break; }
                if (Script->IsArray()) {
                    Push(Value::Error("invalid array index or member"), DebugErrorOrigin::Operation);
                    break;
                }
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__index")) : std::nullopt;
                if (!Method) {
                    Push(Value::Error("member does not exist"), DebugErrorOrigin::Operation); break;
                }
                DispatchCall(DispatchCall, *Method, {Target, Key}, 0, true);
            } else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->GetMember(Key), DebugErrorOrigin::Operation);
            else Push(Value::Error("value has no members"), DebugErrorOrigin::Operation);
            break;
        }
        default:
            if (Instruction == ReadLocalIndex || Instruction == PrepareLocalIndex) {
                const auto& Receiver = Current.Locals[ReadU32(Code, Current.Pc)];
                if (!TryLocalIndex(Current, Instruction, Receiver, FrameConstants)) Push(Receiver);
                break;
            }
            if (Instruction == ReadLocalField || Instruction == PrepareLocalField) {
                const auto& Receiver = Current.Locals[ReadU32(Code, Current.Pc)];
                if (!TryLoadLocalField(Current, Instruction, Receiver, FrameConstants, FaultPc))
                    Push(Receiver); // Replay GetLocal; the next instruction retains its original PC.
                break;
            }
            if (Instruction != StoreLocalMember) throw std::logic_error("invalid opcode");
            if (TryStoreLocalMember(Current)) break;
            [[fallthrough]];
        case Op::SetMember: {
            auto Input = Pop(); auto Key = Pop(); auto Target = Pop();
            if (Target.GetType() != ValueType::Object) {
                Push(Value::Error("value has no members"), DebugErrorOrigin::Operation); break;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = Target.IsScriptObject() ? Target.AsScriptObject() : nullptr) {
                if (auto Hook = Script->FindSetIndex(Key))
                    DispatchCall(DispatchCall, *Hook, {Target, Key, Input}, 0, true);
                else
                    Push(Script->SetRaw(Key, Input), DebugErrorOrigin::Operation);
            } else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->SetMember(Key, Input), DebugErrorOrigin::Operation);
            else Push(Value::Error("value has no members"), DebugErrorOrigin::Operation);
            break;
        }
        }
    }
    throw std::logic_error("VM ended without Return");
    } catch (const std::exception& Fault) {
        auto Thrown = std::current_exception();
        if (FaultObject != &Fault || !FaultLocation) {
            FaultLocation = FaultBody ? LocationAt(*FaultBody, FaultPc) : std::nullopt;
            if (FaultLocation) FaultLocation->ModuleId = FaultModuleId;
        }
        FaultObject = &Fault;
        FaultException = std::move(Thrown);
        if (ActiveRuns == 1) NotifyDebugExecutionFinished(true);
        throw;
    } catch (...) {
        auto Thrown = std::current_exception();
        if (FaultException != Thrown || !FaultLocation) {
            FaultLocation = FaultBody ? LocationAt(*FaultBody, FaultPc) : std::nullopt;
            if (FaultLocation) FaultLocation->ModuleId = FaultModuleId;
        }
        FaultObject = nullptr;
        FaultException = std::move(Thrown);
        if (ActiveRuns == 1) NotifyDebugExecutionFinished(true);
        throw;
    }
}

void Vm::ReachDebugSafePoint(ExecutionState& Execution) {
    auto Controller = DebugController;
    try {
        VmDebugContext Context(this, &Execution);
        Controller->OnSafePoint(Context);
    } catch (...) {
        if (DebugController == Controller) DebugController.reset();
    }
}

void Vm::ReachDebugError(ExecutionState& Execution, const Value& Error,
                         DebugErrorOrigin Origin, std::size_t InstructionPc) {
    if (DebugCallbackSuppression != 0) return;
    auto Controller = DebugController;
    if (!Controller) return;
    try {
        VmDebugContext Context(this, &Execution, InstructionPc);
        Controller->OnError(Context, Error, Origin);
    } catch (...) {
        if (DebugController == Controller) DebugController.reset();
    }
}

void Vm::NotifyDebugExecutionFinished(bool Faulted) {
    if (DebugCallbackSuppression != 0) return;
    auto Controller = DebugController;
    if (!Controller) return;
    try { Controller->OnExecutionFinished(Faulted); }
    catch (...) {
        if (DebugController == Controller) DebugController.reset();
    }
}
} // namespace Feather
