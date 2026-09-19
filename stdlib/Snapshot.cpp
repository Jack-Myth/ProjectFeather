#include <Feather/Snapshot.hpp>

#include <functional>
#include <stdexcept>
#include <string>
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
        if (Key.GetType() != ValueType::String) return Error("member name must be a string");
        auto Found = Members.find(Key.AsString());
        return Found == Members.end() ? Error("unknown snapshot member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};

} // namespace

SnapshotLibrary::SnapshotLibrary(Vm& Machine, SnapshotHost& Host) {
    auto& ModuleType = Machine.CreateNativeObjectType<NativeSingletonType>(
        SnapshotModuleGuid, "Module");
    auto& CheckpointType = Machine.CreateNativeObjectType<NativeSingletonType>(
        SnapshotModuleGuid, "Checkpoint");
    auto& RestoreType = Machine.CreateNativeObjectType<NativeSingletonType>(
        SnapshotModuleGuid, "Restore");
    auto Module = ModuleType.Create<Namespace>();
    Module->Add("Checkpoint", Value::FromObject(CheckpointType.Create<Function>(
        [&Machine, &Host](const std::vector<Value>& Arguments) {
            if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
                return Error("Snapshot.Checkpoint expects one path string");
            return Host.Checkpoint(Machine, Arguments[0].AsString());
        })));
    Module->Add("Restore", Value::FromObject(RestoreType.Create<Function>(
        [&Host](const std::vector<Value>& Arguments) {
            if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
                return Error("Snapshot.Restore expects one path string");
            return Host.Restore(Arguments[0].AsString());
        })));
    ModuleObject = std::move(Module);
}

SnapshotLibrary::~SnapshotLibrary() = default;
Value SnapshotLibrary::GetModule() const { return Value::FromObject(ModuleObject); }

} // namespace Feather
