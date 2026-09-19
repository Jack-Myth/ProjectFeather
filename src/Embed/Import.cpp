#include <Feather/Import.hpp>

#include <stdexcept>
#include <utility>

namespace Feather {
namespace {

constexpr NativeModuleGuid ImportAdapterGuid{{
    0x55, 0x17, 0x89, 0xb2, 0x72, 0x5b, 0x45, 0xec,
    0xa8, 0x24, 0xc7, 0x4d, 0x10, 0x0e, 0x35, 0xc3}};

class ImportFunction final : public NativeObject {
public:
    ImportFunction(NativeObjectType& Type, ImportCallback Callback)
        : NativeObject(Type), Callback(std::move(Callback)) {}

    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
            return Value::FromObject(std::make_shared<ErrorObject>(
                "import expects exactly one string argument"));
        auto Result = Callback(Arguments[0].AsString());
        if (Result.IsScriptObject() ||
            (Result.GetType() == ValueType::Object &&
             Result.GetObjectType() == ObjectType::Function))
            throw std::invalid_argument("import callback must return a host proxy, not a script object or function");
        return Result;
    }
private:
    ImportCallback Callback;
};

class ModuleImportFunction final : public NativeObject {
public:
    ModuleImportFunction(NativeObjectType& Type, Vm& Machine, ModuleImportCallback Callback)
        : NativeObject(Type), Machine(Machine), Callback(std::move(Callback)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
            return Value::FromObject(std::make_shared<ErrorObject>(
                "import expects exactly one string argument"));
        return Callback(Machine.GetActiveModuleId(), Arguments[0].AsString());
    }
private:
    Vm& Machine;
    ModuleImportCallback Callback;
};

} // namespace

void RegisterImport(Vm& Machine, ImportCallback Callback) {
    if (!Callback) throw std::invalid_argument("import callback is empty");
    auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
        ImportAdapterGuid, "LegacyImport");
    Machine.RegisterNativeFunction("import",
        Type.Create<ImportFunction>(std::move(Callback)));
}

void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback) {
    if (!Callback) throw std::invalid_argument("module import callback is empty");
    auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
        ImportAdapterGuid, "ModuleImport:" + std::string(ModuleId));
    auto Native = Type.Create<ModuleImportFunction>(Machine, std::move(Callback));
    if (ModuleId.empty()) Machine.RegisterNativeFunction("import", std::move(Native));
    else {
        Machine.RegisterNativeObject(Native);
        Machine.SetModuleGlobal(ModuleId, "import", Value::FromObject(std::move(Native)));
    }
}

} // namespace Feather
