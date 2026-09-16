#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>
#include <Feather/StdIo.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
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
        std::filesystem::remove(Path);

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
