#include <Feather/Compiler.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

class SharedErrorCall final : public NativeObject {
public:
    explicit SharedErrorCall(Value Result) : Result(std::move(Result)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override { return Result; }
private:
    Value Result;
};

class ThrowCall final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override { throw std::runtime_error("native failure"); }
};

class NestedFaultCall final : public NativeObject {
public:
    NestedFaultCall(Vm& Machine, std::uint32_t Entry, bool Replace)
        : Machine(Machine), Entry(Entry), Replace(Replace) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        if (!Replace) {
            try { return Machine.Run(Entry); }
            catch (...) {
                auto Source = Machine.GetFaultLocation();
                ObservedLine = Source ? Source->Line : 0;
                throw;
            }
        }
        try { return Machine.Run(Entry); }
        catch (const std::runtime_error&) { throw std::runtime_error("native replacement"); }
    }
    std::size_t ObservedLine = 0;
private:
    Vm& Machine;
    std::uint32_t Entry;
    bool Replace;
};
}

int main() {
    try {
        constexpr std::string_view Text = "def helper() {\n  return 1 + \"x\";\n}\ndef main() { return helper(); }\n";
        auto Source = Compile(Text);
        Vm Machine(Source.Program);
        Check(!Source.Initialize(Machine).IsError(), "initializer failed");
        auto Error = Machine.Run(Source.Functions.at("main"));
        Check(Error.IsError(), "expected language Error");
        auto Origin = Machine.GetErrorLocation(Error);
        Check(Origin && Origin->ByteOffset == Text.find(" + ") + 1 &&
              Origin->Line == 2 && Origin->Column == 12,
              "cross-function Error lost its operator location");

        auto Loaded = DeserializeProgram(SerializeProgram(Source));
        Vm LoadedMachine(Loaded.Program);
        Check(!Loaded.Initialize(LoadedMachine).IsError(), "bytecode initializer failed");
        auto LoadedError = LoadedMachine.Run(Loaded.Functions.at("main"));
        Check(LoadedError.IsError() && !LoadedMachine.GetErrorLocation(LoadedError),
              "bare bytecode should use the no-location fallback");

        auto Symbols = SerializeSymbols(Source);
        Check(Symbols == SerializeSymbols(Source), "symbol artifact is not deterministic");
        auto WithSymbols = DeserializeProgram(SerializeProgram(Source));
        AttachSymbols(WithSymbols, Symbols);
        Vm SymbolMachine(WithSymbols.Program);
        Check(!WithSymbols.Initialize(SymbolMachine).IsError(), "symbol initializer failed");
        auto SymbolError = SymbolMachine.Run(WithSymbols.Functions.at("main"));
        auto SymbolOrigin = SymbolMachine.GetErrorLocation(SymbolError);
        Check(SymbolOrigin && SymbolOrigin->ByteOffset == Origin->ByteOffset &&
              SymbolOrigin->Line == Origin->Line && SymbolOrigin->Column == Origin->Column,
              "symbol artifact did not restore cross-function location");
        Check(WithSymbols.Program->Constants.at(WithSymbols.Functions.at("main"))
                  .Function->DebugName == "main",
              "symbol artifact did not restore function names");

        auto Named = Compile("def inspect(arg) { var local = arg; return local; }");
        auto NamedBytes = SerializeProgram(Named);
        auto NamedLoaded = DeserializeProgram(NamedBytes);
        Check(SerializeProgram(NamedLoaded) == NamedBytes,
              "debug names changed the bytecode artifact");
        Check(NamedLoaded.Program->Constants.at(NamedLoaded.Functions.at("inspect"))
                  .Function->LocalVariables.empty(),
              "bytecode unexpectedly retained debug locals");
        AttachSymbols(NamedLoaded, SerializeSymbols(Named));
        const auto& NamedBody = *NamedLoaded.Program->Constants.at(
            NamedLoaded.Functions.at("inspect")).Function;
        Check(NamedBody.DebugName == "inspect" && NamedBody.LocalVariables.size() == 2 &&
              NamedBody.LocalVariables[0].Name == "arg" &&
              NamedBody.LocalVariables[0].StartPc == 0 &&
              NamedBody.LocalVariables[0].EndPc == NamedBody.Code.size() &&
              NamedBody.LocalVariables[1].Name == "local" &&
              NamedBody.LocalVariables[1].StartPc < NamedBody.LocalVariables[1].EndPc,
              "symbol artifact did not restore local variable scopes");

        auto Wrong = DeserializeProgram(SerializeProgram(Compile("def main() { return 2; }")));
        try { AttachSymbols(Wrong, Symbols); throw std::runtime_error("mismatched symbols accepted"); }
        catch (const std::invalid_argument&) {}
        Check(Wrong.Program->Constants[Wrong.Functions.at("main")].Function->Locations.empty(),
              "mismatched symbols mutated the program");
        auto Damaged = Symbols;
        Damaged[Damaged.size() - 1] ^= 1;
        try { AttachSymbols(Loaded, Damaged); throw std::runtime_error("damaged symbols accepted"); }
        catch (const std::invalid_argument&) {}
        Check(Loaded.Program->Constants[Loaded.Functions.at("main")].Function->Locations.empty(),
              "damaged symbols mutated the program");

        constexpr std::string_view NativeText = "def main() { return fail(); } def okay() { return 1; }";
        auto NativeProgram = Compile(NativeText);
        Vm NativeVm(NativeProgram.Program);
        NativeVm.RegisterNativeFunction("fail", std::make_shared<ThrowCall>());
        NativeProgram.Initialize(NativeVm);
        try { (void)NativeVm.Run(NativeProgram.Functions.at("main"));
              throw std::runtime_error("native fault did not propagate"); }
        catch (const std::runtime_error& Failure) {
            Check(std::string_view(Failure.what()) == "native failure",
                  "native exception type or message changed");
        }
        auto NativeFault = NativeVm.GetFaultLocation();
        Check(NativeFault && NativeFault->ByteOffset == NativeText.find("fail(") + 4 &&
              NativeFault->Line == 1,
              "native fault did not identify its call site");
        Check(NativeVm.Run(NativeProgram.Functions.at("okay")).AsNumber() == 1 &&
              !NativeVm.GetFaultLocation(), "successful outer Run retained a stale fault");

        auto BareNative = DeserializeProgram(SerializeProgram(NativeProgram));
        Vm BareVm(BareNative.Program);
        BareVm.RegisterNativeFunction("fail", std::make_shared<ThrowCall>());
        BareNative.Initialize(BareVm);
        try { (void)BareVm.Run(BareNative.Functions.at("main")); }
        catch (const std::runtime_error&) {}
        Check(!BareVm.GetFaultLocation(), "bare bytecode unexpectedly had fault symbols");

        constexpr std::string_view NestedText =
            "def inner() {\n  while (true) { }\n}\n"
            "def main() { return trigger(); }\n";
        auto Nested = Compile(NestedText);
        for (bool Replace : {false, true}) {
            Vm NestedVm(Nested.Program, 100, 20);
            auto Native = std::make_shared<NestedFaultCall>(
                NestedVm, Nested.Functions.at("inner"), Replace);
            NestedVm.RegisterNativeFunction("trigger", Native);
            Nested.Initialize(NestedVm);
            try { (void)NestedVm.Run(Nested.Functions.at("main"));
                  throw std::runtime_error("nested fault did not propagate"); }
            catch (const std::runtime_error& Failure) {
                Check(std::string_view(Failure.what()) ==
                      (Replace ? "native replacement" : "VM instruction budget exceeded"),
                      "unexpected nested fault message");
            }
            auto Fault = NestedVm.GetFaultLocation();
            if (!Fault || Fault->Line != (Replace ? 4u : 2u))
                throw std::runtime_error("nested fault was attributed to line " +
                    (Fault ? std::to_string(Fault->Line) : std::string("none")) +
                    " (inner observed " + std::to_string(Native->ObservedLine) + ")" +
                    (Replace ? " after replacement" : " after propagation"));
        }

        auto Allocation = Compile("def main() { var x = object(); return x; }");
        Vm AllocationVm(Allocation.Program, 1);
        Allocation.Initialize(AllocationVm);
        try { (void)AllocationVm.Run(Allocation.Functions.at("main"));
              throw std::runtime_error("allocation limit did not propagate"); }
        catch (const std::bad_alloc&) {}
        Check(AllocationVm.GetFaultLocation().has_value(),
              "allocation fault did not retain its opcode location");

        auto Shared = Value::FromObject(std::make_shared<ErrorObject>("host failure"));
        auto First = Compile("def main() {\n  return fail();\n}\n");
        auto Second = Compile("\n\ndef main() {\n  return fail();\n}\n");
        Vm FirstVm(First.Program), SecondVm(Second.Program);
        auto Native = std::make_shared<SharedErrorCall>(Shared);
        FirstVm.RegisterNativeFunction("fail", Native);
        SecondVm.RegisterNativeFunction("fail", Native);
        First.Initialize(FirstVm);
        Second.Initialize(SecondVm);
        auto FirstError = FirstVm.Run(First.Functions.at("main"));
        auto SecondError = SecondVm.Run(Second.Functions.at("main"));
        auto FirstOrigin = FirstVm.GetErrorLocation(FirstError);
        auto SecondOrigin = SecondVm.GetErrorLocation(SecondError);
        Check(FirstOrigin && FirstOrigin->Line == 2 && FirstOrigin->Column == 14 &&
              SecondOrigin && SecondOrigin->Line == 4 && SecondOrigin->Column == 14,
              "shared native Error origins leaked across VMs");

        auto& Body = *Source.Program->Constants.at(Source.Functions.at("helper")).Function;
        Body.Locations.front().Pc += 1;
        try { Source.Program->Validate(); throw std::runtime_error("bad debug PC accepted"); }
        catch (const std::invalid_argument&) {}

        auto InvalidLocals = Compile("def bad(arg) { return arg; }");
        auto& BadBody = *InvalidLocals.Program->Constants.at(
            InvalidLocals.Functions.at("bad")).Function;
        BadBody.LocalVariables.front().EndPc = 0;
        try { InvalidLocals.Program->Validate();
              throw std::runtime_error("bad debug local range accepted"); }
        catch (const std::invalid_argument&) {}

        std::cout << "Runtime diagnostics tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Runtime diagnostics tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
