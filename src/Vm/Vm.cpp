#include <Feather/ByteBuffer.hpp>
#include <Feather/Compiler.hpp>
#include <Feather/Runtime.hpp>
#include "Internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Feather {
namespace {
constexpr std::size_t MinimumCollectionGrowth = 1024;

Frame TakeFreeFrame(ExecutionState& Execution) {
    if (Execution.FreeFrameCount == 0) return {};
    return std::move(Execution.FreeFrames[--Execution.FreeFrameCount]);
}
void RecycleTopFrame(ExecutionState& Execution) {
    auto& Current = Execution.Frames.back();
    constexpr std::size_t MaximumRetainedSlots = 4096;
    if (Execution.FreeFrameCount < Execution.FreeFrames.size() &&
        Current.Arguments.capacity() + Current.Locals.capacity() +
            Current.Stack.capacity() <= MaximumRetainedSlots) {
        // Match Frame destruction order, releasing every value before reuse.
        Current.Stack.clear();
        Current.Locals.clear();
        Current.Arguments.clear();
        Current.Function.reset();
        Current.Pc = 0;
        Execution.FreeFrames[Execution.FreeFrameCount++] = std::move(Current);
    }
    Execution.Frames.pop_back();
}

Frame MakeFrame(std::shared_ptr<FunctionObject> Function, const std::vector<Value>& Args,
                Frame Next = {}) {
    const auto& Body = Function->GetBody();
    Next.Function = std::move(Function);
    Next.Pc = 0;
    Next.Arguments = Args;
    Next.Locals.resize(Body.LocalCount);
    for (std::size_t I = 0; I < Body.ParameterCount; ++I) {
        if (I < Args.size()) Next.Locals[I] = Args[I];
        else if (Body.Defaults[I]) Next.Locals[I] = *Body.Defaults[I];
    }
    return Next;
}
Frame MakeFrameFromStack(std::shared_ptr<FunctionObject> Function,
                         std::vector<Value>& Stack, std::size_t Count,
                         Frame Next = {}) {
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
    return Next;
}
Value NumericResult(Op Instruction, double A, double B) {
    switch (Instruction) {
    case Op::Add: return Value::Number(A + B);
    case Op::Sub: return Value::Number(A - B);
    case Op::Mul: return Value::Number(A * B);
    case Op::Div:
        // Preserve explicit IEEE-754 behavior with host divide-by-zero traps.
        if (B == 0) {
            if (A == 0 || std::isnan(A))
                return Value::Number(std::numeric_limits<double>::quiet_NaN());
            return Value::Number(std::copysign(std::numeric_limits<double>::infinity(),
                A * std::copysign(1.0, B)));
        }
        return Value::Number(A / B);
    case Op::Less: return Value::Bool(A < B);
    case Op::Equal: return Value::Bool(A == B);
    default: throw std::logic_error("invalid arithmetic opcode");
    }
}
struct ActiveRunGuard {
    explicit ActiveRunGuard(std::uint32_t& Count, std::function<void()> Finish = {})
        : Count(Count), Finish(std::move(Finish)) { ++Count; }
    ~ActiveRunGuard() { --Count; if (Finish) Finish(); }
    std::uint32_t& Count;
    std::function<void()> Finish;
};
Value Error(std::string Message) { return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message))); }
// Core builtin type identities belong to this VM.
constexpr NativeModuleGuid CoreBuiltinsGuid{{0x46,0x65,0x61,0x74,0x68,0x65,0x72,0x00,
                                             0xb1,0x17,0x49,0x81,0x96,0x27,0xa5,0x01}};
class EvalBytecodeFunction final : public NativeObject {
public:
    EvalBytecodeFunction(NativeObjectType& Type, Vm& Machine) : NativeObject(Type), Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.empty() || Arguments.size() > 2)
            return Error("eval_bytecode expects ByteBuffer and optional context");
        return Machine.EvalBytecode(Arguments[0], Arguments.size() == 2 ? Arguments[1] : Value{});
    }
private:
    Vm& Machine;
};
class ArrayFunction final : public NativeObject {
public:
    ArrayFunction(NativeObjectType& Type, Vm& Machine) : NativeObject(Type), Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (!Arguments.empty()) return Error("array expects no arguments");
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
        if (Arguments.size() != 1) return Error("get_type requires exactly one argument");
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
        return Error("unrecognized value type");
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
            if (!Arguments.empty()) return Error("get_module_paths expects no arguments");
            const auto& Paths = Machine.GetModulePaths();
            auto* List = Machine.CreateArray();
            for (const auto& Path : Paths) List->AppendArray(Value::String(Path));
            return Value::FromScript(List);
        }
        if (Arguments.size() != 1 || !Arguments[0].IsScriptObject())
            return Error("set_module_paths expects a list of strings");
        const auto* List = Arguments[0].AsScriptObject();
        if (!List->IsArray()) return Error("set_module_paths expects an array of strings");
        const auto Elements = List->GetArrayElements();
        if (Elements.size() > 65536) return Error("too many module paths");
        std::vector<std::string> Paths;
        Paths.reserve(Elements.size());
        for (const auto& Entry : Elements) {
            if (Entry.GetType() != ValueType::String)
                return Error("module path array entries must be strings");
            Paths.push_back(Entry.AsString());
        }
        try { Machine.SetModulePaths(std::move(Paths)); }
        catch (const std::invalid_argument& Failure) { return Error(Failure.what()); }
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
            return Error("no active Feather function");
        const auto& Values = Execution->Frames.back().Arguments;
        if (Count) {
            if (!Arguments.empty()) return Error("get_func_argc expects no arguments");
            return Value::Number(static_cast<double>(Values.size()));
        }
        if (Arguments.size() != 1 || !Arguments[0].IsNumeric())
            return Error("get_func_argv expects one numeric index");
        const auto Index = Arguments[0].AsNumber();
        if (!std::isfinite(Index) || Index < 0 || std::trunc(Index) != Index ||
            Index >= static_cast<double>(Values.size()))
            return Error("function argument index out of range");
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
            return Error("invoke_function expects a callable and an array");
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
            return Error("set_metaobject expects two script objects");
        try { Machine.SetMetaObject(Arguments[0].AsScriptObject(), Arguments[1].AsScriptObject()); }
        catch (const std::invalid_argument& Failure) { return Error(Failure.what()); }
        return Arguments[0];
    }
private:
    Vm& Machine;
};

class VmModule final : public NativeObject {
public:
    VmModule(NativeObjectType& Type, Value Eval, Value SetPaths, Value GetPaths,
             Value GetArgc, Value GetArgv, Value Invoke, Value SetMeta)
        : NativeObject(Type), Eval(std::move(Eval)),
          SetPaths(std::move(SetPaths)), GetPaths(std::move(GetPaths)),
          GetArgc(std::move(GetArgc)), GetArgv(std::move(GetArgv)),
          Invoke(std::move(Invoke)), SetMeta(std::move(SetMeta)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String) {
            if (Key.AsString() == "eval_bytecode") return Eval;
            if (Key.AsString() == "set_module_paths") return SetPaths;
            if (Key.AsString() == "get_module_paths") return GetPaths;
            if (Key.AsString() == "get_func_argc") return GetArgc;
            if (Key.AsString() == "get_func_argv") return GetArgv;
            if (Key.AsString() == "invoke_function") return Invoke;
            if (Key.AsString() == "set_metaobject") return SetMeta;
        }
        return Error("unknown vm module member");
    }
    std::vector<std::string> Paths;
private:
    Value Eval;
    Value SetPaths;
    Value GetPaths;
    Value GetArgc;
    Value GetArgv;
    Value Invoke;
    Value SetMeta;
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
Value ConstantValue(Vm& Machine, const Constant& Item, const std::shared_ptr<Object>& Function) {
    switch (Item.Type) {
    case Constant::Kind::Number: return Value::Number(Item.Numeric);
    case Constant::Kind::Byte: return Value::Byte(Item.ByteValue);
    case Constant::Kind::String: return Value::String(Item.Text);
    case Constant::Kind::Buffer: return ByteBufferLibrary(Machine).Create(Item.Text);
    case Constant::Kind::Function: return Value::FromObject(Function);
    }
    throw std::logic_error("invalid constant kind");
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
        if (Arguments.size() != 1) return Error("array method expects exactly one argument");
        if (Lifetime.expired() || !Machine->OwnsScript(Receiver))
            return Error("array receiver is no longer available");

        const auto& Input = Arguments.front();
        if (Action == Operation::Add) {
            Receiver->AppendArray(Input);
            return Value::Bool(true);
        }
        if (Action == Operation::RemoveAt) {
            auto Index = Receiver->GetArrayIndex(Input);
            if (!Index || !Receiver->RemoveArrayAt(*Index)) return Error("invalid array index");
            return Value::Bool(true);
        }
        auto& Elements = *Receiver->Elements;
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
    ArrayMethod::Operation Action;
    if (Name == "Add" || Name == "add") Action = ArrayMethod::Operation::Add;
    else if (Name == "remove") Action = ArrayMethod::Operation::Remove;
    else if (Name == "RemoveAt" || Name == "remove_at") Action = ArrayMethod::Operation::RemoveAt;
    else return std::nullopt;
    return Value::FromObject(std::make_shared<ArrayMethod>(
        *const_cast<ScriptObject*>(this), Action));
}

Value ModuleNamespace::GetMember(const Value& Key) {
    auto Instance = Owner.lock();
    if (!Instance) throw std::logic_error("module namespace has expired");
    if (Instance->Initialization != ModuleInstance::State::Initialized)
        return Instance->Initialization == ModuleInstance::State::Failed ?
            Instance->InitializationError : Error("module is not initialized");
    if (Key.GetType() != ValueType::String || !Instance->Exports.contains(Key.AsString()))
        return Error("unknown module export");
    auto Found = Instance->Globals.find(Key.AsString());
    return Found == Instance->Globals.end() ? Error("module export is not initialized") : Found->second;
}

Value ModuleNamespace::SetMember(const Value&, const Value&) {
    return Error("module exports are read-only");
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
        Values = Instance->second->GlobalScope ? Instance->second->GlobalScope : &Instance->second->Globals;
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
        auto& Values = Found->second->GlobalScope ? *Found->second->GlobalScope : Found->second->Globals;
        Values.insert_or_assign(std::move(Name), std::move(Input));
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
    UpdateCollectionThreshold();
    auto& Type = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "get_type");
    BuiltinGetType = Value::FromObject(Type.Create<GetTypeFunction>());
    Globals.emplace("get_type", BuiltinGetType);
    auto& ArrayType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "array");
    BuiltinArray = Value::FromObject(ArrayType.Create<ArrayFunction>(*this));
    Globals.emplace("array", BuiltinArray);
    auto& EvalType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "eval_bytecode");
    auto Eval = Value::FromObject(EvalType.Create<EvalBytecodeFunction>(*this));
    auto& SetPathsType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "set_module_paths");
    auto& GetPathsType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "get_module_paths");
    auto SetPaths = Value::FromObject(SetPathsType.Create<ModulePathsFunction>(*this, true));
    auto GetPaths = Value::FromObject(GetPathsType.Create<ModulePathsFunction>(*this, false));
    auto& ArgcType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "get_func_argc");
    auto& ArgvType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "get_func_argv");
    auto GetArgc = Value::FromObject(ArgcType.Create<FunctionArgumentsFunction>(ActiveExecution, true));
    auto GetArgv = Value::FromObject(ArgvType.Create<FunctionArgumentsFunction>(ActiveExecution, false));
    auto& InvokeType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "invoke_function");
    auto Invoke = Value::FromObject(InvokeType.Create<InvokeFunction>(*this));
    auto& SetMetaType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "set_metaobject");
    auto SetMeta = Value::FromObject(SetMetaType.Create<SetMetaObjectFunction>(*this));
    auto& VmModuleType = CreateNativeObjectType<NativeSingletonType>(CoreBuiltinsGuid, "vm");
    BuiltinVmModule = Value::FromObject(VmModuleType.Create<VmModule>(
        std::move(Eval), std::move(SetPaths), std::move(GetPaths),
        std::move(GetArgc), std::move(GetArgv), std::move(Invoke), std::move(SetMeta)));
    // Freeze the mutable builder's module before any execution.
    Program = std::make_shared<Module>(*Input);
    for (auto& Item : Program->Constants) {
        if (Item.Type == Constant::Kind::Function)
            Item.Function = std::make_shared<FunctionPrototype>(*Item.Function);
    }
    Functions.resize(Program->Constants.size());
    for (std::size_t I = 0; I < Functions.size(); ++I) {
        const auto& Item = Program->Constants[I];
        if (Item.Type == Constant::Kind::Function)
            Functions[I] = std::make_shared<FunctionObject>(Program, Item.Function);
    }
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
    Instance->Functions.resize(Instance->Program->Constants.size());
    for (std::size_t I = 0; I < Instance->Functions.size(); ++I) {
        const auto& Item = Instance->Program->Constants[I];
        if (Item.Type == Constant::Kind::Function)
            Instance->Functions[I] = std::make_shared<FunctionObject>(Instance->Program, Item.Function);
    }
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
    return Global == Found->second->Globals.end() ? Error("undefined global") : Global->second;
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
    if (ActiveRuns != 0 && ScriptHeap.size() >= NextCollectionObjectCount)
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
    return Found == Globals.end() ? Error("undefined global") : Found->second;
}
void Vm::ValidateOwnedValue(const Value& Input) const {
    if (Input.IsScriptObject() && !OwnsScript(Input.AsScriptObject()))
        throw std::invalid_argument("ScriptObject belongs to another VM");
    if (Input.GetType() == ValueType::Object &&
        Input.GetObjectType() == ObjectType::Function) {
        auto* Function = dynamic_cast<FunctionObject*>(Input.AsObject());
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
void Vm::RegisterNativeObject(const std::shared_ptr<NativeObject>& Input) {
    if (!Input) throw std::invalid_argument("null NativeObject");
    if (Input->Type && &Input->Type->GetVm() != this)
        throw std::invalid_argument("NativeObject type belongs to another VM");
    NativeRegistry.push_back(Input);
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
void Vm::UpdateCollectionThreshold() {
    auto Live = ScriptHeap.size();
    auto Growth = std::max(MinimumCollectionGrowth, Live);
    auto Maximum = std::numeric_limits<std::size_t>::max();
    auto Candidate = Live > Maximum - Growth ? Maximum : Live + Growth;
    NextCollectionObjectCount = std::min(Candidate, ScriptObjectLimit);
}

void Vm::MaybeCollectGarbage() {
    if (ActiveRuns != 1 + RootedEvalDepth || ActiveNativeCalls != 0 || !ActiveExecution)
        return;
    if (!CollectionPending && ScriptHeap.size() < NextCollectionObjectCount)
        return;
    (void)CollectGarbageImpl();
}

std::size_t Vm::CollectGarbage() {
    if (ActiveRuns != 0 || ActiveNativeCalls != 0)
        throw std::logic_error("GC requires an idle VM");
    return CollectGarbageImpl();
}

std::size_t Vm::CollectGarbageImpl() {
    struct MarkReset {
        std::vector<std::unique_ptr<ScriptObject>>& Heap;
        ~MarkReset() { for (auto& Entry : Heap) Entry->Marked = false; }
    } Reset{ScriptHeap};
    std::vector<ScriptObject*> ScriptWork;
    std::vector<std::shared_ptr<NativeObject>> NativeWork;
    std::unordered_set<NativeObject*> SeenNative;
    auto MarkScript = [&](ScriptObject* Input) {
        if (!ScriptObjects.contains(Input))
            throw std::invalid_argument("foreign or stale ScriptObject in GC graph");
        if (!Input->Marked) { Input->Marked = true; ScriptWork.push_back(Input); }
    };
    auto MarkValue = [&](const Value& Input) {
        if (Input.GetType() != ValueType::Object) return;
        if (Input.IsScriptObject()) MarkScript(Input.AsScriptObject());
        else {
            auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject());
            if (Native && SeenNative.insert(Native.get()).second) NativeWork.push_back(std::move(Native));
        }
    };
    MarkScript(RootMetaObject);
    for (const auto& Function : Functions)
        if (Function) MarkValue(Value::FromObject(Function));
    for (const auto& [_, Entry] : Globals) MarkValue(Entry);
    for (const auto& [_, Instance] : LoadedModules) {
        for (const auto& Function : Instance->Functions)
            if (Function) MarkValue(Value::FromObject(Function));
        for (const auto& [__, Entry] : Instance->Globals) MarkValue(Entry);
        MarkValue(Instance->InitializationError);
        MarkValue(Value::FromObject(Instance->Namespace));
    }
    for (const auto& [_, Entry] : HostRoots) MarkValue(Entry);
    if (ActiveExecution)
        for (const auto& Current : ActiveExecution->Frames) {
            for (const auto& Entry : Current.Arguments) MarkValue(Entry);
            for (const auto& Entry : Current.Locals) MarkValue(Entry);
            for (const auto& Entry : Current.Stack) MarkValue(Entry);
        }
    for (auto It = NativeRegistry.begin(); It != NativeRegistry.end();) {
        if (auto Native = It->lock()) {
            if (SeenNative.insert(Native.get()).second) NativeWork.push_back(std::move(Native));
            ++It;
        } else It = NativeRegistry.erase(It);
    }
    while (!ScriptWork.empty() || !NativeWork.empty()) {
        while (!ScriptWork.empty()) {
            auto Script = ScriptWork.back(); ScriptWork.pop_back();
            if (Script->MetaObject) MarkScript(Script->MetaObject);
            for (const auto& [_, Entry] : Script->Members) MarkValue(Entry);
            for (const auto& Entry : Script->GetArrayElements()) MarkValue(Entry);
        }
        while (!NativeWork.empty()) {
            auto Native = std::move(NativeWork.back()); NativeWork.pop_back();
            for (const auto& [_, Entry] : Native->GcVisibleMembers) MarkValue(Entry);
        }
    }
    auto Before = ScriptHeap.size();
    std::erase_if(ScriptHeap, [&](const auto& Entry) {
        if (Entry->Marked) return false;
        ScriptObjects.erase(Entry.get());
        return true;
    });
    for (auto& Entry : ScriptHeap) Entry->Marked = false;
    auto Collected = Before - ScriptHeap.size();
    ++CollectionCount;
    TotalCollectedScriptObjects += Collected;
    CollectionPending = false;
    UpdateCollectionThreshold();
    return Collected;
}
GcStatistics Vm::GetGcStatistics() const {
    GcStatistics Result;
    Result.ScriptObjectCount = ScriptHeap.size();
    Result.EstimatedScriptBytes = ScriptHeap.size() * sizeof(ScriptObject);
    for (const auto& Script : ScriptHeap) {
        Result.ScriptMemberCount += Script->Members.size();
        Result.EstimatedScriptBytes += Script->Members.bucket_count() * sizeof(void*);
        for (const auto& [Key, _] : Script->Members)
            Result.EstimatedScriptBytes += sizeof(Key) + sizeof(Value) + 2 * sizeof(void*) + Key.Text.capacity();
    }
    Result.CollectionCount = CollectionCount;
    Result.TotalAllocatedScriptObjects = TotalAllocatedScriptObjects;
    Result.TotalCollectedScriptObjects = TotalCollectedScriptObjects;
    Result.NextCollectionObjectCount = NextCollectionObjectCount;
    return Result;
}

Value Vm::Run(std::uint32_t FunctionConstant, const std::vector<Value>& Arguments) {
    if (ActiveRuns == 0) { FaultLocation.reset(); FaultException = {}; FaultObject = nullptr; }
    if (FunctionConstant >= Functions.size() || !Functions[FunctionConstant])
        throw std::invalid_argument("entry is not a function constant");
    for (const auto& Input : Arguments) ValidateOwnedValue(Input);
    ExecutionState Execution;
    Execution.Frames.push_back(MakeFrame(
        std::static_pointer_cast<FunctionObject>(Functions[FunctionConstant]), Arguments));
    return Execute(Execution);
}

ScriptFunctionAddress Vm::GetFunctionAddress(const Value& Input) const {
    if (Input.GetType() != ValueType::Object || Input.GetObjectType() != ObjectType::Function)
        throw std::invalid_argument("expected a script function");
    ValidateOwnedValue(Input);
    const auto* Function = static_cast<const FunctionObject*>(Input.AsObject());
    const auto* Table = &Functions;
    ScriptFunctionAddress Address{};
    if (Function->GetOwner() != Program) {
        const auto& Instance = *ModuleByProgram.at(Function->GetOwner().get());
        if (!Instance.Identity)
            throw std::invalid_argument("function module has no transferable program identity");
        Table = &Instance.Functions;
        Address.ModuleId = Instance.Id;
        Address.ModuleIdentity = Instance.Identity;
    }
    auto Found = std::find(Table->begin(), Table->end(), Input.AsNativeObject());
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
        if (Target.GetType() != ValueType::Object) return Error("value is not callable");
        if (Target.IsScriptObject()) {
            auto* Meta = Target.AsScriptObject()->GetMetaObject();
            auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
            if (!Method) return Error("__call is missing");
            Arguments.insert(Arguments.begin(), Target);
            Target = *Method;
            Roots.push_back(AddToRoot(Target));
            continue;
        }
        auto* Native = dynamic_cast<NativeObject*>(Target.AsObject());
        if (!Native || !Native->IsCallable()) return Error("value is not callable");
        if (dynamic_cast<InvokeFunction*>(Native)) {
            if (Depth == 63) return Error("invoke_function forwarding limit exceeded");
            if (auto Failure = InvokeFunction::Expand(Arguments, Target, Arguments); Failure.IsError())
                return Failure;
            Roots.push_back(AddToRoot(Target));
            for (const auto& Argument : Arguments) Roots.push_back(AddToRoot(Argument));
            continue;
        }
        if (Native->GetObjectType() == ObjectType::Function) {
            ExecutionState Execution;
            Execution.Frames.push_back(MakeFrame(
                std::static_pointer_cast<FunctionObject>(Target.AsNativeObject()), Arguments));
            return Execute(Execution);
        }
        if (dynamic_cast<EvalBytecodeFunction*>(Native)) {
            auto Result = Native->Call(Arguments);
            ValidateOwnedValue(Result);
            return Result;
        }
        ActiveRunGuard Guard(ActiveNativeCalls);
        auto Result = Native->Call(Arguments);
        ValidateOwnedValue(Result);
        return Result;
    }
    return Error("MetaObject call cycle");
}

Value Vm::RunModule(std::string_view Id, std::uint32_t FunctionConstant,
                    const std::vector<Value>& Arguments) {
    if (ActiveRuns == 0) { FaultLocation.reset(); FaultException = {}; FaultObject = nullptr; }
    auto Found = LoadedModules.find(std::string(Id));
    if (Found == LoadedModules.end()) throw std::invalid_argument("unknown loaded module");
    const auto& ModuleFunctions = Found->second->Functions;
    if (FunctionConstant >= ModuleFunctions.size() || !ModuleFunctions[FunctionConstant])
        throw std::invalid_argument("entry is not a function constant");
    for (const auto& Input : Arguments) ValidateOwnedValue(Input);
    ExecutionState Execution;
    Execution.Frames.push_back(MakeFrame(
        std::static_pointer_cast<FunctionObject>(ModuleFunctions[FunctionConstant]), Arguments));
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

Value Vm::EvalBytecode(const Value& Code, Value Context) {
    const auto* Bytes = ByteBufferData(Code);
    if (!Bytes) return Error("eval_bytecode expects a ByteBuffer");
    CompiledProgram Compiled;
    try {
        Compiled = DeserializeProgram({reinterpret_cast<const std::uint8_t*>(Bytes->data()), Bytes->size()});
        if (Compiled.Functions.size() != 1 || !Compiled.Functions.contains("__compiled_block") || !Compiled.Exports.empty())
            return Error("eval_bytecode requires a compiled block without module declarations");
    } catch (const std::exception& Failure) {
        return Error(std::string("invalid compiled bytecode: ") + Failure.what());
    }
    auto Entry = Compiled.Functions.at("__compiled_block");
    auto& Body = Compiled.Program->Constants.at(Entry).Function;
    if (Body->ParameterCount != 1) return Error("eval_bytecode requires a compiled block");
    auto Instance = std::make_shared<ModuleInstance>();
    Instance->Program = Compiled.Program;
    Instance->Id = GetActiveModuleId();
    Instance->GlobalScope = &Globals;
    if (ActiveExecution && !ActiveExecution->Frames.empty()) {
        auto Owner = ActiveExecution->Frames.back().Function->GetOwner();
        if (Owner != Program) {
            auto Caller = ModuleByProgram.at(Owner.get());
            Instance->GlobalScope = Caller->GlobalScope ? Caller->GlobalScope : &Caller->Globals;
        }
    }
    Instance->Functions.resize(Compiled.Program->Constants.size());
    for (std::size_t I = 0; I < Instance->Functions.size(); ++I) {
        const auto& Item = Compiled.Program->Constants[I];
        if (Item.Type == Constant::Kind::Function)
            Instance->Functions[I] = std::make_shared<FunctionObject>(Compiled.Program, Item.Function);
    }
    ModuleByProgram.emplace(Instance->Program.get(), Instance);
    struct Guard {
        Vm& Machine; const Module* Program;
        ~Guard() { Machine.ModuleByProgram.erase(Program); }
    } Cleanup{*this, Instance->Program.get()};
    // Nested execution replaces ActiveExecution; keep all caller frames rooted.
    std::vector<RootHandle> CallerRoots;
    if (ActiveExecution) for (const auto& Frame : ActiveExecution->Frames) {
        for (const auto& Value : Frame.Arguments) CallerRoots.push_back(AddToRoot(Value));
        for (const auto& Value : Frame.Locals) CallerRoots.push_back(AddToRoot(Value));
        for (const auto& Value : Frame.Stack) CallerRoots.push_back(AddToRoot(Value));
    }
    struct EvalDepthGuard {
        Vm& Machine; bool Nested;
        EvalDepthGuard(Vm& Machine) : Machine(Machine), Nested(Machine.ActiveRuns != 0) {
            if (Nested) ++Machine.RootedEvalDepth;
        }
        ~EvalDepthGuard() { if (Nested) --Machine.RootedEvalDepth; }
    } RootedDepth(*this);
    return Call(Value::FromObject(Instance->Functions[Entry]), {Context});
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
        PausedGlobals = Found->second->GlobalScope ? Found->second->GlobalScope : &Found->second->Globals;
    }

    auto Instance = std::make_shared<ModuleInstance>();
    Instance->Id = "<debug-expression>";
    Instance->Source = EvaluationProgram;
    Instance->Program = std::make_shared<Module>(*EvaluationProgram);
    Instance->Globals = *PausedGlobals;
    for (auto& Item : Instance->Program->Constants)
        if (Item.Type == Constant::Kind::Function)
            Item.Function = std::make_shared<FunctionPrototype>(*Item.Function);
    Instance->Functions.resize(Instance->Program->Constants.size());
    for (std::size_t I = 0; I < Instance->Functions.size(); ++I) {
        const auto& Item = Instance->Program->Constants[I];
        if (Item.Type == Constant::Kind::Function)
            Instance->Functions[I] = std::make_shared<FunctionObject>(Instance->Program, Item.Function);
    }
    if (FunctionConstant >= Instance->Functions.size() || !Instance->Functions[FunctionConstant])
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
    Evaluation.Frames.push_back(MakeFrame(
        std::static_pointer_cast<FunctionObject>(Instance->Functions[FunctionConstant]), Arguments));
    auto Result = Execute(Evaluation);
    if (Result.GetType() == ValueType::Object && Result.GetObjectType() == ObjectType::Function) {
        auto* Function = dynamic_cast<FunctionObject*>(Result.AsObject());
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
    if (Initializer >= Instance.Functions.size() || !Instance.Functions[Initializer] ||
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
    auto PushFrame = [&](const std::shared_ptr<FunctionObject>& Function,
                         const std::vector<Value>& Args) {
        if (Frames.size() >= 1024) throw std::runtime_error("VM call depth exceeded");
        Frames.push_back(MakeFrame(Function, Args,
                                   TakeFreeFrame(Execution)));
    };
    const FunctionPrototype* FaultBody = nullptr;
    std::size_t FaultPc = 0;
    std::string FaultModuleId;
    try {
    while (!Frames.empty()) {
        MaybeCollectGarbage();
        ReachDebugSafePoint(Execution);
        Frame& Next = Frames.back();
        FaultBody = &Next.Function->GetBody();
        FaultPc = Next.Pc;
        // The module function table retains this heap object across frame moves/pops.
        // Borrow its owner only for this instruction.
        const auto& FrameOwner = Next.Function->GetOwner();
        ModuleInstance* FrameInstance = nullptr;
        if (FrameOwner != Program) {
            auto Found = ModuleByProgram.find(FrameOwner.get());
            if (Found == ModuleByProgram.end())
                throw std::logic_error("active function has no module instance");
            FrameInstance = Found->second.get();
        }
        FaultModuleId = FrameInstance ? FrameInstance->Id : std::string{};
        auto& FrameFunctions = FrameInstance ? FrameInstance->Functions : Functions;
        auto& FrameGlobals = FrameInstance ? (FrameInstance->GlobalScope ? *FrameInstance->GlobalScope : FrameInstance->Globals) : Globals;
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
            if (Current.Stack.size() >= 65'536) throw std::runtime_error("VM stack limit exceeded");
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
                Push(Error("MetaObject call cycle"), DebugErrorOrigin::Operation); return;
            }
            if (Target.GetType() != ValueType::Object) {
                Push(Error(MetaCall ? "metafunction is not callable" : "value is not callable"),
                     DebugErrorOrigin::Operation); return;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue)) {
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
                if (!Method) { Push(Error("__call is missing"), DebugErrorOrigin::Operation); return; }
                Args.insert(Args.begin(), Target);
                Self(Self, *Method, std::move(Args), Depth + 1, true);
                return;
            }
            auto Native = dynamic_cast<NativeObject*>(ObjectValue);
            if (!Native || !Native->IsCallable()) {
                Push(Error(MetaCall ? "metafunction is not callable" : "value is not callable"),
                     DebugErrorOrigin::Operation); return;
            }
            if (dynamic_cast<InvokeFunction*>(Native)) {
                if (Depth == 63) {
                    Push(Error("invoke_function forwarding limit exceeded"), DebugErrorOrigin::Operation);
                    return;
                }
                if (auto Failure = InvokeFunction::Expand(Args, Target, Args); Failure.IsError()) {
                    Push(std::move(Failure), DebugErrorOrigin::FunctionReturn);
                    return;
                }
                Self(Self, std::move(Target), std::move(Args), Depth + 1, false);
            } else if (Native->GetObjectType() == ObjectType::Function) {
                auto Function = std::static_pointer_cast<FunctionObject>(Target.AsNativeObject());
                if (Function->GetOwner() != Program &&
                    !ModuleByProgram.contains(Function->GetOwner().get()))
                    throw std::invalid_argument("function belongs to another VM module");
                PushFrame(Function, Args);
            } else if (dynamic_cast<EvalBytecodeFunction*>(Native)) {
                // This builtin runs interpreter code, not an arbitrary native
                // callback. Caller values are rooted by EvalBytecode, so normal
                // instruction-boundary GC remains available inside the block.
                Push(Native->Call(Args), DebugErrorOrigin::FunctionReturn);
            } else {
                ActiveRunGuard NativeGuard(ActiveNativeCalls);
                Push(Native->Call(Args), DebugErrorOrigin::FunctionReturn);
            }
        };
        switch (Instruction) {
        case Op::Const: {
            auto Id = ReadU32(Code, Current.Pc);
            Push(ConstantValue(*this, FrameOwner->Constants[Id], FrameFunctions[Id])); break;
        }
        case Op::Null: Push({}); break;
        case Op::True: Push(Value::Bool(true)); break;
        case Op::False: Push(Value::Bool(false)); break;
        case Op::Pop: Pop(); break;
        case Op::GetLocal: Push(Current.Locals[ReadU32(Code, Current.Pc)]); break;
        case Op::SetLocal: {
            auto Id = ReadU32(Code, Current.Pc);
            Current.Locals[Id] = Current.Stack.back(); break;
        }
        case Op::GetGlobal: {
            auto Id = ReadU32(Code, Current.Pc);
            auto Found = FrameGlobals.find(FrameOwner->Constants[Id].Text);
            if (Found == FrameGlobals.end())
                Push(Error("undefined global"), DebugErrorOrigin::Operation);
            else Push(Found->second);
            break;
        }
        case Op::SetGlobal: {
            auto Id = ReadU32(Code, Current.Pc);
            FrameGlobals[FrameOwner->Constants[Id].Text] = Current.Stack.back(); break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        case Op::Equal: case Op::Less: {
            const auto Size = Current.Stack.size();
            if (Size >= 2 && Current.Stack[Size - 2].IsNumeric() &&
                Current.Stack[Size - 1].IsNumeric()) {
                const double A = Current.Stack[Size - 2].AsNumber();
                const double B = Current.Stack[Size - 1].AsNumber();
                // Numeric/bool results cannot need ownership or Error hooks;
                // replacing two values with one also cannot exceed the stack limit.
                Current.Stack[Size - 2] = NumericResult(Instruction, A, B);
                Current.Stack.pop_back();
                break;
            }
            auto Right = Pop(); auto Left = Pop();
            if (Instruction == Op::Equal) { Push(Value::Bool(Equal(Left, Right))); break; }
            if (Instruction == Op::Add && Left.GetType() == ValueType::String &&
                Right.GetType() == ValueType::String) {
                Push(Value::String(Left.AsString() + Right.AsString())); break;
            }
            if (!Left.IsNumeric() || !Right.IsNumeric()) {
                Push(Error("invalid operands"), DebugErrorOrigin::Operation); break;
            }
            Push(NumericResult(Instruction, Left.AsNumber(), Right.AsNumber()));
            break;
        }
        case Op::ToByte: {
            auto Input = Pop();
            if (!Input.IsNumeric()) {
                Push(Error("byte requires a numeric value"), DebugErrorOrigin::Operation);
            } else {
                try { Push(Value::Byte(Input.AsNumber())); }
                catch (const std::invalid_argument& Failure) {
                    Push(Error(Failure.what()), DebugErrorOrigin::Operation);
                }
            }
            break;
        }
        case Op::Negate: {
            auto Input = Pop();
            if (Input.IsNumeric()) Push(Value::Number(-Input.AsNumber()));
            else Push(Error("invalid operand"), DebugErrorOrigin::Operation);
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
                    dynamic_cast<FunctionObject*>(Target.AsObject()) : nullptr;
                if (Function &&
                    (Function->GetOwner() == Program ||
                                 ModuleByProgram.contains(Function->GetOwner().get()))) {
                    auto Owner = std::static_pointer_cast<FunctionObject>(Target.AsNativeObject());
                    auto NewFrame = MakeFrameFromStack(std::move(Owner), Current.Stack,
                                                      Count,
                                                      TakeFreeFrame(Execution));
                    // Appending can relocate Frames; do not use Current afterward.
                    Frames.push_back(std::move(NewFrame));
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
            RecycleTopFrame(Execution);
            if (Frames.empty()) {
                if (ActiveRuns == 1) {
                    FaultLocation.reset(); FaultException = {}; FaultObject = nullptr;
                    NotifyDebugExecutionFinished(false);
                }
                return Result;
            }
            Frames.back().Stack.push_back(std::move(Result));
            break;
        }
        case Op::NewObject:
            Push(Value::FromScript(CreateScriptObject()));
            break;
        case Op::GetMember: {
            auto Key = Pop(); auto Target = Pop();
            if (Target.GetType() != ValueType::Object) {
                Push(Error("value has no members"), DebugErrorOrigin::Operation); break;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue)) {
                if ((Key.GetType() != ValueType::String && !Key.IsNumeric()) ||
                    (Key.GetType() == ValueType::Number && std::isnan(Key.AsNumber()))) {
                    Push(Error("invalid ScriptObject key"), DebugErrorOrigin::Operation); break;
                }
                auto Found = Script->GetRaw(Key);
                if (Found) { Push(*Found, DebugErrorOrigin::Operation); break; }
                if (Script->IsArray()) {
                    Push(Error("invalid array index or member"), DebugErrorOrigin::Operation);
                    break;
                }
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__index")) : std::nullopt;
                if (!Method) {
                    Push(Error("member does not exist"), DebugErrorOrigin::Operation); break;
                }
                DispatchCall(DispatchCall, *Method, {Target, Key}, 0, true);
            } else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->GetMember(Key), DebugErrorOrigin::Operation);
            else Push(Error("value has no members"), DebugErrorOrigin::Operation);
            break;
        }
        case Op::SetMember: {
            auto Input = Pop(); auto Key = Pop(); auto Target = Pop();
            if (Target.GetType() != ValueType::Object) {
                Push(Error("value has no members"), DebugErrorOrigin::Operation); break;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue))
                Push(Script->SetRaw(Key, Input), DebugErrorOrigin::Operation);
            else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->SetMember(Key, Input), DebugErrorOrigin::Operation);
            else Push(Error("value has no members"), DebugErrorOrigin::Operation);
            break;
        }
        default: throw std::logic_error("invalid opcode");
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
    if (DebugCallbackSuppression != 0) return;
    auto Controller = DebugController;
    if (!Controller) return;
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
