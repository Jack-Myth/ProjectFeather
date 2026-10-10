#include <Feather/Checkpoint.hpp>
#include <Feather/Compiler.hpp>
#include <Feather/StdIo.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace Feather;
namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}
template<class Action> void Reject(Action Run, const char* Message) {
    try { Run(); } catch (const std::exception&) { return; }
    throw std::runtime_error(Message);
}
Value Evaluate(const std::string& Expression) {
    auto Program = Compile("def main() { return " + Expression + "; }");
    Vm Machine(Program.Program);
    Program.Initialize(Machine);
    return Machine.Run(Program.Functions.at("main"));
}
Value Call(Value Object, const char* Name, std::vector<Value> Arguments = {}) {
    auto Method = dynamic_cast<NativeObject*>(Object.AsObject())->GetMember(Value::String(Name));
    return dynamic_cast<NativeObject*>(Method.AsObject())->Call(Arguments);
}
Value Member(Value Object, const char* Name) {
    return dynamic_cast<NativeObject*>(Object.AsObject())->GetMember(Value::String(Name));
}
class Capture final : public NativeObject {
public:
    Capture(NativeObjectType& Type, Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObject(Type), Machine(Machine), Saved(Saved) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        Saved = Machine.CaptureSnapshot();
        return Value::Bool(false);
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
};
}
int main(int Argc, char** Argv) {
    try {
        static_assert(sizeof(std::uint8_t) == 1);
        for (unsigned I = 0; I < 256; ++I) {
            auto Original = Value::Byte(I);
            auto Copy = Original;
            Value Moved(std::move(Copy));
            Copy = Value::String("replace");
            Copy = Moved;
            Check(Copy.GetType() == ValueType::Byte && Copy.AsByte() == I &&
                  Copy.AsNumber() == I && Copy.IsTruthy() == (I != 0), "byte value/copy/truth");
        }
        for (double Bad : {-1.0, 256.0, 0.5, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()})
            Reject([&] { (void)Value::Byte(Bad); }, "invalid native byte accepted");
        Reject([] { (void)Value::Number(1).AsByte(); }, "AsByte accepted number");
        Check(Evaluate("byte(0)").AsByte() == 0, "zero conversion");
        Check(Evaluate("byte(255)").AsByte() == 255, "255 conversion");
        Check(Evaluate("byte(byte(128))").AsByte() == 128, "idempotent conversion");
        for (const auto* Bad : {"-1", "256", "1.5", "1 / 0", "0 / 0",
                                "null", "true", "\"1\"", "object()"})
            Check(Evaluate(std::string("byte(") + Bad + ")").IsError(), "invalid conversion accepted");
        for (const auto* Bad : {"byte()", "byte(1, 2)", "byte(1", "byte"})
            Reject([&] { (void)Evaluate(Bad); }, "invalid byte syntax accepted");
        Reject([] { (void)Compile("def f(x = byte(256)) {}"); }, "invalid default accepted");
        Reject([] { (void)Compile("def f(x = byte(1.5)) {}"); }, "fractional default accepted");
        Reject([] { (void)Compile("def f(x = byte(true)) {}"); }, "non-numeric default accepted");
        for (const auto& [Expression, Expected] :
             std::vector<std::pair<std::string, double>>{
                 {"byte(255) + byte(1)", 256}, {"byte(0) - byte(1)", -1},
                 {"byte(255) * 2", 510}, {"2 + byte(255)", 257},
                 {"byte(3) / byte(2)", 1.5}, {"-byte(255)", -255}}) {
            auto Result = Evaluate(Expression);
            Check(Result.GetType() == ValueType::Number && Result.AsNumber() == Expected,
                  "arithmetic must promote to number");
        }
        Check(Evaluate("byte(255) == 255").AsBool(), "mixed numeric equality");
        Check(Evaluate("byte(1) != true").AsBool(), "bool is not numeric");
        Check(Evaluate("byte(1) < 2").AsBool(), "mixed numeric ordering");
        Check(Evaluate("byte(256 - 1)").AsByte() == 255, "computed conversion");
        Check(Evaluate("byte(byte(255) + 1)").IsError(), "overflow was silently wrapped");
        Check(std::isinf(Evaluate("byte(1) / byte(0)").AsNumber()), "division semantics changed");

        auto Program = Compile(R"(
            var global = byte(255);
            def pass(x = byte(128)) { return x; }
            def main() {
                var a = object();
                a[byte(0)] = global;
                a.field = pass();
                a[1] = pass(a[0]);
                return pass(a[byte(1)]);
            }
        )");
        // A host-created byte constant must use one payload byte, too.
        Program.Program->AddByte(42);
        auto Bytes = SerializeProgram(Program);
        auto Loaded = DeserializeProgram(Bytes);
        Check(Bytes == SerializeProgram(Loaded), "byte artifact round-trip");
        auto Symbols = SerializeSymbols(Program);
        AttachSymbols(Loaded, Symbols);
        Vm Machine(Loaded.Program);
        Loaded.Initialize(Machine);
        Check(Machine.Run(Loaded.Functions.at("main")).AsByte() == 255, "assignment/field/index/call");
        Check(Machine.Run(Loaded.Functions.at("pass")).AsByte() == 128, "byte default round-trip");
        auto Compact = Compile("def f(x = byte(1)) { return x; }");
        auto Wide = Compile("def f(x = 1) { return x; }");
        Check(SerializeProgram(Wide).size() == SerializeProgram(Compact).size() + 7,
              "serialized byte default is not one byte");
        auto NumberConstant = Compile("");
        NumberConstant.Program->AddNumber(1);
        auto ByteConstant = Compile("");
        ByteConstant.Program->AddByte(1);
        Check(SerializeProgram(NumberConstant).size() == SerializeProgram(ByteConstant).size() + 7,
              "serialized byte constant is not one byte");
        auto Truncated = SerializeProgram(ByteConstant);
        Truncated.resize(Truncated.size() - 1);
        Reject([&] { (void)DeserializeProgram(Truncated); }, "truncated byte artifact accepted");
        auto Old = Bytes; Old[8] = 3;
        Reject([&] { (void)DeserializeProgram(Old); }, "old bytecode version accepted");
        auto Broken = Compile("def main() { return byte(1); }");
        Broken.Program->Constants[Broken.Functions.at("main")].Function->Code =
            {static_cast<std::uint8_t>(Op::ToByte), static_cast<std::uint8_t>(Op::Return)};
        Reject([&] { Broken.Program->Validate(); }, "ToByte underflow accepted");

        auto SnapshotProgram = Compile(R"(
            var global = byte(255);
            var container = object();
            container[byte(1)] = byte(128);
            def pick(first, second) { return first; }
            def main() { var n = 7; var local = byte(n); return pick(local, capture()); }
        )");
        Vm SnapshotMachine(SnapshotProgram.Program);
        std::vector<std::uint8_t> Saved;
        auto& CaptureType = SnapshotMachine.CreateNativeObjectType<NativeSingletonType>(
            NativeModuleGuid{{0x42}}, "ByteTestCapture");
        SnapshotMachine.RegisterNativeFunction("capture",
            CaptureType.Create<Capture>(SnapshotMachine, Saved));
        SnapshotProgram.Initialize(SnapshotMachine);
        Check(SnapshotMachine.Run(SnapshotProgram.Functions.at("main")).AsByte() == 7, "capture path");
        Vm Restored(SnapshotProgram.Program);
        auto& RestoredType = Restored.CreateNativeObjectType<NativeSingletonType>(
            NativeModuleGuid{{0x42}}, "ByteTestCapture");
        Restored.RegisterNativeFunction("capture", RestoredType.Create<Capture>(Restored, Saved));
        Check(Restored.ResumeSnapshot(Saved).AsByte() == 7, "snapshot local/operand stack");
        Check(Restored.GetGlobal("global").AsByte() == 255, "snapshot byte global");
        Check(Restored.GetGlobal("container").AsScriptObject()
                  ->GetRaw(Value::Number(1))->AsByte() == 128, "snapshot byte field");
        auto OldSnapshot = Saved; OldSnapshot[4] = 4;
        Vm RejectOld(SnapshotProgram.Program);
        Reject([&] { RejectOld.ResumeSnapshot(OldSnapshot); }, "old snapshot accepted");

        // Checkpoints intentionally preserve current locals; their saved operand stack retains byte.
        auto CheckpointProgram = Compile(R"(
            var done = false; var id = null;
            def pick(first, second) { return first; }
            def main() {
                var saved = pick(byte(255), id = checkpoint.Capture());
                if (done) { return saved; }
                done = true;
                checkpoint.Restore(id);
                return null;
            }
        )");
        Vm CheckpointMachine(CheckpointProgram.Program);
        CheckpointLibrary Checkpoints(CheckpointMachine);
        CheckpointMachine.SetGlobal("checkpoint", Checkpoints.GetModule());
        CheckpointProgram.Initialize(CheckpointMachine);
        Check(CheckpointMachine.Run(CheckpointProgram.Functions.at("main")).AsByte() == 255,
              "checkpoint byte operand stack");

        Check(Argc == 2, "missing work file path");
        auto Path = std::filesystem::path(Argv[1]);
        std::filesystem::create_directories(Path.parent_path());
        std::istringstream Input;
        std::ostringstream Output;
        {
            auto IoProgram = Compile("");
            Vm IoMachine(IoProgram.Program);
            auto Library = std::make_unique<StdIoLibrary>(IoMachine, Input, Output);
            auto IO = Member(Library->GetModule(), "IO");
            auto File = Call(IO, "OpenFile", {Value::String(Path.string()), Value::String("w+")});
            Check(!File.IsError(), "open byte test file");
            for (unsigned I = 0; I < 256; ++I)
                Check(Call(IO, "Write", {File, Value::Byte(I)}).AsNumber() == 1, "single byte write");
            Check(Call(IO, "Tell", {File}).AsNumber() == 256, "bytes must occupy exactly 256 bytes");
            Check(!Call(IO, "Seek", {File, Value::Byte(0), Value::String("start")}).IsError(), "byte offset");
            auto Buffer = Call(IO, "Read", {File, Value::Number(256)});
            Check(Member(Buffer, "Length").AsNumber() == 256, "packed buffer length");
            for (unsigned I = 0; I < 256; ++I)
                Check(Call(Buffer, "Get", {Value::Byte(I)}).AsByte() == I, "buffer unsigned byte");
            Check(Call(Buffer, "Get", {Value::Number(256)}).IsError(), "out-of-range buffer index");
            Check(Call(Buffer, "ToString").IsError(), "invalid UTF-8 bytes accepted as string");
            Check(!Call(IO, "CloseFile", {File}).IsError(), "close byte test file");
            Check(!Call(Member(Library->GetModule(), "Console"), "PrintLine",
                        {Value::Byte(255)}).IsError(), "print byte");
        }
        Check(Output.str() == "255\n", "byte printed as character");
        std::filesystem::remove(Path);
        std::cout << "Byte tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << Failure.what() << '\n';
        return 1;
    }
}
