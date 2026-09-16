#include <Feather/Compiler.hpp>

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Feather;

namespace {

void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

template <typename Action>
void Reject(Action&& Try, const char* Message) {
    try { Try(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(Message);
}

class CaptureCall final : public NativeObject {
public:
    explicit CaptureCall(std::string& Text) : Text(Text) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
            throw std::runtime_error("invalid quick operator arguments");
        Text = Arguments[0].AsString();
        return {};
    }
private:
    std::string& Text;
};

} // namespace

int main() {
    try {
        auto Source = Compile("var base = 2; def score(x = 3) { return base + x; }");
        auto Bytes = SerializeProgram(Source);
        Check(Bytes.size() > 16 && Bytes[8] == 3 && Bytes[9] == 0,
              "unexpected current bytecode version");
        Check(Bytes == SerializeProgram(Source), "artifact is not deterministic");
        auto Loaded = DeserializeProgram(Bytes);
        Check(!Loaded.UsesQuickOperators && Loaded.Functions.contains("score"),
              "program metadata was lost");
        Vm Machine(Loaded.Program);
        Check(!Loaded.Initialize(Machine).IsError(), "loaded initializer failed");
        Check(Machine.Run(Loaded.Functions.at("score")).AsNumber() == 5,
              "loaded defaults or global binding changed");
        Check(Machine.Run(Loaded.Functions.at("score"), {Value::Number(8)}).AsNumber() == 10,
              "loaded function invocation changed");

        auto Exported = Compile("export var base = 2; export def score(x = 3) { return base + x; }");
        auto ExportBytes = SerializeProgram(Exported);
        auto ExportLoad = DeserializeProgram(ExportBytes);
        Check(ExportLoad.Exports.size() == 2 && ExportLoad.Exports[0] == "base" &&
              ExportLoad.Exports[1] == "score" && SerializeProgram(ExportLoad) == ExportBytes,
              "v3 export metadata did not round-trip deterministically");

        auto Quick = DeserializeProgram(SerializeProgram(Compile(">hello")));
        Check(Quick.UsesQuickOperators, "quick operator requirement was lost");
        Vm QuickMachine(Quick.Program);
        std::string Captured;
        QuickMachine.RegisterNativeFunction("__QuickOperatorRightAngleBucket",
            std::make_shared<CaptureCall>(Captured));
        (void)Quick.Initialize(QuickMachine);
        Check(Captured == "hello", "loaded quick operator did not reach host function");

        auto Corrupt = Bytes;
        Corrupt[0] ^= 1;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "bad magic accepted");
        Corrupt = Bytes; Corrupt[8] = 1;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "unsupported format accepted");
        Corrupt = Bytes; Corrupt[10] = 2;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "unsupported ISA accepted");
        Corrupt = Bytes; Corrupt[12] ^= 1;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "bad length accepted");
        Corrupt = Bytes; Corrupt.pop_back();
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "truncated artifact accepted");
        Corrupt = Bytes; Corrupt[Corrupt.size() - 5] = 2;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "invalid flags accepted");
        Corrupt = Bytes; Corrupt[20] = 255;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "invalid constant accepted");
        Reject([&] { (void)DeserializeProgram(Bytes, Bytes.size() - 1); },
               "artifact size limit ignored");

        auto BrokenEntry = Source;
        BrokenEntry.Initializer = 999'999;
        Reject([&] { (void)SerializeProgram(BrokenEntry); }, "invalid initializer encoded");

        auto Simple = SerializeProgram(Compile("def main() { return 1; }"));
        Check(Simple.size() > 37 && Simple[20] == 2, "unexpected function layout");
        Simple[37] = 255; // First instruction of the first function.
        Reject([&] { (void)DeserializeProgram(Simple); }, "invalid opcode accepted");

        std::cout << "Artifact tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Artifact tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
