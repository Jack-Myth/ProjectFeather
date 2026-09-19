#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>
#include <Feather/StdIo.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>

using namespace Feather;

namespace {

void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

Value Call(Value Object, const char* Name, std::vector<Value> Arguments = {}) {
    auto* Native = dynamic_cast<NativeObject*>(Object.AsObject());
    Check(Native != nullptr, "expected native standard library object");
    auto Function = Native->GetMember(Value::String(Name));
    Check(!Function.IsError(), "standard library method is missing");
    return dynamic_cast<NativeObject*>(Function.AsObject())->Call(Arguments);
}
Value Member(Value Object, const char* Name) {
    auto* Native = dynamic_cast<NativeObject*>(Object.AsObject());
    Check(Native != nullptr, "expected native standard library object");
    return Native->GetMember(Value::String(Name));
}

class SnapshotCall final : public NativeObject {
public:
    SnapshotCall(NativeObjectType& Type, Vm& Machine, std::vector<std::uint8_t>& Saved)
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

constexpr NativeModuleGuid SnapshotTestGuid{{
    0xe7, 0x43, 0xf8, 0x60, 0x8c, 0x2c, 0x42, 0x7e,
    0x8c, 0x6a, 0x83, 0x3b, 0x95, 0xe2, 0x67, 0x1d}};

class SnapshotCallType final : public NativeObjectType {
public:
    SnapshotCallType(Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObjectType(Machine, SnapshotTestGuid, "Checkpoint"), Saved(Saved) {}
    std::shared_ptr<SnapshotCall> Create() {
        return CreateObject<SnapshotCall>(GetVm(), Saved);
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || !Payload.empty())
            throw std::invalid_argument("invalid checkpoint payload");
        return Create();
    }
private:
    std::vector<std::uint8_t>& Saved;
};

} // namespace

int main() {
    try {
        std::istringstream Input("first line\n");
        std::ostringstream Output;
        StdIoLibrary Library(Input, Output);
        auto Stdio = Library.GetModule();
        auto Console = Member(Stdio, "Console");
        Check(!Call(Console, "Print", {Value::String("prompt: ")}).IsError(), "Print failed");
        Check(!Call(Console, "PrintLine", {Value::Number(42)}).IsError(), "PrintLine failed");
        Check(Output.str() == "prompt: 42\n", "console output mismatch");
        Check(Call(Console, "InputLine").AsString() == "first line", "InputLine mismatch");
        Check(Call(Console, "InputLine").GetType() == ValueType::Null, "EOF should return null");
        Check(Call(Console, "Print", {Member(Stdio, "IO")}).IsError(), "printing objects should fail");
        std::istringstream InvalidInput(std::string("\xff\n", 2));
        std::ostringstream IgnoredOutput;
        StdIoLibrary InvalidConsole(InvalidInput, IgnoredOutput);
        Check(Call(Member(InvalidConsole.GetModule(), "Console"), "InputLine").IsError(),
              "invalid UTF-8 console input should fail");

        auto Program = Compile(
            "def main() { stdio.Console.PrintLine(\"script\"); "
            "var f = stdio.IO.OpenFile(testPath, \"w+\"); "
            "stdio.IO.Write(f, \"A\\nB\"); "
            "stdio.IO.Seek(f, 0, \"start\"); "
            "var data = stdio.IO.Read(f, 3); "
            "var position = stdio.IO.Tell(f); "
            "stdio.IO.CloseFile(f); "
            "return data.Get(2) + position; } "
            "var stdio = import(\"stdio\");");
        Vm Machine(Program.Program);
        RegisterImport(Machine, [&Library](std::string_view Specifier) {
            return Specifier == "stdio" ? Library.GetModule() : Value{};
        });
        auto Path = std::filesystem::temp_directory_path() / "feather-stdio-test.bin";
        Machine.SetGlobal("testPath", Value::String(Path.string()));
        auto Result = Program.Initialize(Machine);
        Check(!Result.IsError(), "script initializer failed");
        Check(Machine.GetGlobal("Console").IsError() && Machine.GetGlobal("IO").IsError(),
              "standard IO should not be available as direct globals");
        Check(Machine.GetGlobal("stdio").AsObject() == Stdio.AsObject(),
              "top-level import did not create a global value");
        Result = Machine.Run(Program.Functions.at("main"));
        if (Result.IsError()) {
            auto* Failure = dynamic_cast<ErrorObject*>(Result.AsObject());
            throw std::runtime_error(Failure ? Failure->GetMessage() : "script returned Error");
        }
        Check(Result.GetType() == ValueType::Number && Result.AsNumber() == 69,
              "script file round trip failed");
        std::ifstream File(Path, std::ios::binary);
        std::string Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        Check(Bytes == std::string("A\nB", 3), "file bytes were changed");
        File.close();
        {
            std::ofstream Binary(Path, std::ios::binary | std::ios::trunc);
            Binary.write("\0\xff", 2);
        }
        auto IO = Member(Stdio, "IO");
        auto Handle = Call(IO, "OpenFile", {Value::String(Path.string()), Value::String("r")});
        Check(!Handle.IsError(), "binary file open failed");
        auto Buffer = Call(IO, "Read", {Handle, Value::Number(2)});
        Check(Call(Buffer, "Get", {Value::Number(1)}).AsNumber() == 255,
              "non-UTF-8 byte was changed");
        auto Empty = Call(IO, "Read", {Handle, Value::Number(2)});
        auto* NativeEmpty = dynamic_cast<NativeObject*>(Empty.AsObject());
        Check(NativeEmpty->GetMember(Value::String("Length")).AsNumber() == 0,
              "EOF should return an empty ByteBuffer");
        Check(Call(Buffer, "ToString").IsError(), "invalid UTF-8 must not become a string");
        Check(Call(IO, "Write", {Handle, Value::String("bad")}).IsError(),
              "read-only file should reject writes");
        Check(Call(Member(InvalidConsole.GetModule(), "IO"), "Tell", {Handle}).IsError(),
              "another library must reject a foreign file handle");
        Check(Call(IO, "CloseFile", {Handle}).AsBool(), "binary file close failed");
        Check(Call(IO, "CloseFile", {Handle}).IsError(), "double close should fail");

        auto SnapshotProgram = Compile(
            "var stdio = import(\"stdio\"); var handle = null; var buffer = null; "
            "def main() { var io = stdio.IO; var read = io.Read; "
            "handle = io.OpenFile(testPath, \"w+\"); io.Write(handle, \"ABC\"); "
            "io.Seek(handle, 0, \"start\"); buffer = read(handle, 2); "
            "var getter = buffer.Get; if (checkpoint()) { "
            "var tail = read(handle, 1); stdio.Console.PrintLine(\"restored\"); "
            "var result = getter(0) + buffer.Get(1) + tail.Get(0); "
            "io.CloseFile(handle); return result; } return 0; }");
        std::vector<std::uint8_t> Saved;
        {
            std::istringstream SnapshotInput;
            std::ostringstream SnapshotOutput;
            Vm Original(SnapshotProgram.Program);
            StdIoLibrary SnapshotIo(Original, SnapshotInput, SnapshotOutput);
            RegisterImport(Original, [&](std::string_view Name) {
                return Name == "stdio" ? SnapshotIo.GetModule() : Value{};
            });
            auto& CheckpointType = Original.CreateNativeObjectType<SnapshotCallType>(Saved);
            Original.RegisterNativeFunction("checkpoint", CheckpointType.Create());
            Original.SetGlobal("testPath", Value::String(Path.string()));
            Check(!SnapshotProgram.Initialize(Original).IsError(),
                  "snapshot stdio initializer failed");
            Check(Original.Run(SnapshotProgram.Functions.at("main")).AsNumber() == 0,
                  "snapshot stdio save path failed");
            Check(!Saved.empty(), "snapshot stdio bytes are missing");
        }
        {
            std::istringstream SnapshotInput;
            std::ostringstream SnapshotOutput;
            Vm Restored(SnapshotProgram.Program);
            StdIoLibrary SnapshotIo(Restored, SnapshotInput, SnapshotOutput);
            RegisterImport(Restored, [&](std::string_view Name) {
                return Name == "stdio" ? SnapshotIo.GetModule() : Value{};
            });
            Restored.CreateNativeObjectType<SnapshotCallType>(Saved);
            Check(Restored.ResumeSnapshot(Saved).AsNumber() == 198,
                  "stdio snapshot did not restore namespaces, methods, buffer, or file");
            Check(SnapshotOutput.str() == "restored\n",
                  "restored Console did not bind to the new host streams");
        }
        std::filesystem::remove(Path);
        {
            std::istringstream SnapshotInput;
            std::ostringstream SnapshotOutput;
            Vm MissingFile(SnapshotProgram.Program);
            StdIoLibrary SnapshotIo(MissingFile, SnapshotInput, SnapshotOutput);
            RegisterImport(MissingFile, [&](std::string_view Name) {
                return Name == "stdio" ? SnapshotIo.GetModule() : Value{};
            });
            MissingFile.CreateNativeObjectType<SnapshotCallType>(Saved);
            bool Rejected = false;
            try { (void)MissingFile.ResumeSnapshot(Saved); }
            catch (const std::runtime_error&) { Rejected = true; }
            Check(Rejected, "missing saved file should reject stdio snapshot restore");
            Check(MissingFile.GetGlobal("handle").IsError(),
                  "failed stdio restore committed VM globals");
        }

        Check(Call(IO, "OpenFile", {Value::String("unused"), Value::String("invalid")}).IsError(),
              "invalid mode should fail");
        Check(Call(IO, "Read", {Value{}, Value::Number(1)}).IsError(),
              "invalid handle should fail");
        Check(Library.GetModule().AsObject() == Stdio.AsObject(),
              "stdio import should preserve identity");
        Check(Member(Stdio, "unknown").IsError(), "unknown stdio member should fail");
        std::cout << "StdIo tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "StdIo tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
