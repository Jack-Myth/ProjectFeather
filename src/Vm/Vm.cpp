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
Frame MakeFrame(std::shared_ptr<FunctionObject> Function, const std::vector<Value>& Args) {
    const auto& Body = Function->GetBody();
    Frame Next;
    Next.Function = std::move(Function);
    Next.Locals.resize(Body.LocalCount);
    for (std::size_t I = 0; I < Body.ParameterCount; ++I) {
        if (I < Args.size()) Next.Locals[I] = Args[I];
        else if (Body.Defaults[I]) Next.Locals[I] = *Body.Defaults[I];
    }
    return Next;
}
struct ActiveRunGuard {
    explicit ActiveRunGuard(std::uint32_t& Count) : Count(Count) { ++Count; }
    ~ActiveRunGuard() { --Count; }
    std::uint32_t& Count;
};
Value Error(std::string Message) { return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message))); }
bool Equal(const Value& Left, const Value& Right) {
    if (Left.GetType() != Right.GetType()) return false;
    switch (Left.GetType()) {
    case ValueType::Null: return true;
    case ValueType::Bool: return Left.AsBool() == Right.AsBool();
    case ValueType::Number: return Left.AsNumber() == Right.AsNumber();
    case ValueType::String: return Left.AsString() == Right.AsString();
    case ValueType::Object: return Left.AsObject() == Right.AsObject();
    }
    return false;
}
std::uint32_t ReadU32(const std::vector<std::uint8_t>& Code, std::size_t& Pc) {
    std::uint32_t Result = 0;
    for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(Code[Pc++]) << (I * 8);
    return Result;
}
Value ConstantValue(const Constant& Item, const std::shared_ptr<Object>& Function) {
    switch (Item.Type) {
    case Constant::Kind::Number: return Value::Number(Item.Numeric);
    case Constant::Kind::String: return Value::String(Item.Text);
    case Constant::Kind::Function: return Value::FromObject(Function);
    }
    throw std::logic_error("invalid constant kind");
}
std::optional<SourceLocation> LocationAt(const FunctionPrototype& Body, std::size_t Pc) {
    auto Found = std::lower_bound(Body.Locations.begin(), Body.Locations.end(), Pc,
        [](const InstructionLocation& Location, std::size_t Target) { return Location.Pc < Target; });
    if (Found == Body.Locations.end() || Found->Pc != Pc) return std::nullopt;
    return Found->Source;
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

Vm::Vm(std::shared_ptr<Module> Input, std::size_t MaxScriptObjects,
       std::size_t MaxInstructionsPerInvocation)
    : ScriptObjectLimit(MaxScriptObjects), InstructionLimit(MaxInstructionsPerInvocation) {
    if (!Input) throw std::invalid_argument("null module");
    if (ScriptObjectLimit == 0) throw std::invalid_argument("ScriptObject limit must include RootMetaObject");
    if (InstructionLimit == 0) throw std::invalid_argument("instruction limit must be positive");
    Input->Validate();
    SourceProgram = Input;
    ScriptHeap.emplace_back(new ScriptObject(this));
    RootMetaObject = ScriptHeap.back().get();
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
        bool SameProgram = Identity.empty() == Found->second->Identity.empty() &&
            (Identity.empty() ? Found->second->Source == Source :
                std::equal(Identity.begin(), Identity.end(), Found->second->Identity.begin(),
                           Found->second->Identity.end()));
        if (SameProgram && Found->second->Exports == Exports) return;
        throw std::invalid_argument("module ID already belongs to another program");
    }
    if (LoadedModules.size() >= 65'536) throw std::invalid_argument("too many loaded modules");
    Source->Validate();
    auto Instance = std::make_shared<ModuleInstance>();
    Instance->Id = Id;
    Instance->Source = Source;
    Instance->Identity.assign(Identity.begin(), Identity.end());
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
    if (Identity.empty() != Found->second->Identity.empty()) return false;
    if (!Identity.empty())
        return std::equal(Identity.begin(), Identity.end(), Found->second->Identity.begin(),
                          Found->second->Identity.end());
    return Found->second->Source == Source;
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

ScriptObject* Vm::CreateScriptObject() {
    if (ScriptHeap.size() >= ScriptObjectLimit) throw std::bad_alloc();
    ScriptHeap.emplace_back(new ScriptObject(this, RootMetaObject));
    return ScriptHeap.back().get();
}
ScriptObject* Vm::CreateMetaObject() { return CreateScriptObject(); }
bool Vm::OwnsScript(ScriptObject* Input) const {
    return std::any_of(ScriptHeap.begin(), ScriptHeap.end(),
                       [Input](const auto& Entry) { return Entry.get() == Input; });
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
    NativeRegistry.push_back(Input);
}
std::size_t Vm::CollectGarbage() {
    if (ActiveRuns != 0 || ActiveNativeCalls != 0)
        throw std::logic_error("GC requires an idle VM");
    struct MarkReset {
        std::vector<std::unique_ptr<ScriptObject>>& Heap;
        ~MarkReset() { for (auto& Entry : Heap) Entry->Marked = false; }
    } Reset{ScriptHeap};
    std::unordered_set<ScriptObject*> HeapPointers;
    for (const auto& Entry : ScriptHeap) HeapPointers.insert(Entry.get());
    std::vector<ScriptObject*> ScriptWork;
    std::vector<std::shared_ptr<NativeObject>> NativeWork;
    std::unordered_set<NativeObject*> SeenNative;
    auto MarkScript = [&](ScriptObject* Input) {
        if (!HeapPointers.contains(Input)) throw std::invalid_argument("foreign or stale ScriptObject in GC graph");
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
    for (const auto& [_, Entry] : Globals) MarkValue(Entry);
    for (const auto& [_, Instance] : LoadedModules) {
        for (const auto& [__, Entry] : Instance->Globals) MarkValue(Entry);
        MarkValue(Instance->InitializationError);
        MarkValue(Value::FromObject(Instance->Namespace));
    }
    for (const auto& [_, Entry] : HostRoots) MarkValue(Entry);
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
        }
        while (!NativeWork.empty()) {
            auto Native = std::move(NativeWork.back()); NativeWork.pop_back();
            for (const auto& [_, Entry] : Native->GcVisibleMembers) MarkValue(Entry);
        }
    }
    auto Before = ScriptHeap.size();
    std::erase_if(ScriptHeap, [](const auto& Entry) { return !Entry->Marked; });
    for (auto& Entry : ScriptHeap) Entry->Marked = false;
    return Before - ScriptHeap.size();
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
    if (ActiveRuns == 0) InstructionsRemaining = InstructionLimit;
    ActiveRunGuard Guard(ActiveRuns);
    HasRun = true;
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
        Frames.push_back(MakeFrame(Function, Args));
    };
    const FunctionPrototype* FaultBody = nullptr;
    std::size_t FaultPc = 0;
    std::string FaultModuleId;
    try {
    while (!Frames.empty()) {
        Frame& Next = Frames.back();
        FaultBody = &Next.Function->GetBody();
        FaultPc = Next.Pc;
        auto FrameOwner = Next.Function->GetOwner();
        ModuleInstance* FrameInstance = nullptr;
        if (FrameOwner != Program) {
            auto Found = ModuleByProgram.find(FrameOwner.get());
            if (Found == ModuleByProgram.end())
                throw std::logic_error("active function has no module instance");
            FrameInstance = Found->second.get();
        }
        FaultModuleId = FrameInstance ? FrameInstance->Id : std::string{};
        auto& FrameFunctions = FrameInstance ? FrameInstance->Functions : Functions;
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
        auto Push = [&](Value Input) {
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
        };
        std::function<void(Value, std::vector<Value>, unsigned, bool)> DispatchCall;
        DispatchCall = [&](Value Target, std::vector<Value> Args, unsigned Depth, bool MetaCall) {
            if (Depth >= 64) { Push(Error("MetaObject call cycle")); return; }
            if (Target.GetType() != ValueType::Object) {
                Push(Error(MetaCall ? "metafunction is not callable" : "value is not callable")); return;
            }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue)) {
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__call")) : std::nullopt;
                if (!Method) { Push(Error("__call is missing")); return; }
                Args.insert(Args.begin(), Target);
                DispatchCall(*Method, std::move(Args), Depth + 1, true);
                return;
            }
            auto Native = dynamic_cast<NativeObject*>(ObjectValue);
            if (!Native || !Native->IsCallable()) {
                Push(Error(MetaCall ? "metafunction is not callable" : "value is not callable")); return;
            }
            if (Native->GetObjectType() == ObjectType::Function) {
                auto Function = std::static_pointer_cast<FunctionObject>(Target.AsNativeObject());
                if (Function->GetOwner() != Program &&
                    !ModuleByProgram.contains(Function->GetOwner().get()))
                    throw std::invalid_argument("function belongs to another VM module");
                PushFrame(Function, Args);
            } else {
                ActiveRunGuard NativeGuard(ActiveNativeCalls);
                Push(Native->Call(Args));
            }
        };
        switch (Instruction) {
        case Op::Const: {
            auto Id = ReadU32(Code, Current.Pc);
            Push(ConstantValue(FrameOwner->Constants[Id], FrameFunctions[Id])); break;
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
            Push(Found == FrameGlobals.end() ? Error("undefined global") : Found->second); break;
        }
        case Op::SetGlobal: {
            auto Id = ReadU32(Code, Current.Pc);
            FrameGlobals[FrameOwner->Constants[Id].Text] = Current.Stack.back(); break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        case Op::Equal: case Op::Less: {
            auto Right = Pop(); auto Left = Pop();
            if (Instruction == Op::Equal) { Push(Value::Bool(Equal(Left, Right))); break; }
            if (Instruction == Op::Add && Left.GetType() == ValueType::String &&
                Right.GetType() == ValueType::String) {
                Push(Value::String(Left.AsString() + Right.AsString())); break;
            }
            if (Left.GetType() != ValueType::Number || Right.GetType() != ValueType::Number) {
                Push(Error("invalid operands")); break;
            }
            double A = Left.AsNumber(), B = Right.AsNumber();
            switch (Instruction) {
            case Op::Add: Push(Value::Number(A + B)); break;
            case Op::Sub: Push(Value::Number(A - B)); break;
            case Op::Mul: Push(Value::Number(A * B)); break;
            case Op::Div: {
                // Explicit IEEE-754 behavior even when the host FP environment traps divide-by-zero.
                if (B == 0) {
                    if (A == 0 || std::isnan(A)) Push(Value::Number(std::numeric_limits<double>::quiet_NaN()));
                    else Push(Value::Number(std::copysign(std::numeric_limits<double>::infinity(), A * std::copysign(1.0, B))));
                } else Push(Value::Number(A / B));
                break;
            }
            case Op::Less: Push(Value::Bool(A < B)); break;
            default: throw std::logic_error("invalid arithmetic opcode");
            }
            break;
        }
        case Op::Negate: {
            auto Input = Pop();
            Push(Input.GetType() == ValueType::Number ? Value::Number(-Input.AsNumber()) : Error("invalid operand"));
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
            std::vector<Value> Args(Count);
            for (std::size_t I = Count; I > 0; --I) Args[I - 1] = Pop();
            Value Target = Pop();
            Execution.PendingCallResult = true;
            DispatchCall(std::move(Target), std::move(Args), 0, false);
            Execution.PendingCallResult = false;
            break;
        }
        case Op::Return: {
            auto Result = Pop();
            Frames.pop_back();
            if (Frames.empty()) {
                if (ActiveRuns == 1) {
                    FaultLocation.reset(); FaultException = {}; FaultObject = nullptr;
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
            if (Target.GetType() != ValueType::Object) { Push(Error("value has no members")); break; }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue)) {
                if ((Key.GetType() != ValueType::String && Key.GetType() != ValueType::Number) ||
                    (Key.GetType() == ValueType::Number && std::isnan(Key.AsNumber()))) {
                    Push(Error("invalid ScriptObject key")); break;
                }
                auto Found = Script->GetRaw(Key);
                if (Found) { Push(*Found); break; }
                auto Meta = Script->GetMetaObject();
                auto Method = Meta ? Meta->GetRaw(Value::String("__index")) : std::nullopt;
                if (!Method) { Push(Error("member does not exist")); break; }
                DispatchCall(*Method, {Target, Key}, 0, true);
            } else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->GetMember(Key));
            else Push(Error("value has no members"));
            break;
        }
        case Op::SetMember: {
            auto Input = Pop(); auto Key = Pop(); auto Target = Pop();
            if (Target.GetType() != ValueType::Object) { Push(Error("value has no members")); break; }
            auto ObjectValue = Target.AsObject();
            if (auto Script = dynamic_cast<ScriptObject*>(ObjectValue))
                Push(Script->SetRaw(Key, Input));
            else if (auto Native = dynamic_cast<NativeObject*>(ObjectValue))
                Push(Native->SetMember(Key, Input));
            else Push(Error("value has no members"));
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
        throw;
    } catch (...) {
        auto Thrown = std::current_exception();
        if (FaultException != Thrown || !FaultLocation) {
            FaultLocation = FaultBody ? LocationAt(*FaultBody, FaultPc) : std::nullopt;
            if (FaultLocation) FaultLocation->ModuleId = FaultModuleId;
        }
        FaultObject = nullptr;
        FaultException = std::move(Thrown);
        throw;
    }
}
} // namespace Feather
