#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>

#include <iostream>
#include <stdexcept>

using namespace Feather;

namespace {

void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

class FunctionProxy final : public NativeObject {
public:
    FunctionProxy(std::shared_ptr<Vm> Child, std::uint32_t Entry)
        : Child(std::move(Child)), Entry(Entry) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::Number)
            return Value::FromObject(std::make_shared<ErrorObject>("proxy expects one number"));
        auto Result = Child->Run(Entry, Arguments);
        if (Result.GetType() == ValueType::Object)
            return Value::FromObject(std::make_shared<ErrorObject>(
                "proxy cannot transfer child VM objects"));
        return Result;
    }
private:
    std::shared_ptr<Vm> Child;
    std::uint32_t Entry;
};

class ModuleProxy final : public NativeObject {
public:
    explicit ModuleProxy(std::shared_ptr<FunctionProxy> Multiply)
        : Multiply(std::move(Multiply)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "multiply")
            return Value::FromObject(Multiply);
        return Value::FromObject(std::make_shared<ErrorObject>("unknown export"));
    }
private:
    std::shared_ptr<FunctionProxy> Multiply;
};

class ReturnValue final : public NativeObject {
public:
    explicit ReturnValue(Value Result) : Result(std::move(Result)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override { return Result; }
private:
    Value Result;
};

} // namespace

int main() {
    try {
        auto ChildProgram = Compile("var factor = 6; def multiply(x) { return factor * x; }");
        auto Child = std::make_shared<Vm>(ChildProgram.Program);
        (void)ChildProgram.Initialize(*Child);
        auto Proxy = std::make_shared<ModuleProxy>(
            std::make_shared<FunctionProxy>(Child, ChildProgram.Functions.at("multiply")));

        auto ParentProgram = Compile(
            "var math = import(\"math\"); def main() { return math.multiply(7); }");
        Vm Parent(ParentProgram.Program);
        int Calls = 0;
        RegisterImport(Parent, [&](std::string_view Specifier) -> Value {
            ++Calls;
            if (Specifier == "math") return Value::FromObject(Proxy);
            return Value::FromObject(std::make_shared<ErrorObject>("unknown module"));
        });
        (void)ParentProgram.Initialize(Parent);
        Check(Calls == 1 && Parent.Run(ParentProgram.Functions.at("main")).AsNumber() == 42,
              "host Feather module proxy failed");

        auto BadProgram = Compile("def bad() { return import(7); }");
        Vm Bad(BadProgram.Program);
        RegisterImport(Bad, [](std::string_view) -> Value { return {}; });
        (void)BadProgram.Initialize(Bad);
        Check(Bad.Run(BadProgram.Functions.at("bad")).IsError(),
              "invalid import argument did not return Error");

        auto ForeignProgram = Compile("def probe() { return import(\"other\"); }");
        Vm Foreign(ForeignProgram.Program);
        (void)ForeignProgram.Initialize(Foreign);
        auto ForeignScript = Foreign.CreateScriptObject();
        Vm Guarded(ForeignProgram.Program);
        RegisterImport(Guarded, [&](std::string_view) -> Value {
            return Value::FromScript(ForeignScript);
        });
        (void)ForeignProgram.Initialize(Guarded);
        try { Guarded.SetGlobal("foreign_function", Foreign.GetGlobal("probe")); }
        catch (const std::invalid_argument&) { /* expected */ }
        Check(Guarded.GetGlobal("foreign_function").IsError(),
              "foreign function stored in another VM");

        auto NativeProgram = Compile("def probe() { return foreign(); }");
        Vm NativeGuarded(NativeProgram.Program);
        NativeGuarded.RegisterNativeFunction("foreign",
            std::make_shared<ReturnValue>(Value::FromScript(ForeignScript)));
        (void)NativeProgram.Initialize(NativeGuarded);
        bool NativeReturnRejected = false;
        try { (void)NativeGuarded.Run(NativeProgram.Functions.at("probe")); }
        catch (const std::invalid_argument&) { NativeReturnRejected = true; }
        Check(NativeReturnRejected, "foreign native return entered another VM");
        try { (void)Guarded.Run(ForeignProgram.Functions.at("probe")); }
        catch (const std::invalid_argument&) {
            std::cout << "Import tests passed\n";
            return 0;
        }
        throw std::runtime_error("foreign script object accepted from import callback");
    } catch (const std::exception& Failure) {
        std::cerr << "Import tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
