#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

class CheckpointCall final : public NativeObject {
public:
    CheckpointCall(NativeObjectType& Type, Vm& Machine,
                   std::vector<std::uint8_t>& Saved);
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

constexpr NativeModuleGuid SnapshotTestModule{{
    0x42, 0x2e, 0x81, 0x13, 0x64, 0xf1, 0x4c, 0x98,
    0xa7, 0xc5, 0x35, 0xd0, 0x69, 0xa1, 0x77, 0x20}};

class CheckpointType final : public NativeObjectType {
public:
    CheckpointType(Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObjectType(Machine, SnapshotTestModule, "Checkpoint", 1), Saved(Saved) {}
    std::shared_ptr<CheckpointCall> Create() {
        return CreateObject<CheckpointCall>(GetVm(), Saved);
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        if (!dynamic_cast<const CheckpointCall*>(&Input))
            throw std::runtime_error("unexpected multimodule host object");
        return {};
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || !Payload.empty())
            throw std::runtime_error("unknown multimodule host record");
        return Create();
    }
private:
    std::vector<std::uint8_t>& Saved;
};

CheckpointCall::CheckpointCall(NativeObjectType& Type, Vm& Machine,
                               std::vector<std::uint8_t>& Saved)
    : NativeObject(Type), Machine(Machine), Saved(Saved) {}
}

int main() {
    try {
        auto Root = Compile("var math = import(\"math\"); def main() { return math.sum(7); } "
                            "def fault() { return math.bad(); }");
        auto Math = Compile(
            "export var base = 6; var hidden = 99; "
            "export def sum(x) { return base + x; } "
            "export def make() { var x = object(); x.value = base; return x; } "
            "export def read(x) { return x.value; } "
            "export def bad() { return 1 + \"x\"; }");
        auto Other = Compile("export var base = 40; export def sum(x) { return base + x; }");
        auto Consumer = Compile(
            "var math = import(\"math\"); var base = 100; "
            "export def share() { return math.read(math.make()) + base; }");
        Vm Machine(Root.Program);
        Math.LoadInto(Machine, "math");
        Other.LoadInto(Machine, "other");
        Consumer.LoadInto(Machine, "consumer");
        Check(Machine.IsModuleBuiltFrom("math", Math.Program, SerializeProgram(Math)),
              "module content identity lost");
        Math.LoadInto(Machine, "math"); // Reuse the same module instance.
        try { Other.LoadInto(Machine, "math");
              throw std::runtime_error("duplicate ID accepted a different program"); }
        catch (const std::invalid_argument&) {}

        RegisterModuleImport(Machine, {}, [&](std::string_view Caller,
                                               std::string_view Specifier) -> Value {
            Check(Caller.empty() && Specifier == "math", "root import context changed");
            Check(!Math.InitializeModule(Machine, "math").IsError(), "math initializer failed");
            return Machine.GetModuleNamespace("math");
        });
        RegisterModuleImport(Machine, "consumer", [&](std::string_view Caller,
                                                         std::string_view Specifier) -> Value {
            Check(Caller == "consumer" && Specifier == "math", "module import context changed");
            Check(!Math.InitializeModule(Machine, "math").IsError(), "repeated math import failed");
            return Machine.GetModuleNamespace("math");
        });
        Check(!Root.Initialize(Machine).IsError(), "root initializer failed");
        Check(!Consumer.InitializeModule(Machine, "consumer").IsError(),
              "consumer initializer failed");
        Check(!Other.InitializeModule(Machine, "other").IsError(), "other initializer failed");
        Check(Machine.Run(Root.Functions.at("main")).AsNumber() == 13,
              "cross-module function used the caller's globals or constants");
        Check(Machine.RunModule("consumer", Consumer.Functions.at("share")).AsNumber() == 106,
              "same-VM ScriptObject did not cross module function calls");
        auto CrossError = Machine.Run(Root.Functions.at("fault"));
        auto CrossOrigin = Machine.GetErrorLocation(CrossError);
        Check(CrossError.IsError() && CrossOrigin && CrossOrigin->ModuleId == "math",
              "cross-module Error was attributed to the caller");
        Check(Machine.GetModuleGlobal("math", "base").AsNumber() == 6 &&
              Machine.GetModuleGlobal("other", "base").AsNumber() == 40 &&
              Machine.GetGlobal("base").IsError(), "module globals leaked into another module");
        auto Namespace = Machine.GetModuleNamespace("math");
        auto NativeNamespace = std::dynamic_pointer_cast<NativeObject>(Namespace.AsNativeObject());
        Check(NativeNamespace && NativeNamespace->GetMember(Value::String("hidden")).IsError(),
              "unexported global was visible");
        Machine.SetModuleGlobal("math", "base", Value::Number(8));
        auto MathReloaded = DeserializeProgram(SerializeProgram(Math));
        MathReloaded.LoadInto(Machine, "math");
        Check(MathReloaded.InitializeModule(Machine, "math").GetType() == ValueType::Null &&
              Machine.GetModuleGlobal("math", "base").AsNumber() == 8,
              "deserialized equivalent module did not reuse the existing instance");
        Check(NativeNamespace->GetMember(Value::String("base")).AsNumber() == 8 &&
              Machine.Run(Root.Functions.at("main")).AsNumber() == 15,
              "module namespace was not a live view of its globals");
        Check(Math.InitializeModule(Machine, "math").GetType() == ValueType::Null &&
              Machine.GetModuleGlobal("math", "base").AsNumber() == 8,
              "repeated initialization reran top-level side effects");

        auto A = Compile("var b = import(\"B\"); export def x() { return 1; }");
        auto B = Compile("var a = import(\"A\"); export def y() { return 2; }");
        Vm Cyclic(A.Program);
        A.LoadInto(Cyclic, "A"); B.LoadInto(Cyclic, "B");
        RegisterModuleImport(Cyclic, "A", [&](std::string_view, std::string_view) -> Value {
            (void)B.InitializeModule(Cyclic, "B");
            return Cyclic.GetModuleNamespace("B");
        });
        RegisterModuleImport(Cyclic, "B", [&](std::string_view, std::string_view) -> Value {
            (void)A.InitializeModule(Cyclic, "A");
            return Cyclic.GetModuleNamespace("A");
        });
        try { (void)A.InitializeModule(Cyclic, "A");
              throw std::runtime_error("initialization cycle was accepted"); }
        catch (const std::runtime_error& Failure) {
            Check(std::string_view(Failure.what()) == "circular module initialization",
                  "initialization cycle had the wrong fault");
        }

        auto SnapshotRoot = Compile("def main() { return math.entry() + 1; }");
        auto SnapshotMath = Compile(
            "export var shared = object(); shared.value = 41; "
            "export def entry() { if (checkpoint()) { return shared.value; } return 0; }");
        std::vector<std::uint8_t> Saved;
        Vm Before(SnapshotRoot.Program);
        auto& BeforeType = Before.CreateNativeObjectType<CheckpointType>(Saved);
        SnapshotMath.LoadInto(Before, "math");
        Before.SetModuleGlobal("math", "checkpoint",
            Value::FromObject(BeforeType.Create()));
        Check(!SnapshotMath.InitializeModule(Before, "math").IsError(),
              "snapshot module initializer failed");
        Before.SetGlobal("math", Before.GetModuleNamespace("math"));
        SnapshotRoot.Initialize(Before);
        Check(Before.Run(SnapshotRoot.Functions.at("main")).AsNumber() == 1 &&
              Saved.size() > 12 && Saved[4] == 4,
              "cross-module save point failed");

        Vm After(SnapshotRoot.Program);
        After.CreateNativeObjectType<CheckpointType>(Saved);
        SnapshotMath.LoadInto(After, "math");
        Check(After.ResumeSnapshot(Saved).AsNumber() == 42,
              "cross-module frames or globals failed to restore");
        Check(After.GetModuleGlobal("math", "shared").AsScriptObject() !=
              Before.GetModuleGlobal("math", "shared").AsScriptObject() &&
              After.GetGlobal("math").AsObject() == After.GetModuleNamespace("math").AsObject(),
              "restored module object identity changed");
        Check(After.CollectGarbage() == 0,
              "restored module global did not root its ScriptObject");
        auto DamagedSnapshot = Saved;
        DamagedSnapshot[20] ^= 1;
        Vm DamagedTarget(SnapshotRoot.Program);
        SnapshotMath.LoadInto(DamagedTarget, "math");
        try { (void)DamagedTarget.ResumeSnapshot(DamagedSnapshot);
              throw std::runtime_error("damaged multi-module snapshot accepted"); }
        catch (const std::invalid_argument&) {}
        Check(DamagedTarget.GetScriptObjectCount() == 1 &&
              DamagedTarget.GetModuleGlobal("math", "shared").IsError(),
              "damaged multi-module snapshot mutated target VM");

        std::cout << "Multimodule tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Multimodule tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
