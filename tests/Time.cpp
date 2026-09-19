#include "Time.hpp"
#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>

#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

class Host final : public TimeHost {
public:
    double Monotonic() override { return Now; }
    double UnixTime() override { return Wall; }
    void Sleep(double Seconds) override { Now += Seconds; }
    void SleepUntil(double Deadline) override { Now = std::max(Now, Deadline); }
    double Now = 10;
    double Wall = 1000;
};

constexpr NativeModuleGuid CheckpointGuid{{
    0x38, 0x85, 0x72, 0x1d, 0xce, 0xe5, 0x4b, 0x74,
    0x9e, 0x0f, 0xa1, 0x8a, 0xc9, 0x67, 0x44, 0x22}};

class Checkpoint final : public NativeObject {
public:
    Checkpoint(NativeObjectType& Type, Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObject(Type), Machine(Machine), Saved(Saved) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        try { Saved = Machine.CaptureSnapshot(); }
        catch (const std::exception& Failure) {
            return Value::FromObject(std::make_shared<ErrorObject>(Failure.what()));
        }
        return Value::Bool(false);
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
};

class CheckpointType final : public NativeObjectType {
public:
    CheckpointType(Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObjectType(Machine, CheckpointGuid, "Checkpoint"), Saved(Saved) {}
    std::shared_ptr<Checkpoint> Create() {
        return CreateObject<Checkpoint>(GetVm(), Saved);
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || !Payload.empty())
            throw std::invalid_argument("invalid test checkpoint");
        return Create();
    }
private:
    std::vector<std::uint8_t>& Saved;
};

Value Member(const Value& Object, const char* Name) {
    return dynamic_cast<NativeObject*>(Object.AsObject())->GetMember(Value::String(Name));
}

Value Call(const Value& Module, const char* Name, std::vector<Value> Arguments = {}) {
    auto Function = Member(Module, Name);
    Check(!Function.IsError(), "time function missing");
    return dynamic_cast<NativeObject*>(Function.AsObject())->Call(Arguments);
}
}

int main() {
    try {
        auto Program = Compile("def main() { return null; }");
        Vm Machine(Program.Program);
        auto Fake = std::make_shared<Host>();
        TimeLibrary Library(Machine, Fake);
        auto Time = Library.GetModule();

        Check(Call(Time, "Monotonic").AsNumber() == 10, "monotonic time mismatch");
        Check(Call(Time, "UnixTime").AsNumber() == 1000, "Unix time mismatch");
        auto Clock = Call(Time, "CreateClock");
        Check(!Call(Time, "Sleep", {Value::Number(0.25)}).IsError(), "sleep failed");
        Check(std::abs(Call(Time, "Tick", {Clock}).AsNumber() - 0.25) < 1e-9,
              "clock tick mismatch");
        Check(!Call(Time, "SleepUntil", {Value::Number(12)}).IsError() && Fake->Now == 12,
              "sleep until mismatch");
        Check(!Call(Time, "Reset", {Clock}).IsError(), "clock reset failed");
        Fake->Now += 0.5;
        Check(std::abs(Call(Time, "Tick", {Clock}).AsNumber() - 0.5) < 1e-9,
              "clock reset baseline mismatch");
        Check(Call(Time, "Sleep", {Value::Number(-1)}).IsError(),
              "negative sleep was accepted");
        Check(Call(Time, "Tick", {Value{}}).IsError(), "invalid Clock was accepted");

        auto SnapshotProgram = Compile(
            "var time = import(\"time\"); var clock = null; var before = 0; "
            "def main() { clock = time.CreateClock(); time.Sleep(2); "
            "before = time.Tick(clock); if (checkpoint()) { "
            "time.Sleep(4); return before + time.Tick(clock); } return before; }");
        std::vector<std::uint8_t> Saved;
        {
            Vm Original(SnapshotProgram.Program);
            auto OriginalHost = std::make_shared<Host>();
            TimeLibrary OriginalTime(Original, OriginalHost);
            RegisterImport(Original, [&](std::string_view Name) {
                return Name == "time" ? OriginalTime.GetModule() : Value{};
            });
            auto& Checkpoints = Original.CreateNativeObjectType<CheckpointType>(Saved);
            Original.RegisterNativeFunction("checkpoint", Checkpoints.Create());
            Check(!SnapshotProgram.Initialize(Original).IsError(), "time snapshot initializer failed");
            Check(Original.Run(SnapshotProgram.Functions.at("main")).AsNumber() == 2 && !Saved.empty(),
                  "time snapshot save path failed");
        }
        {
            Vm Restored(SnapshotProgram.Program);
            auto RestoredHost = std::make_shared<Host>();
            RestoredHost->Now = 5000;
            TimeLibrary RestoredTime(Restored, RestoredHost);
            RegisterImport(Restored, [&](std::string_view Name) {
                return Name == "time" ? RestoredTime.GetModule() : Value{};
            });
            Restored.CreateNativeObjectType<CheckpointType>(Saved);
            Check(Restored.ResumeSnapshot(Saved).AsNumber() == 6,
                  "restored Clock counted offline time or lost its new baseline");
        }

        std::cout << "Time tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Time tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
