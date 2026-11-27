#include <Feather/ByteBuffer.hpp>
#include <Feather/Compiler.hpp>
#include <Feather/NativeModule.hpp>

#include <array>
#include <future>
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

void CheckReentrantArguments() {
    auto Program = Compile(R"(
        def inner(x) { get_type(x); return x; }
        def outer() { return host.reenter(3, 19, null, "retained"); }
    )");
    Vm Machine(Program.Program);
    NativeModuleGuid Id{{1}};
    NativeModuleBuilder Host(Machine, Id, "host");
    Host.AddFunction("reenter", [&](const std::vector<Value>& Args) {
        Check(Args.size() == 4 && Args[0].AsNumber() == 3, "outer native arguments");
        auto Result = Machine.Run(Program.Functions.at("inner"), {Value::Number(42)});
        Check(Result.AsNumber() == 42 && Args.size() == 4 && Args[3].AsString() == "retained",
              "nested native call overwrote outer arguments");
        return Args[1];
    });
    Machine.SetGlobal("host", Host.Build());
    Program.Initialize(Machine);
    Check(Machine.Run(Program.Functions.at("outer")).AsNumber() == 19, "reentrant return");
}

void CheckMessageArchive() {
    auto Program = std::make_shared<Module>();
    std::vector<std::uint8_t> Packet;
    {
        Vm Sender(Program);
        auto* State = Sender.CreateScriptObject();
        auto* Items = Sender.CreateArray();
        auto* Meta = Sender.CreateMetaObject();
        auto Buffer = ByteBufferLibrary(Sender).Create(std::string("a\0b", 3));
        // FTM1 encoding from before the buffer type identity moved into the runtime.
        const std::vector<std::uint8_t> LegacyPacket{
            'F','T','M','1', 1,0,0,0, 1,0,0,0, 9,
            0x2d,0x33,0xd7,0x69,0x4a,0x83,0x46,0xc0,0xb5,0x87,0x65,0x72,0x47,0x4f,0x1f,0xd2,
            10,0,0,0, 'B','y','t','e','B','u','f','f','e','r', 3,0,0,0, 'a',0,'b', 5,0,0,0,0};
        Check(Sender.SerializeArguments(std::array{Buffer}) == LegacyPacket,
              "ByteBuffer message encoding changed");
        Sender.SetMetaObject(State, Meta);
        Meta->SetRaw(Value::String("owner"), Value::FromScript(State));
        Items->AppendArray(Value::FromScript(State));
        Items->AppendArray(Buffer);
        State->SetRaw(Value::Number(2.5), Value::FromScript(Items));
        std::array Arguments{Value{}, Value::Bool(true), Value::Byte(42), Value::Number(42),
                             Value::String("message"), Value::FromScript(State),
                             Value::FromScript(Items), Buffer};
        Packet = Sender.SerializeArguments(Arguments);
        Items->RemoveArrayAt(0);
    }

    auto Worker = std::async(std::launch::async, [Packet = std::move(Packet)] {
        Vm Receiver(std::make_shared<Module>());
        const auto Arguments = Receiver.DeserializeArguments(Packet);
        Check(Arguments.size() == 8 && Arguments[0].GetType() == ValueType::Null &&
                  Arguments[1].AsBool() && Arguments[2].GetType() == ValueType::Byte &&
                  Arguments[2].AsByte() == 42 && Arguments[3].GetType() == ValueType::Number &&
                  Arguments[3].AsNumber() == 42 && Arguments[4].AsString() == "message",
              "message preserves argument order and types");
        auto Root = Receiver.AddToRoot(Arguments[5]);
        Receiver.CollectGarbage();
        auto* State = Root.Get().AsScriptObject();
        auto* Items = Arguments[6].AsScriptObject();
        auto* Meta = State->GetMetaObject();
        Check(State->GetRaw(Value::Number(2.5))->AsScriptObject() == Items &&
                  Items->GetArrayElements()[0].AsScriptObject() == State &&
                  Meta->GetRaw(Value::String("owner"))->AsScriptObject() == State &&
                  Meta->GetMetaObject() == Receiver.GetRootMetaObject(),
              "message preserves shared references, cycles and MetaObjects");
        Check(Items->GetArrayElements()[1].AsObject() == Arguments[7].AsObject() &&
                  *ByteBufferData(Arguments[7]) == std::string("a\0b", 3) &&
                  &static_cast<NativeObject*>(Arguments[7].AsObject())->GetNativeObjectType()->GetVm() == &Receiver,
              "binary data uses the receiver's native type");
        try {
            Receiver.DeserializeArguments(std::span(Packet).first(Packet.size() - 1));
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("truncated message archive was accepted");
    });
    Worker.get();
}
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
        CheckReentrantArguments();
        CheckMessageArchive();
        std::cout << "Embedding tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Embedding tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
