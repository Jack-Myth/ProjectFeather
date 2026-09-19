#pragma once
#include <Feather/Export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Feather {

class Object;
class Module;
class Vm;
class ScriptObject;
class NativeObject;
class NativeObjectType;
struct ExecutionState;
struct ModuleInstance;

struct NativeModuleGuid {
    std::array<std::uint8_t, 16> Bytes{};
    bool operator==(const NativeModuleGuid&) const = default;
};

struct NativeTypeId {
    NativeModuleGuid Module;
    std::string Name;
    bool operator==(const NativeTypeId&) const = default;
};

struct NativeTypeIdHash {
    std::size_t operator()(const NativeTypeId& Input) const noexcept;
};

enum class ValueType { Null, Bool, Number, String, Object };
enum class ObjectType { Script, Function, Error, Host };

class FEATHER_API Value {
public:
    Value();
    Value(const Value& Other);
    Value(Value&& Other) noexcept;
    Value& operator=(const Value& Other);
    Value& operator=(Value&& Other) noexcept;
    ~Value();

    static Value Bool(bool Input);
    static Value Number(double Input);
    static Value String(std::string Input);
    static Value FromObject(std::shared_ptr<Object> Input);
    static Value FromScript(ScriptObject* Input);

    ValueType GetType() const { return Type; }
    bool IsScriptObject() const { return Type == ValueType::Object && ScriptReference; }
    bool AsBool() const;
    double AsNumber() const;
    const std::string& AsString() const;
    Object* AsObject() const;
    ObjectType GetObjectType() const;
    bool IsError() const;
    ScriptObject* AsScriptObject() const;
    const std::shared_ptr<Object>& AsNativeObject() const;
    bool IsTruthy() const;

private:
    explicit Value(ValueType NewType);
    void Destroy();
    void CopyFrom(const Value& Other);
    ValueType Type = ValueType::Null;
    bool ScriptReference = false;
    union Storage {
        bool Boolean;
        double Numeric;
        std::shared_ptr<const std::string> Text;
        std::shared_ptr<Object> Reference;
        ScriptObject* Script;
        Storage() {}
        ~Storage() {}
    } Data;
};

class FEATHER_API Object {
public:
    virtual ~Object() = default;
    virtual ObjectType GetObjectType() const = 0;
};

class FEATHER_API NativeObject : public Object {
public:
    NativeObjectType* GetNativeObjectType() const noexcept { return Type; }
    virtual bool IsCallable() const { return false; }
    virtual Value Call(const std::vector<Value>& Arguments);
    virtual Value GetMember(const Value& Key);
    virtual Value SetMember(const Value& Key, const Value& Input);
    void SetGcVisibleMember(std::string Name, Value Input);
    void RemoveGcVisibleMember(const std::string& Name);
protected:
    NativeObject() = default;
    explicit NativeObject(NativeObjectType& InputType) : Type(&InputType) {}
private:
    friend class Vm;
    friend class VmDebugContext;
    NativeObjectType* Type = nullptr;
    std::unordered_map<std::string, Value> GcVisibleMembers;
};

class FEATHER_API NativeObjectType : public std::enable_shared_from_this<NativeObjectType> {
public:
    virtual ~NativeObjectType() = default;
    const NativeTypeId& GetId() const noexcept { return Id; }
    std::uint32_t GetSnapshotVersion() const noexcept { return SnapshotVersion; }
    Vm& GetVm() const noexcept { return Machine; }
    virtual std::vector<std::uint8_t> Serialize(const NativeObject& Input) const;
    virtual std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t StoredVersion, std::span<const std::uint8_t> Payload) = 0;
    virtual void FinalizeRestore(NativeObject&) {}
protected:
    NativeObjectType(Vm& Machine, NativeModuleGuid Module, std::string Name,
                     std::uint32_t SnapshotVersion = 1);
    template<class ObjectType, class... Arguments>
    std::shared_ptr<ObjectType> CreateObject(Arguments&&... Input) {
        static_assert(std::is_base_of_v<NativeObject, ObjectType>);
        auto Owner = shared_from_this();
        auto Result = std::shared_ptr<ObjectType>(
            new ObjectType(*this, std::forward<Arguments>(Input)...),
            [Owner = std::move(Owner)](ObjectType* Object) { Owner->Destroy(Object); });
        RegisterObject(Result);
        return Result;
    }
    void RegisterObject(const std::shared_ptr<NativeObject>& Input);
    virtual void Destroy(NativeObject* Input) noexcept { delete Input; }
private:
    friend class Vm;
    Vm& Machine;
    NativeTypeId Id;
    std::uint32_t SnapshotVersion;
};

class FEATHER_API NativeSingletonType final : public NativeObjectType {
public:
    NativeSingletonType(Vm& Machine, NativeModuleGuid Module, std::string Name,
                        std::uint32_t SnapshotVersion = 1)
        : NativeObjectType(Machine, Module, std::move(Name), SnapshotVersion) {}
    template<class Object, class... Arguments>
    std::shared_ptr<Object> Create(Arguments&&... Input) {
        if (!Canonical.expired())
            throw std::logic_error("native singleton already exists");
        auto Result = CreateObject<Object>(std::forward<Arguments>(Input)...);
        Canonical = Result;
        return Result;
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t StoredVersion, std::span<const std::uint8_t> Payload) override;
private:
    std::weak_ptr<NativeObject> Canonical;
};

class FEATHER_API ScriptObject final : public Object {
public:
    ObjectType GetObjectType() const override { return ObjectType::Script; }
    std::optional<Value> GetRaw(const Value& Key) const;
    Value SetRaw(const Value& Key, const Value& Input);
    ScriptObject* GetMetaObject() const { return MetaObject; }
private:
    friend class Vm;
    friend class VmDebugContext;
    explicit ScriptObject(Vm* Owner, ScriptObject* MetaObject = {});
    struct Key {
        ValueType Type;
        std::string Text;
        double Number = 0;
        bool operator==(const Key& Other) const;
    };
    struct KeyHash { std::size_t operator()(const Key& Input) const; };
    static std::optional<Key> ConvertKey(const Value& Input);
    Vm* Owner;
    ScriptObject* MetaObject;
    bool Marked = false;
    std::unordered_map<Key, Value, KeyHash> Members;
};

class FEATHER_API RootHandle final {
public:
    RootHandle() = default;
    RootHandle(const RootHandle&) = delete;
    RootHandle& operator=(const RootHandle&) = delete;
    RootHandle(RootHandle&& Other) noexcept;
    RootHandle& operator=(RootHandle&& Other) noexcept;
    ~RootHandle();
    Value Get() const;
    void Reset();
private:
    friend class Vm;
    RootHandle(Vm* Owner, std::weak_ptr<int> Lifetime, std::uint64_t Token);
    Vm* Owner = nullptr;
    std::weak_ptr<int> Lifetime;
    std::uint64_t Token = 0;
};

struct GcStatistics {
    std::size_t ScriptObjectCount = 0;
    std::size_t ScriptMemberCount = 0;
    std::size_t EstimatedScriptBytes = 0;
    std::size_t CollectionCount = 0;
    std::size_t TotalAllocatedScriptObjects = 0;
    std::size_t TotalCollectedScriptObjects = 0;
    std::size_t NextCollectionObjectCount = 0;
};

struct SourceLocation {
    std::size_t ByteOffset = 0;
    std::size_t Line = 0;
    std::size_t Column = 0;
    // Empty for the primary module; host-assigned logical ID for a loaded module.
    std::string ModuleId;
};

struct InstructionLocation {
    std::size_t Pc = 0;
    SourceLocation Source;
    // Synthetic instructions retain a diagnostic location but are skipped by
    // source breakpoints. Stepping may still observe them.
    bool Breakable = true;
};

struct LocalVariableInfo {
    std::string Name;
    std::uint32_t Slot = 0;
    std::size_t StartPc = 0;
    std::size_t EndPc = 0;
};

struct DebugFrameView {
    std::size_t FrameId = 0;
    std::uint32_t FunctionId = 0;
    std::string_view FunctionName;
    std::size_t Pc = 0;
    std::optional<SourceLocation> Source;
    bool Breakable = true;
    std::span<const Value> Locals;
    std::span<const Value> Stack;
    std::span<const LocalVariableInfo> LocalVariables;
};

struct DebugNamedValue {
    std::string Name;
    Value Data;
};

struct DebugProperty {
    Value Key;
    Value Data;
};

// Describes the result boundary at which an Error became observable. Error is
// still an ordinary language value; this classification only exists for the
// optional debugger hook.
enum class DebugErrorOrigin { Operation, FunctionReturn };

// A callback-scoped VM view. Inspection is read-only by default; the explicit
// Set*/Evaluate methods are the only supported mutation and debug-execution paths.
class FEATHER_API VmDebugContext final {
public:
    std::size_t GetFrameCount() const;
    DebugFrameView GetFrame(std::size_t FrameId) const;
    std::vector<DebugNamedValue> GetGlobals(std::size_t FrameId) const;
    std::vector<DebugProperty> GetProperties(const Value& Input) const;
    Value Evaluate(std::size_t FrameId, const std::shared_ptr<Module>& Program,
                   std::uint32_t FunctionConstant,
                   const std::vector<Value>& Arguments) const;
    void SetLocal(std::size_t FrameId, std::size_t Slot, Value Input) const;
    void SetStack(std::size_t FrameId, std::size_t Slot, Value Input) const;
    void SetGlobal(std::size_t FrameId, std::string Name, Value Input) const;
    Value SetProperty(const Value& Target, const Value& Key, Value Input) const;
private:
    friend class Vm;
    VmDebugContext(Vm* Machine, ExecutionState* Execution,
                   std::optional<std::size_t> TopFramePc = std::nullopt)
        : Machine(Machine), Execution(Execution), TopFramePc(TopFramePc) {}
    Vm* Machine;
    ExecutionState* Execution;
    std::optional<std::size_t> TopFramePc;
};

// Optional low-level hook implemented by embeddable debugger components.
class FEATHER_API VmDebugController {
public:
    virtual ~VmDebugController() = default;
    virtual void OnSafePoint(const VmDebugContext& Context) = 0;
    virtual void OnError(const VmDebugContext& Context, const Value& Error,
                         DebugErrorOrigin Origin) = 0;
    virtual void OnExecutionFinished(bool Faulted) = 0;
};

class FEATHER_API ErrorObject final : public NativeObject {
public:
    explicit ErrorObject(std::string Message) : Message(std::move(Message)) {}
    ObjectType GetObjectType() const override { return ObjectType::Error; }
    const std::string& GetMessage() const { return Message; }
    Value GetMember(const Value& Key) override;
private:
    std::string Message;
};

enum class Op : std::uint8_t {
    Const, Null, True, False, Pop, GetLocal, GetGlobal, SetLocal, SetGlobal,
    NewObject, GetMember, SetMember, Add, Sub, Mul, Div, Negate, Equal, Less,
    Jump, JumpIf, Call, Return
};

struct FunctionPrototype {
    std::vector<std::uint8_t> Code;
    // Optional debug data; .fbc omits it and a matching .fbs may supply it.
    std::vector<InstructionLocation> Locations;
    std::string DebugName;
    std::vector<LocalVariableInfo> LocalVariables;
    std::uint32_t ParameterCount = 0;
    std::uint32_t LocalCount = 0;
    std::vector<std::optional<Value>> Defaults;
};

struct FEATHER_API Constant {
    enum class Kind { Number, String, Function } Type;
    double Numeric = 0;
    std::string Text;
    std::shared_ptr<FunctionPrototype> Function;
    static Constant Number(double Input);
    static Constant String(std::string Input);
    static Constant FunctionRef(std::shared_ptr<FunctionPrototype> Input);
};

class FEATHER_API Module final {
public:
    std::vector<Constant> Constants;
    std::uint32_t AddNumber(double Input);
    std::uint32_t AddString(std::string Input);
    std::uint32_t AddFunction(std::shared_ptr<FunctionPrototype> Input);
    void Validate() const;
private:
    friend class Vm;
};

class FEATHER_API Builder final {
public:
    void Emit(Op Instruction);
    void EmitU16(Op Instruction, std::uint16_t Operand);
    void EmitU32(Op Instruction, std::uint32_t Operand);
    std::size_t EmitJump(Op Instruction);
    void PatchJump(std::size_t OperandPosition, std::size_t Target);
    std::size_t Offset() const { return Code.size(); }
    std::vector<std::uint8_t> Finish() && { return std::move(Code); }
private:
    std::vector<std::uint8_t> Code;
};

class FEATHER_API Vm final {
public:
    explicit Vm(std::shared_ptr<Module> Program,
                std::size_t MaxScriptObjects = std::numeric_limits<std::size_t>::max(),
                std::size_t MaxInstructionsPerInvocation = std::numeric_limits<std::size_t>::max());
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&) = delete;
    Vm& operator=(Vm&&) = delete;
    Value Run(std::uint32_t FunctionConstant, const std::vector<Value>& Arguments = {});
    void LoadModule(std::string Id, std::shared_ptr<Module> Source,
                    std::vector<std::string> Exports = {},
                    std::span<const std::uint8_t> Identity = {});
    bool IsModuleBuiltFrom(std::string_view Id, const std::shared_ptr<Module>& Source,
                           std::span<const std::uint8_t> Identity = {}) const;
    Value RunModule(std::string_view Id, std::uint32_t FunctionConstant,
                    const std::vector<Value>& Arguments = {});
    Value InitializeModule(std::string_view Id, std::uint32_t Initializer);
    Value GetModuleGlobal(std::string_view Id, const std::string& Name) const;
    void SetModuleGlobal(std::string_view Id, std::string Name, Value Input);
    Value GetModuleNamespace(std::string_view Id) const;
    std::string GetActiveModuleId() const;
    std::vector<std::uint8_t> CaptureSnapshot(
        std::size_t MaxBytes = 64 * 1024 * 1024) const;
    Value ResumeSnapshot(const std::vector<std::uint8_t>& Bytes,
                         std::size_t MaxBytes = 64 * 1024 * 1024);
    template<class Type, class... Arguments>
    Type& CreateNativeObjectType(Arguments&&... Input) {
        static_assert(std::is_base_of_v<NativeObjectType, Type>);
        auto Owned = std::make_shared<Type>(*this, std::forward<Arguments>(Input)...);
        auto* Result = Owned.get();
        RegisterNativeObjectType(std::move(Owned));
        return *Result;
    }
    bool IsBuiltFrom(const std::shared_ptr<Module>& Source) const { return SourceProgram == Source; }
    ScriptObject* GetRootMetaObject() const { return RootMetaObject; }
    ScriptObject* CreateScriptObject();
    ScriptObject* CreateMetaObject();
    void SetMetaObject(ScriptObject* Target, ScriptObject* MetaObject);
    Value GetGlobal(const std::string& Name) const;
    std::optional<SourceLocation> GetErrorLocation(const Value& Input) const;
    // Position of the most recent uncaught execution fault; reset on a new outer run.
    std::optional<SourceLocation> GetFaultLocation() const { return FaultLocation; }
    // The controller runs on the VM execution thread. Replace it only while idle.
    void SetDebugController(std::shared_ptr<VmDebugController> Input);
    std::shared_ptr<VmDebugController> GetDebugController() const { return DebugController; }
    void SetGlobal(std::string Name, Value Input);
    void RegisterNativeFunction(std::string Name, std::shared_ptr<NativeObject> Input);
    RootHandle AddToRoot(Value Input);
    void RegisterNativeObject(const std::shared_ptr<NativeObject>& Input);
    // Force a full collection while the VM is idle. Execution also collects
    // automatically at safe instruction boundaries when its threshold is reached.
    std::size_t CollectGarbage();
    GcStatistics GetGcStatistics() const;
    std::size_t GetScriptObjectCount() const { return ScriptHeap.size(); }
    std::uint32_t GetNativeCallDepth() const { return ActiveNativeCalls; }
private:
    friend class RootHandle;
    friend class ScriptObject;
    friend class VmDebugContext;
    ScriptObject* AllocateScriptObject(ScriptObject* MetaObject);
    void MaybeCollectGarbage();
    std::size_t CollectGarbageImpl();
    void UpdateCollectionThreshold();
    void RemoveFromRoot(std::uint64_t Token);
    void RegisterNativeObjectType(std::shared_ptr<NativeObjectType> Input);
    NativeObjectType* FindNativeObjectType(const NativeTypeId& Id) const;
    bool OwnsScript(ScriptObject* Input) const;
    void ValidateOwnedValue(const Value& Input) const;
    Value EvaluateDebugExpression(ExecutionState& PausedExecution, std::size_t FrameId,
                                  const std::shared_ptr<Module>& EvaluationProgram,
                                  std::uint32_t FunctionConstant,
                                  const std::vector<Value>& Arguments);
    Value Execute(ExecutionState& Execution);
    void ReachDebugSafePoint(ExecutionState& Execution);
    void ReachDebugError(ExecutionState& Execution, const Value& Error,
                         DebugErrorOrigin Origin, std::size_t InstructionPc);
    void NotifyDebugExecutionFinished(bool Faulted);
    std::vector<std::shared_ptr<NativeObjectType>> OwnedNativeTypes;
    std::unordered_map<NativeTypeId, NativeObjectType*, NativeTypeIdHash> NativeTypes;
    std::shared_ptr<Module> Program;
    std::shared_ptr<Module> SourceProgram;
    std::vector<std::shared_ptr<Object>> Functions;
    ScriptObject* RootMetaObject = nullptr;
    std::vector<std::unique_ptr<ScriptObject>> ScriptHeap;
    std::unordered_set<ScriptObject*> ScriptObjects;
    std::unordered_map<std::string, Value> Globals;
    std::unordered_map<std::string, std::shared_ptr<ModuleInstance>> LoadedModules;
    std::unordered_map<const Module*, std::shared_ptr<ModuleInstance>> ModuleByProgram;
    struct ErrorOrigin {
        std::weak_ptr<Object> Lifetime;
        SourceLocation Source;
    };
    std::unordered_map<const ErrorObject*, ErrorOrigin> ErrorLocations;
    std::optional<SourceLocation> FaultLocation;
    std::exception_ptr FaultException;
    const std::exception* FaultObject = nullptr;
    std::shared_ptr<VmDebugController> DebugController;
    std::unordered_map<std::uint64_t, Value> HostRoots;
    std::vector<std::weak_ptr<NativeObject>> NativeRegistry;
    std::shared_ptr<int> Lifetime = std::make_shared<int>(0);
    std::uint64_t NextRootToken = 1;
    std::uint32_t ActiveRuns = 0;
    std::uint32_t ActiveNativeCalls = 0;
    std::uint32_t DebugCallbackSuppression = 0;
    ExecutionState* ActiveExecution = nullptr;
    bool HasRun = false;
    mutable bool SnapshotBusy = false;
    std::size_t ScriptObjectLimit;
    std::size_t NextCollectionObjectCount = 0;
    std::size_t CollectionCount = 0;
    std::size_t TotalAllocatedScriptObjects = 0;
    std::size_t TotalCollectedScriptObjects = 0;
    bool CollectionPending = false;
    std::size_t InstructionLimit;
    std::size_t InstructionsRemaining = 0;
};

} // namespace Feather
