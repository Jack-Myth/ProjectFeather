#include <Feather/Compiler.hpp>

#include <cmath>
#include <iostream>
#include <limits>
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
template<class Action> void RejectWith(Action Run, const char* Expected) {
    try { Run(); }
    catch (const std::exception& Failure) {
        Check(std::string(Failure.what()).find(Expected) != std::string::npos,
              "snapshot rejected for the wrong reason");
        return;
    }
    throw std::runtime_error("invalid snapshot accepted");
}

class CheckpointNative final : public NativeObject {
public:
    CheckpointNative(Vm& Machine, std::vector<std::uint8_t>& Saved,
                     SnapshotHostCodec& Codec)
        : Machine(Machine), Saved(Saved), Codec(Codec) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        try { Saved = Machine.CaptureSnapshot(&Codec); }
        catch (const std::logic_error&) {
            return Value::FromObject(std::make_shared<ErrorObject>("snapshot depth rejected"));
        }
        return Value::Bool(false);
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
    SnapshotHostCodec& Codec;
};

class Codec final : public SnapshotHostCodec {
public:
    explicit Codec(std::vector<std::uint8_t>& Saved) : Saved(Saved) {}
    HostSnapshotRecord Encode(const std::shared_ptr<NativeObject>& Input) override {
        if (FailEncode) throw std::runtime_error("host encode failed");
        if (!std::dynamic_pointer_cast<CheckpointNative>(Input))
            throw std::runtime_error("unexpected host object");
        return {"checkpoint", {}};
    }
    std::shared_ptr<NativeObject> Decode(Vm& Machine, const HostSnapshotRecord& Input) override {
        if (FailDecode) throw std::runtime_error("host decode failed");
        if (Input.TypeId != "checkpoint" || !Input.Payload.empty())
            throw std::runtime_error("unknown host object");
        return std::make_shared<CheckpointNative>(Machine, Saved, *this);
    }
    bool FailEncode = false;
    bool FailDecode = false;
private:
    std::vector<std::uint8_t>& Saved;
};

std::uint32_t ReadU32(const std::vector<std::uint8_t>& Bytes, std::size_t At) {
    return std::uint32_t(Bytes[At]) | (std::uint32_t(Bytes[At + 1]) << 8) |
           (std::uint32_t(Bytes[At + 2]) << 16) | (std::uint32_t(Bytes[At + 3]) << 24);
}
class BridgeNative final : public NativeObject {
public:
    BridgeNative(Vm& Machine, std::uint32_t Target) : Machine(Machine), Target(Target) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override { return Machine.Run(Target); }
private:
    Vm& Machine;
    std::uint32_t Target;
};
}

int main() {
    try {
        auto Compiled = Compile(R"(
            var shared = object();
            shared.self = shared;
            def inner(x) {
                var alias = shared;
                if (checkpoint()) {
                    if (alias.self == shared) { return x + 1; }
                    return 99;
                }
                return 0;
            }
            def outer() { return inner(4) * 2; }
        )");
        std::vector<std::uint8_t> Saved;
        Codec HostCodec(Saved);
        Vm Original(Compiled.Program);
        auto Checkpoint = std::make_shared<CheckpointNative>(Original, Saved, HostCodec);
        Original.RegisterNativeFunction("checkpoint", Checkpoint);
        Original.GetRootMetaObject()->SetRaw(Value::String("marker"), Value::Number(9));
        Compiled.Initialize(Original);
        auto OriginalShared = Original.GetGlobal("shared").AsScriptObject();
        auto CustomMeta = Original.CreateMetaObject();
        Original.SetMetaObject(OriginalShared, CustomMeta);
        CustomMeta->SetRaw(Value::String("back"), Value::FromScript(OriginalShared));
        Original.SetGlobal("alias", Original.GetGlobal("checkpoint"));
        Original.SetGlobal("problem", Value::FromObject(std::make_shared<ErrorObject>("oops")));
        Original.SetGlobal("notANumber", Value::Number(std::numeric_limits<double>::quiet_NaN()));
        auto Hidden = Original.CreateScriptObject();
        Checkpoint->SetGcVisibleMember("hidden", Value::FromScript(Hidden));
        Check(Original.Run(Compiled.Functions.at("outer")).AsNumber() == 0,
              "save path should return false");
        Check(!Saved.empty(), "snapshot bytes missing");

        auto BeforeFailedEncode = Saved;
        HostCodec.FailEncode = true;
        RejectWith([&] { (void)Original.Run(Compiled.Functions.at("outer")); },
                   "host encode failed");
        HostCodec.FailEncode = false;
        Check(Saved == BeforeFailedEncode &&
              Original.GetGlobal("shared").AsScriptObject() == OriginalShared,
              "host encode failure changed saved bytes or VM globals");
        Check(Original.Run(Compiled.Functions.at("outer")).AsNumber() == 0 && !Saved.empty(),
              "snapshot operation stayed busy after host encode failure");

        Vm Restored(Compiled.Program);
        Check(Restored.ResumeSnapshot(Saved, &HostCodec).AsNumber() == 10,
              "restored nested frames or pending call result");
        auto Shared = Restored.GetGlobal("shared").AsScriptObject();
        Check(Shared->GetRaw(Value::String("self"))->AsScriptObject() == Shared,
              "shared cyclic reference lost");
        Check(Shared->GetMetaObject() != Restored.GetRootMetaObject() &&
              Shared->GetMetaObject()->GetRaw(Value::String("back"))->AsScriptObject() == Shared &&
              Shared->GetMetaObject()->GetMetaObject() == Restored.GetRootMetaObject(),
              "custom MetaObject cycle or RootMetaObject identity lost");
        Check(Restored.GetRootMetaObject()->GetRaw(Value::String("marker"))->AsNumber() == 9,
              "RootMetaObject member lost");
        Check(Restored.GetGlobal("alias").AsObject() == Restored.GetGlobal("checkpoint").AsObject(),
              "native object alias lost");
        Check(Restored.GetGlobal("problem").IsError() &&
              dynamic_cast<ErrorObject*>(Restored.GetGlobal("problem").AsObject())->GetMessage() == "oops",
              "ErrorObject message lost");
        Check(std::isnan(Restored.GetGlobal("notANumber").AsNumber()), "NaN value lost");
        Check(Shared != Original.GetGlobal("shared").AsScriptObject(),
              "restore reused original VM object");
        Check(Restored.GetScriptObjectCount() == 4 && Restored.CollectGarbage() == 0,
              "native visible member did not retain hidden script object");
        Check(Restored.Run(Compiled.Functions.at("outer")).AsNumber() == 0 && !Saved.empty(),
              "restored host callback did not bind to the new VM");

        Vm NoCodec(Compiled.Program);
        Reject([&] { NoCodec.ResumeSnapshot(Saved); }, "missing host codec accepted");
        Check(NoCodec.GetScriptObjectCount() == 1, "parse failure mutated heap");
        Vm TooSmall(Compiled.Program);
        RejectWith([&] { TooSmall.ResumeSnapshot(Saved, &HostCodec, 10); }, "size limit");

        Vm Failed(Compiled.Program);
        HostCodec.FailDecode = true;
        Reject([&] { Failed.ResumeSnapshot(Saved, &HostCodec); }, "host decode failure accepted");
        HostCodec.FailDecode = false;
        Check(Failed.GetScriptObjectCount() == 1 && Failed.GetGlobal("shared").IsError(),
              "host decode failure did not roll back VM");
        Check(Failed.ResumeSnapshot(Saved, &HostCodec).AsNumber() == 10,
              "restore retry after rollback");

        auto Different = Compile("def outer() { return 12; }");
        Vm Wrong(Different.Program);
        Reject([&] { Wrong.ResumeSnapshot(Saved, &HostCodec); }, "different module accepted");
        auto Corrupt = Saved;
        Corrupt[0] = 0;
        Vm BadMagic(Compiled.Program);
        Reject([&] { BadMagic.ResumeSnapshot(Corrupt, &HostCodec); }, "bad magic accepted");
        Corrupt = Saved;
        Corrupt[4] = 2;
        Vm BadVersion(Compiled.Program);
        Reject([&] { BadVersion.ResumeSnapshot(Corrupt, &HostCodec); }, "bad version accepted");
        Corrupt = Saved;
        Corrupt.pop_back();
        Vm Truncated(Compiled.Program);
        Reject([&] { Truncated.ResumeSnapshot(Corrupt, &HostCodec); }, "truncated data accepted");
        Corrupt = Saved;
        std::size_t At = 8;
        for (int I = 0; I < 3; ++I) At += 4 + ReadU32(Corrupt, At);
        At += 4 + 4 + 1; // frame section length, count, pending-result marker
        for (int I = 0; I < 4; ++I) Corrupt[At + I] = 0xff;
        Vm BadFrame(Compiled.Program);
        Reject([&] { BadFrame.ResumeSnapshot(Corrupt, &HostCodec); }, "invalid frame index accepted");
        Corrupt = Saved;
        At = 8;
        for (int I = 0; I < 3; ++I) At += 4 + ReadU32(Corrupt, At);
        At += 4 + 4 + 1 + 4 + 4 + 4; // first frame stack count; outer has no locals
        Corrupt[At] = 1;
        Vm BadStack(Compiled.Program);
        RejectWith([&] { BadStack.ResumeSnapshot(Corrupt, &HostCodec); }, "stack count");

        auto Nested = Compile("def save() { return checkpoint(); } def call() { return bridge(); }");
        Vm NestedVm(Nested.Program);
        NestedVm.RegisterNativeFunction("checkpoint",
            std::make_shared<CheckpointNative>(NestedVm, Saved, HostCodec));
        NestedVm.RegisterNativeFunction("bridge",
            std::make_shared<BridgeNative>(NestedVm, Nested.Functions.at("save")));
        Nested.Initialize(NestedVm);
        Check(NestedVm.Run(Nested.Functions.at("call")).IsError(),
              "nested native capture should be rejected");

        std::cout << "M5 snapshot tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "M5 snapshot tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
