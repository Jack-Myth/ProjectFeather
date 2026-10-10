#include <Feather/Checkpoint.hpp>

#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

namespace Feather {
namespace {

Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}

using Method = std::function<Value(const std::vector<Value>&)>;

class Function final : public NativeObject {
public:
    Function(NativeObjectType& Type, Method Body)
        : NativeObject(Type), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        try { return Body(Arguments); }
        catch (const std::exception& Failure) { return Error(Failure.what()); }
    }
private:
    Method Body;
};

class Namespace final : public NativeObject {
public:
    explicit Namespace(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    void Add(std::string Name, Value Input) {
        Members.emplace(std::move(Name), std::move(Input));
    }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String)
            return Error("checkpoint member name must be a string");
        auto Found = Members.find(Key.AsString());
        return Found == Members.end() ? Error("unknown checkpoint member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};

bool ReadId(const Value& Input, std::uint64_t& Id) {
    constexpr double MaxExactScriptInteger = 9007199254740991.0;
    if (!Input.IsNumeric()) return false;
    auto Number = Input.AsNumber();
    if (!std::isfinite(Number) || Number < 1 || Number > MaxExactScriptInteger ||
        std::trunc(Number) != Number) return false;
    Id = static_cast<std::uint64_t>(Number);
    return true;
}

} // namespace

CheckpointLibrary::CheckpointLibrary(Vm& Machine) {
    auto& ModuleType = Machine.CreateNativeObjectType<NativeSingletonType>(
        CheckpointModuleGuid, "Module");
    auto& CaptureType = Machine.CreateNativeObjectType<NativeSingletonType>(
        CheckpointModuleGuid, "Capture");
    auto& RestoreType = Machine.CreateNativeObjectType<NativeSingletonType>(
        CheckpointModuleGuid, "Restore");
    auto& UnloadType = Machine.CreateNativeObjectType<NativeSingletonType>(
        CheckpointModuleGuid, "Unload");
    auto Module = ModuleType.Create<Namespace>();
    Module->Add("Capture", Value::FromObject(CaptureType.Create<Function>(
        [&Machine](const std::vector<Value>& Arguments) {
            if (!Arguments.empty()) return Error("checkpoint.Capture expects no arguments");
            return Value::Number(static_cast<double>(Machine.CaptureCheckpoint()));
        })));
    Module->Add("Restore", Value::FromObject(RestoreType.Create<Function>(
        [&Machine](const std::vector<Value>& Arguments) {
            std::uint64_t Id = 0;
            if (Arguments.size() != 1 || !ReadId(Arguments[0], Id))
                return Error("checkpoint.Restore expects one checkpoint ID");
            Machine.RequestCheckpointRestore(Id);
            return Value{};
        })));
    Module->Add("Unload", Value::FromObject(UnloadType.Create<Function>(
        [&Machine](const std::vector<Value>& Arguments) {
            std::uint64_t Id = 0;
            if (Arguments.size() != 1 || !ReadId(Arguments[0], Id))
                return Error("checkpoint.Unload expects one checkpoint ID");
            if (!Machine.UnloadCheckpoint(Id)) return Error("unknown checkpoint ID");
            return Value::Bool(true);
        })));
    ModuleObject = std::move(Module);
}

CheckpointLibrary::~CheckpointLibrary() = default;
Value CheckpointLibrary::GetModule() const { return Value::FromObject(ModuleObject); }

} // namespace Feather
