#include <Feather/Runtime.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}
bool IsError(const Value& Input) {
    return Input.GetType() == ValueType::Object &&
           Input.AsObject()->GetObjectType() == ObjectType::Error;
}
std::uint32_t Function(std::shared_ptr<Module> Program, Builder Code,
                       std::uint32_t Parameters = 0) {
    auto Body = std::make_shared<FunctionPrototype>();
    Body->Code = std::move(Code).Finish();
    Body->ParameterCount = Parameters;
    Body->LocalCount = Parameters;
    Body->Defaults.resize(Parameters);
    return Program->AddFunction(Body);
}
class IndexNative final : public NativeObject {
public:
    explicit IndexNative(Value Result) : Result(std::move(Result)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        ++Calls;
        Check(Arguments.size() == 2, "__index argument count");
        Check(Arguments[0].AsObject()->GetObjectType() == ObjectType::Script,
              "__index self");
        return Result;
    }
    int Calls = 0;
private:
    Value Result;
};
class CallNative final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        Check(Arguments.size() == 2, "__call arguments");
        Check(Arguments[0].AsObject()->GetObjectType() == ObjectType::Script,
              "__call self");
        return Arguments[1];
    }
};
class MemberNative final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "saved") return Saved;
        return NativeObject::GetMember(Key);
    }
    Value SetMember(const Value& Key, const Value& Input) override {
        if (Key.GetType() != ValueType::String || Key.AsString() != "saved")
            return NativeObject::SetMember(Key, Input);
        Saved = Input;
        return Input;
    }
private:
    Value Saved;
};
void MemberInstructions() {
    auto Program = std::make_shared<Module>();
    auto Key = Program->AddString("answer");
    auto Number = Program->AddNumber(42);
    Builder Code;
    Code.Emit(Op::NewObject); Code.EmitU32(Op::SetLocal, 0); Code.Emit(Op::Pop);
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU32(Op::Const, Key);
    Code.EmitU32(Op::Const, Number); Code.Emit(Op::SetMember); Code.Emit(Op::Pop);
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU32(Op::Const, Key);
    Code.Emit(Op::GetMember); Code.Emit(Op::Return);
    auto Body = std::make_shared<FunctionPrototype>();
    Body->Code = std::move(Code).Finish(); Body->LocalCount = 1;
    auto Id = Program->AddFunction(Body);
    Vm Machine(Program);
    Check(Machine.Run(Id).AsNumber() == 42, "ScriptObject member roundtrip");

    auto ObjectValue = Machine.CreateScriptObject();
    Check(ObjectValue->SetRaw(Value::Number(+0.0), Value::Number(7)).AsNumber() == 7,
          "write +0 key");
    Check(ObjectValue->GetRaw(Value::Number(-0.0))->AsNumber() == 7, "read -0 key");
    auto Nan = Value::Number(std::numeric_limits<double>::quiet_NaN());
    Check(IsError(ObjectValue->SetRaw(Nan, Value::Number(1))), "NaN write Error");
    Check(!ObjectValue->GetRaw(Nan), "NaN not stored");
    ObjectValue->SetRaw(Value::String("present"), Value{});
    Check(ObjectValue->GetRaw(Value::String("present")).has_value(), "null is present member");
}
void Metafunctions() {
    auto Program = std::make_shared<Module>();
    auto Missing = Program->AddString("missing");
    auto Eight = Program->AddNumber(8);
    Builder Read;
    Read.EmitU32(Op::GetLocal, 0); Read.EmitU32(Op::Const, Missing);
    Read.Emit(Op::GetMember); Read.Emit(Op::Return);
    auto ReadId = Function(Program, std::move(Read), 1);
    Builder Invoke;
    Invoke.Emit(Op::NewObject); Invoke.EmitU32(Op::Const, Eight);
    Invoke.EmitU16(Op::Call, 1); Invoke.Emit(Op::Return);
    auto InvokeId = Function(Program, std::move(Invoke));
    Builder IndexBody;
    IndexBody.EmitU32(Op::GetLocal, 1); IndexBody.Emit(Op::Return);
    auto ScriptIndexId = Function(Program, std::move(IndexBody), 2);
    Builder Expose;
    Expose.EmitU32(Op::Const, ScriptIndexId); Expose.Emit(Op::Return);
    auto ExposeId = Function(Program, std::move(Expose));
    Vm Machine(Program);
    auto Root = Machine.GetRootMetaObject();
    auto First = std::make_shared<IndexNative>(Value::Number(11));
    Root->SetRaw(Value::String("__index"), Value::FromObject(First));
    auto ObjectValue = Machine.CreateScriptObject();
    Check(Machine.Run(ReadId, {Value::FromScript(ObjectValue)}).AsNumber() == 11,
          "shared RootMetaObject index");
    auto Second = std::make_shared<IndexNative>(Value::Number(12));
    Root->SetRaw(Value::String("__index"), Value::FromObject(Second));
    Check(Machine.Run(ReadId, {Value::FromScript(ObjectValue)}).AsNumber() == 12,
          "RootMetaObject mutation affects existing object");
    auto PresentNull = Machine.CreateScriptObject();
    PresentNull->SetRaw(Value::String("missing"), Value{});
    auto BeforeNull = Second->Calls;
    Check(Machine.Run(ReadId, {Value::FromScript(PresentNull)}).GetType() == ValueType::Null,
          "present null bypasses __index");
    Check(Second->Calls == BeforeNull, "__index called for present null");
    auto OriginalError = Value::FromObject(std::make_shared<ErrorObject>("original"));
    Root->SetRaw(Value::String("__index"),
                 Value::FromObject(std::make_shared<IndexNative>(OriginalError)));
    auto ErrorResult = Machine.Run(ReadId, {Value::FromScript(ObjectValue)});
    Check(IsError(ErrorResult) && ErrorResult.AsObject() == OriginalError.AsObject(),
          "__index Error result must be returned unchanged");
    Root->SetRaw(Value::String("__index"), Machine.Run(ExposeId));
    Check(Machine.Run(ReadId, {Value::FromScript(ObjectValue)}).AsString() == "missing",
          "script __index runs in a VM frame");
    auto Nan = Value::Number(std::numeric_limits<double>::quiet_NaN());
    Builder NaNRead; NaNRead.EmitU32(Op::GetLocal, 0);
    NaNRead.EmitU32(Op::Const, Program->AddNumber(Nan.AsNumber()));
    NaNRead.Emit(Op::GetMember); NaNRead.Emit(Op::Return);
    auto NaNId = Function(Program, std::move(NaNRead), 1);
    Vm WithNaN(Program);
    WithNaN.GetRootMetaObject()->SetRaw(Value::String("__index"), Value::FromObject(Second));
    auto Before = Second->Calls;
    Check(IsError(WithNaN.Run(NaNId, {Value::FromScript(WithNaN.CreateScriptObject())})),
          "NaN read Error");
    Check(Second->Calls == Before, "NaN must not call __index");

    Root->SetRaw(Value::String("__call"), Value::FromObject(std::make_shared<CallNative>()));
    Check(Machine.Run(InvokeId).AsNumber() == 8, "__call receives self and args");
    Root->SetRaw(Value::String("__call"), Machine.Run(ExposeId));
    Check(Machine.Run(InvokeId).AsNumber() == 8, "script __call runs in VM frame");
    Root->SetRaw(Value::String("__call"), Value::Number(1));
    Check(IsError(Machine.Run(InvokeId)), "invalid __call returns Error");

    auto Custom = Machine.CreateMetaObject();
    Custom->SetRaw(Value::String("__index"), Value::FromObject(First));
    Machine.SetMetaObject(ObjectValue, Custom);
    Check(Machine.Run(ReadId, {Value::FromScript(ObjectValue)}).AsNumber() == 11,
          "replaced MetaObject");
    Vm NoIndex(Program);
    auto NoIndexObject = NoIndex.CreateScriptObject();
    Check(IsError(NoIndex.Run(ReadId, {Value::FromScript(NoIndexObject)})),
          "missing __index returns Error");
}
void NativeAndErrorMembers() {
    auto Program = std::make_shared<Module>();
    auto Key = Program->AddString("saved");
    auto ValueId = Program->AddNumber(5);
    Builder Code;
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU32(Op::Const, Key);
    Code.EmitU32(Op::Const, ValueId); Code.Emit(Op::SetMember); Code.Emit(Op::Pop);
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU32(Op::Const, Key);
    Code.Emit(Op::GetMember); Code.Emit(Op::Return);
    auto Id = Function(Program, std::move(Code), 1);
    Vm Machine(Program);
    auto Host = std::make_shared<MemberNative>();
    Check(Machine.Run(Id, {Value::FromObject(Host)}).AsNumber() == 5,
          "NativeObject get/set_member");
    Check(IsError(Host->SetMember(Value::String("other"), Value::Number(6))),
          "NativeObject write failure");

    Builder Message;
    Message.EmitU32(Op::GetLocal, 0);
    Message.EmitU32(Op::Const, Program->AddString("message"));
    Message.Emit(Op::GetMember); Message.Emit(Op::Return);
    auto MessageId = Function(Program, std::move(Message), 1);
    Vm Messages(Program);
    Check(Messages.Run(MessageId, {Value::FromObject(std::make_shared<ErrorObject>("oops"))}).AsString() == "oops",
          "Error.message member");
}
} // namespace

int main() {
    try {
        MemberInstructions(); Metafunctions(); NativeAndErrorMembers();
        std::cout << "M2 tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "M2 tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
