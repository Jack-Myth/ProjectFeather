#include <Feather/NativeModule.hpp>

namespace Feather {
namespace {

class ExportedFunction final : public NativeObject {
public:
    ExportedFunction(NativeObjectType& Type, NativeModuleBuilder::Function Body)
        : NativeObject(Type), Body(std::move(Body)) {}

    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        try { return Body(Arguments); }
        catch (const IdleCallbackFault&) { throw; }
        catch (const std::exception& Failure) { return Value::Error(Failure.what()); }
    }
private:
    NativeModuleBuilder::Function Body;
};

} // namespace

class NativeModuleBuilder::Namespace final : public NativeObject {
public:
    Namespace(NativeObjectType& Type, std::string Name)
        : NativeObject(Type), Name(std::move(Name)) {}

    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String)
            return Value::Error("member name must be a string");
        auto Found = Members.find(Key.AsString());
        if (Found == Members.end()) return Value::Error("unknown " + Name + " member");
        return Found->second;
    }

    std::unordered_map<std::string, Value> Members;
private:
    std::string Name;
};

NativeModuleBuilder::NativeModuleBuilder(Vm& Machine, NativeModuleGuid Id, std::string Name)
    : Machine(Machine), Id(Id) {
    auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(Id, "Module");
    Module = Type.Create<Namespace>(std::move(Name));
}

void NativeModuleBuilder::AddFunction(std::string Name, Function Body) {
    if (!Body) throw std::invalid_argument("empty native module function");
    auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(Id, Name);
    Module->Members.emplace(std::move(Name), Value::FromObject(Type.Create<ExportedFunction>(std::move(Body))));
}

Value NativeModuleBuilder::Build() const { return Value::FromObject(Module); }

} // namespace Feather
