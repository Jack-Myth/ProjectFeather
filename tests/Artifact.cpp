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

void CheckOptimizedInstructions() {
    auto Source = Compile(R"(
        def update(x, y) { x = x + y; return x; }
        def doubled(x) { x = x + x; return x; }
        def increment(x) { x = x + 1; return x; }
        def score(o) { return (o.value * 3 + 4) - 2; }
    )");
    auto HasOp = [&](const char* Name, Op Expected) {
        const auto& Body = *Source.Program->Constants.at(Source.Functions.at(Name)).Function;
        for (const auto& Location : Body.Locations)
            if (Body.Code.at(Location.Pc) == static_cast<std::uint8_t>(Expected)) return true;
        return false;
    };
    Check(HasOp("update", Op::AddToLocal) && HasOp("increment", Op::AddConstToLocal) &&
          HasOp("score", Op::GetField) && HasOp("score", Op::MulConst) &&
          HasOp("score", Op::AddConst) && HasOp("score", Op::SubConst), "optimized opcodes missing");
    auto Bytes = SerializeProgram(Source);
    auto Loaded = DeserializeProgram(Bytes);
    AttachSymbols(Loaded, SerializeSymbols(Source));
    Vm Machine(Loaded.Program);
    auto Update = Loaded.Functions.at("update");
    Check(Machine.Run(Loaded.Functions.at("doubled"), {Value::Number(1.25)}).AsNumber() == 2.5,
          "aliased number operands changed");
    Check(Machine.Run(Update, {Value::Number(1.5), Value::Byte(2)}).AsNumber() == 3.5,
          "mixed local addition lost byte promotion");
    Check(Machine.Run(Update, {Value::String("a"), Value::String("b")}).AsString() == "ab",
          "local string concatenation changed");
    Check(Machine.Run(Loaded.Functions.at("doubled"), {Value::Byte(7)}).AsNumber() == 14,
          "aliased byte operands changed");
    auto Error = Machine.Run(Loaded.Functions.at("increment"), {Value::String("x")});
    Check(Error.IsError() && Machine.GetErrorLocation(Error).has_value(), "local addition lost error origin");
    auto Item = Machine.CreateScriptObject();
    Item->SetRaw(Value::String("value"), Value::Byte(5));
    Check(Machine.Run(Loaded.Functions.at("score"), {Value::FromScript(Item)}).AsNumber() == 17,
          "constant field or arithmetic changed");
    Bytes[10] = 2;
    Reject([&] { DeserializeProgram(Bytes); }, "new opcode accepted under old ISA");
    auto Legacy = Compile("def answer() { return 42; }");
    Legacy.Program->InstructionVersion = 2;
    auto OldBytes = SerializeProgram(Legacy);
    auto OldProgram = DeserializeProgram(OldBytes);
    AttachSymbols(OldProgram, SerializeSymbols(Legacy));
    Check(SerializeProgram(OldProgram) == OldBytes, "old bytecode identity changed");
}

} // namespace

int main() {
    try {
        CheckOptimizedInstructions();
        auto Source = Compile("var base = 2; def score(x = 3) { return base + x; }");
        auto Bytes = SerializeProgram(Source);
        Check(Bytes.size() > 16 && Bytes[8] == 6 && Bytes[9] == 0,
              "unexpected current bytecode version");
        Check(Bytes == SerializeProgram(Source), "artifact is not deterministic");
        const auto Cached = SerializeProgram(Source, 4096, 42);
        auto CachedProgram = DeserializeProgram(Cached, 4096, 42);
        Check(SerializeProgram(CachedProgram) == Bytes, "source size changed program identity");
        AttachSymbols(CachedProgram, SerializeSymbols(Source));
        Reject([&] { DeserializeProgram(Cached, 4096, 41); }, "wrong source size accepted");
        Reject([&] { DeserializeProgram(Bytes, 4096, 42); }, "cache without source size accepted");
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
              "v4 export metadata did not round-trip deterministically");

        auto Repeated = Compile("var g = 1; def a() { g = g + byte(1); } def b() { g = g + byte(1); }");
        std::size_t Names = 0, BytesOfOne = 0, NumbersOfOne = 0;
        for (const auto& Item : Repeated.Program->Constants) {
            Names += Item.Type == Constant::Kind::String && Item.Text == "g";
            BytesOfOne += Item.Type == Constant::Kind::Byte && Item.ByteValue == 1;
            NumbersOfOne += Item.Type == Constant::Kind::Number && Item.Numeric == 1;
        }
        Check(Names == 1 && BytesOfOne == 1 && NumbersOfOne == 1, "typed constants must be shared across functions");
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
        Corrupt = Bytes; Corrupt[8] = 5;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "unsupported format accepted");
        Corrupt = Bytes; Corrupt[10] = 255;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "unsupported ISA accepted");
        Corrupt = Bytes; Corrupt[12] ^= 1;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "bad length accepted");
        Corrupt = Bytes; Corrupt.pop_back();
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "truncated artifact accepted");
        Corrupt = Bytes; Corrupt[Corrupt.size() - 5] = 2;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "invalid flags accepted");
        Corrupt = Bytes; Corrupt[28] = 255;
        Reject([&] { (void)DeserializeProgram(Corrupt); }, "invalid constant accepted");
        Reject([&] { (void)DeserializeProgram(Bytes, Bytes.size() - 1); },
               "artifact size limit ignored");

        auto BrokenEntry = Source;
        BrokenEntry.Initializer = 999'999;
        Reject([&] { (void)SerializeProgram(BrokenEntry); }, "invalid initializer encoded");

        auto Simple = SerializeProgram(Compile("def main() { return 1; }"));
        Check(Simple.size() > 45 && Simple[28] == 2, "unexpected function layout");
        Simple[45] = 255; // First instruction of the first function.
        Reject([&] { (void)DeserializeProgram(Simple); }, "invalid opcode accepted");

        std::cout << "Artifact tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Artifact tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
