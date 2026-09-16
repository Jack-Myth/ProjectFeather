#include <Feather/Import.hpp>

#include <stdexcept>
#include <utility>

namespace Feather {
namespace {

class ImportFunction final : public NativeObject {
public:
    explicit ImportFunction(ImportCallback Callback) : Callback(std::move(Callback)) {}

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
    ModuleImportFunction(Vm& Machine, ModuleImportCallback Callback)
        : Machine(Machine), Callback(std::move(Callback)) {}
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
    Machine.RegisterNativeFunction("import",
        std::make_shared<ImportFunction>(std::move(Callback)));
}

void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback) {
    if (!Callback) throw std::invalid_argument("module import callback is empty");
    auto Native = std::make_shared<ModuleImportFunction>(Machine, std::move(Callback));
    if (ModuleId.empty()) Machine.RegisterNativeFunction("import", std::move(Native));
    else {
        Machine.RegisterNativeObject(Native);
        Machine.SetModuleGlobal(ModuleId, "import", Value::FromObject(std::move(Native)));
    }
}

} // namespace Feather
