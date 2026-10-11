#include <Feather/Compiler.hpp>
#include <iostream>
#include <stdexcept>
using namespace Feather;
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}
class Capture final : public NativeObject {
public:
    Capture(NativeObjectType& Type, Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObject(Type), Machine(Machine), Saved(Saved) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        Saved = Machine.CaptureSnapshot(); return {};
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
};
int main() {
    try {
        auto Program = Compile(R"(
            var alias = get_type;
            def f() { return null; }
            def inspect(v) { return alias(v); }
            def main() {
                var r = object();
                r[0] = get_type(null); r[1] = get_type(false);
                r[2] = get_type(byte(255)); r[3] = get_type(255);
                r[4] = get_type("hello"); r[5] = get_type(object());
                r[6] = get_type(f); r[7] = get_type(byte(256));
                r[8] = get_type(get_type); r[9] = get_type(get_type());
                r[10] = get_type(get_type(1, 2));
                r[11] = get_type(byte(1) + byte(2));
                return r;
            }
        )");
        Vm Machine(Program.Program);
        Program.Initialize(Machine);
        auto Result = Machine.Run(Program.Functions.at("main"));
        auto Root = Machine.AddToRoot(Result);
        const char* Expected[] = {"null", "bool", "byte", "number", "string",
            "ScriptObject", "Function", "Error", "NativeObject", "Error", "Error", "number"};
        for (int I = 0; I < 12; ++I) {
            auto Item = Result.AsScriptObject()->GetRaw(Value::Number(I));
            Check(Item && Item->AsString() == Expected[I], "type name mismatch");
        }
        Machine.SetGlobal("get_type", Value::Number(7));
        Check(Machine.Run(Program.Functions.at("inspect"), {Value::Byte(0)}).AsString() == "byte",
              "first-class builtin alias");
        auto Child = Compile("export def inspect() { return get_type(byte(1)); }");
        Child.LoadInto(Machine, "child");
        Child.InitializeModule(Machine, "child");
        auto Fn = Machine.GetModuleGlobal("child", "inspect");
        Check(Machine.Call(Fn, {}).AsString() == "byte", "module builtin isolated from root shadow");
        auto SnapshotProgram = Compile(R"(
            var alias = get_type;
            def main() { capture(); return alias(byte(255)); }
        )");
        std::vector<std::uint8_t> Saved;
        Vm Original(SnapshotProgram.Program);
        auto& CaptureType = Original.CreateNativeObjectType<NativeSingletonType>(
            NativeModuleGuid{{0x71}}, "TypeTestCapture");
        Original.RegisterNativeFunction("capture", CaptureType.Create<Capture>(Original, Saved));
        SnapshotProgram.Initialize(Original);
        Check(Original.Run(SnapshotProgram.Functions.at("main")).AsString() == "byte", "capture");
        Vm Restored(SnapshotProgram.Program);
        auto& RestoredType = Restored.CreateNativeObjectType<NativeSingletonType>(
            NativeModuleGuid{{0x71}}, "TypeTestCapture");
        Restored.RegisterNativeFunction("capture", RestoredType.Create<Capture>(Restored, Saved));
        Check(Restored.ResumeSnapshot(Saved).AsString() == "byte", "restored builtin alias");
        auto Loaded = DeserializeProgram(SerializeProgram(Program));
        Vm BytecodeMachine(Loaded.Program);
        Loaded.Initialize(BytecodeMachine);
        Check(!BytecodeMachine.Run(Loaded.Functions.at("main")).IsError(), "bytecode builtin");
        std::cout << "get_type passed\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << Error.what() << '\n'; return 1;
    }
}
