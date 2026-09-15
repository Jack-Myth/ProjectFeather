#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Feather {

class Object;
class Module;
class Vm;
class ScriptObject;
struct ExecutionState;
class SnapshotHostCodec;

struct HostSnapshotRecord {
    std::string TypeId;
    std::vector<std::uint8_t> Payload;
};

enum class ValueType { Null, Bool, Number, String, Object };
enum class ObjectType { Script, Function, Error, Host };

class Value {
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

class Object {
public:
    virtual ~Object() = default;
    virtual ObjectType GetObjectType() const = 0;
};

class NativeObject : public Object {
public:
    virtual bool IsCallable() const { return false; }
    virtual Value Call(const std::vector<Value>& Arguments);
    virtual Value GetMember(const Value& Key);
    virtual Value SetMember(const Value& Key, const Value& Input);
    void SetGcVisibleMember(std::string Name, Value Input);
    void RemoveGcVisibleMember(const std::string& Name);
private:
    friend class Vm;
    std::unordered_map<std::string, Value> GcVisibleMembers;
};

class SnapshotHostCodec {
public:
    virtual ~SnapshotHostCodec() = default;
    virtual HostSnapshotRecord Encode(const std::shared_ptr<NativeObject>& Input) = 0;
    virtual std::shared_ptr<NativeObject> Decode(Vm& Machine, const HostSnapshotRecord& Input) = 0;
};

class ScriptObject final : public Object {
public:
    ObjectType GetObjectType() const override { return ObjectType::Script; }
    std::optional<Value> GetRaw(const Value& Key) const;
    Value SetRaw(const Value& Key, const Value& Input);
    ScriptObject* GetMetaObject() const { return MetaObject; }
private:
    friend class Vm;
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

class RootHandle final {
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
};

class ErrorObject final : public NativeObject {
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
    std::uint32_t ParameterCount = 0;
    std::uint32_t LocalCount = 0;
    std::vector<std::optional<Value>> Defaults;
};

struct Constant {
    enum class Kind { Number, String, Function } Type;
    double Numeric = 0;
    std::string Text;
    std::shared_ptr<FunctionPrototype> Function;
    static Constant Number(double Input);
    static Constant String(std::string Input);
    static Constant FunctionRef(std::shared_ptr<FunctionPrototype> Input);
};

class Module final {
public:
    std::vector<Constant> Constants;
    std::uint32_t AddNumber(double Input);
    std::uint32_t AddString(std::string Input);
    std::uint32_t AddFunction(std::shared_ptr<FunctionPrototype> Input);
    void Validate() const;
private:
    friend class Vm;
};

class Builder final {
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

class Vm final {
public:
    explicit Vm(std::shared_ptr<Module> Program,
                std::size_t MaxScriptObjects = std::numeric_limits<std::size_t>::max(),
                std::size_t MaxInstructionsPerInvocation = std::numeric_limits<std::size_t>::max());
    Value Run(std::uint32_t FunctionConstant, const std::vector<Value>& Arguments = {});
    std::vector<std::uint8_t> CaptureSnapshot(SnapshotHostCodec* Codec = nullptr,
                                              std::size_t MaxBytes = 64 * 1024 * 1024) const;
    Value ResumeSnapshot(const std::vector<std::uint8_t>& Bytes,
                         SnapshotHostCodec* Codec = nullptr,
                         std::size_t MaxBytes = 64 * 1024 * 1024);
    bool IsBuiltFrom(const std::shared_ptr<Module>& Source) const { return SourceProgram == Source; }
    ScriptObject* GetRootMetaObject() const { return RootMetaObject; }
    ScriptObject* CreateScriptObject();
    ScriptObject* CreateMetaObject();
    void SetMetaObject(ScriptObject* Target, ScriptObject* MetaObject);
    Value GetGlobal(const std::string& Name) const;
    void SetGlobal(std::string Name, Value Input);
    void RegisterNativeFunction(std::string Name, std::shared_ptr<NativeObject> Input);
    RootHandle AddToRoot(Value Input);
    void RegisterNativeObject(const std::shared_ptr<NativeObject>& Input);
    std::size_t CollectGarbage();
    GcStatistics GetGcStatistics() const;
    std::size_t GetScriptObjectCount() const { return ScriptHeap.size(); }
    std::uint32_t GetNativeCallDepth() const { return ActiveNativeCalls; }
private:
    friend class RootHandle;
    friend class ScriptObject;
    void RemoveFromRoot(std::uint64_t Token);
    bool OwnsScript(ScriptObject* Input) const;
    void ValidateOwnedValue(const Value& Input) const;
    Value Execute(ExecutionState& Execution);
    std::shared_ptr<Module> Program;
    std::shared_ptr<Module> SourceProgram;
    std::vector<std::shared_ptr<Object>> Functions;
    ScriptObject* RootMetaObject = nullptr;
    std::vector<std::unique_ptr<ScriptObject>> ScriptHeap;
    std::unordered_map<std::string, Value> Globals;
    std::unordered_map<std::uint64_t, Value> HostRoots;
    std::vector<std::weak_ptr<NativeObject>> NativeRegistry;
    std::shared_ptr<int> Lifetime = std::make_shared<int>(0);
    std::uint64_t NextRootToken = 1;
    std::uint32_t ActiveRuns = 0;
    std::uint32_t ActiveNativeCalls = 0;
    ExecutionState* ActiveExecution = nullptr;
    bool HasRun = false;
    mutable bool SnapshotBusy = false;
    std::size_t ScriptObjectLimit;
    std::size_t InstructionLimit;
    std::size_t InstructionsRemaining = 0;
};

} // namespace Feather
