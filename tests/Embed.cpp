#include <Feather/Runtime.hpp>

#include <iostream>
#include <stdexcept>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}
class DoubleNative final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::Number)
            return Value::FromObject(std::make_shared<ErrorObject>("expected one number"));
        return Value::Number(Arguments[0].AsNumber() * 2);
    }
};
}

int main() {
    try {
        auto Program = std::make_shared<Module>();
        auto Name = Program->AddString("double");
        auto Three = Program->AddNumber(3);
        Builder Code;
        Code.EmitU32(Op::GetGlobal, Name);
        Code.EmitU32(Op::Const, Three);
        Code.EmitU16(Op::Call, 1);
        Code.Emit(Op::Return);
        auto Body = std::make_shared<FunctionPrototype>();
        Body->Code = std::move(Code).Finish();
        auto Entry = Program->AddFunction(Body);

        Vm Machine(Program);
        Machine.RegisterNativeFunction("double", std::make_shared<DoubleNative>());
        Check(Machine.Run(Entry).AsNumber() == 6, "registered native call");
        Check(Machine.GetGlobal("double").GetObjectType() == ObjectType::Host,
              "host global lookup");
        auto Script = Machine.CreateScriptObject();
        Machine.SetGlobal("entity", Value::FromScript(Script));
        Check(Machine.CollectGarbage() == 0, "host global keeps script object");
        auto Handle = Machine.AddToRoot(Machine.GetGlobal("entity"));
        Machine.SetGlobal("entity", Value{});
        Check(Machine.CollectGarbage() == 0, "RootHandle keeps global result");
        Handle.Reset();
        Check(Machine.CollectGarbage() == 1, "released host value is collected");
        auto Missing = Machine.GetGlobal("unknown");
        Check(Missing.IsError() &&
              dynamic_cast<ErrorObject*>(Missing.AsObject())->GetMessage() == "undefined global",
              "host can inspect language Error");
        std::cout << "Embedding tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Embedding tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
