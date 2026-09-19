#include <Feather/Compiler.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <span>
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
              (std::string("snapshot rejected for the wrong reason: expected ") + Expected +
               ", got " + Failure.what()).c_str());
        return;
    }
    throw std::runtime_error("invalid snapshot accepted");
}

class CheckpointNative final : public NativeObject {
public:
    CheckpointNative(NativeObjectType& Type, Vm& Machine,
                     std::vector<std::uint8_t>& Saved);
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        try { Saved = Machine.CaptureSnapshot(); }
        catch (const std::logic_error&) {
            return Value::FromObject(std::make_shared<ErrorObject>("snapshot depth rejected"));
        }
        return Value::Bool(false);
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
};

constexpr NativeModuleGuid CheckpointModule{{
    0x8d, 0x9a, 0x62, 0xc4, 0x2d, 0x1f, 0x44, 0xd5,
    0x91, 0xa4, 0x18, 0x77, 0x31, 0x5e, 0xb2, 0x09}};
std::size_t DestroyedCheckpointObjects = 0;

class CheckpointType final : public NativeObjectType {
public:
    CheckpointType(Vm& Machine, std::vector<std::uint8_t>& Saved,
                   NativeModuleGuid Module = CheckpointModule,
                   std::string Name = "Checkpoint")
        : NativeObjectType(Machine, Module, std::move(Name), 1), Saved(Saved) {}
    std::shared_ptr<CheckpointNative> Create() {
        return CreateObject<CheckpointNative>(GetVm(), Saved);
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        if (FailEncode) throw std::runtime_error("host encode failed");
        if (!dynamic_cast<const CheckpointNative*>(&Input))
            throw std::runtime_error("unexpected host object");
        return {};
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (FailDecode) throw std::runtime_error("host decode failed");
        if (Version != 1 || !Payload.empty())
            throw std::runtime_error("unknown host object");
        return Create();
    }
    void FinalizeRestore(NativeObject& Input) override {
        if (!dynamic_cast<CheckpointNative*>(&Input))
            throw std::runtime_error("unexpected finalized host object");
        Finalized = true;
    }
    bool FailEncode = false;
    bool FailDecode = false;
    bool Finalized = false;
protected:
    void Destroy(NativeObject* Input) noexcept override {
        ++DestroyedCheckpointObjects;
        NativeObjectType::Destroy(Input);
    }
private:
    std::vector<std::uint8_t>& Saved;
};

CheckpointNative::CheckpointNative(NativeObjectType& Type, Vm& Machine,
                                   std::vector<std::uint8_t>& Saved)
    : NativeObject(Type), Machine(Machine), Saved(Saved) {}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& Bytes, std::size_t At) {
    return std::uint32_t(Bytes[At]) | (std::uint32_t(Bytes[At + 1]) << 8) |
           (std::uint32_t(Bytes[At + 2]) << 16) | (std::uint32_t(Bytes[At + 3]) << 24);
}
std::uint32_t TestChecksum(std::span<const std::uint8_t> Bytes) {
    std::uint32_t Crc = 0xffffffffu;
    for (auto Byte : Bytes) {
        Crc ^= Byte;
        for (int Bit = 0; Bit < 8; ++Bit)
            Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320u : 0u);
    }
    return ~Crc;
}
void SealSnapshot(std::vector<std::uint8_t>& Bytes) {
    auto Crc = TestChecksum(std::span<const std::uint8_t>(Bytes.data(), Bytes.size() - 4));
    for (unsigned I = 0; I < 4; ++I)
        Bytes[Bytes.size() - 4 + I] = static_cast<std::uint8_t>(Crc >> (8 * I));
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
        std::shared_ptr<CheckpointNative> TypeLifetimeObject;
        auto DestroyedBefore = DestroyedCheckpointObjects;
        {
            Vm TypeLifetimeVm(Compiled.Program);
            auto& Type = TypeLifetimeVm.CreateNativeObjectType<CheckpointType>(Saved);
            TypeLifetimeObject = Type.Create();
        }
        Check(DestroyedCheckpointObjects == DestroyedBefore,
              "VM destroyed a native type while one of its objects survived");
        TypeLifetimeObject.reset();
        Check(DestroyedCheckpointObjects == DestroyedBefore + 1,
              "native object destruction did not return to its type");

        Vm TypeRules(Compiled.Program);
        TypeRules.CreateNativeObjectType<CheckpointType>(Saved);
        RejectWith([&] { TypeRules.CreateNativeObjectType<CheckpointType>(Saved); },
                   "duplicate native object type");
        auto OtherModule = CheckpointModule;
        OtherModule.Bytes[0] ^= 0xff;
        TypeRules.CreateNativeObjectType<CheckpointType>(Saved, OtherModule, "Checkpoint");
        RejectWith([&] {
            TypeRules.CreateNativeObjectType<CheckpointType>(
                Saved, NativeModuleGuid{}, "ZeroGuid");
        }, "native module GUID is zero");

        Vm Original(Compiled.Program);
        auto& OriginalType = Original.CreateNativeObjectType<CheckpointType>(Saved);
        auto Checkpoint = OriginalType.Create();
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
        Check(Saved[4] == 4 &&
              ReadU32(Saved, Saved.size() - 4) ==
                  TestChecksum(std::span<const std::uint8_t>(Saved.data(), Saved.size() - 4)),
              "snapshot v4 checksum was not written correctly");

        auto BeforeFailedEncode = Saved;
        OriginalType.FailEncode = true;
        RejectWith([&] { (void)Original.Run(Compiled.Functions.at("outer")); },
                   "host encode failed");
        OriginalType.FailEncode = false;
        Check(Saved == BeforeFailedEncode &&
              Original.GetGlobal("shared").AsScriptObject() == OriginalShared,
              "host encode failure changed saved bytes or VM globals");
        Check(Original.Run(Compiled.Functions.at("outer")).AsNumber() == 0 && !Saved.empty(),
              "snapshot operation stayed busy after host encode failure");

        Vm Restored(Compiled.Program);
        auto& RestoredType = Restored.CreateNativeObjectType<CheckpointType>(Saved);
        Check(Restored.ResumeSnapshot(Saved).AsNumber() == 10,
              "restored nested frames or pending call result");
        Check(RestoredType.Finalized, "native object restore was not finalized");
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

        Vm NoType(Compiled.Program);
        Reject([&] { NoType.ResumeSnapshot(Saved); }, "missing native type accepted");
        Check(NoType.GetScriptObjectCount() == 1, "parse failure mutated heap");
        Vm TooSmall(Compiled.Program);
        RejectWith([&] { TooSmall.ResumeSnapshot(Saved, 10); }, "size limit");
        Vm TooFewObjects(Compiled.Program, 2);
        TooFewObjects.CreateNativeObjectType<CheckpointType>(Saved);
        RejectWith([&] { TooFewObjects.ResumeSnapshot(Saved); },
                   "ScriptObject limit exceeded");
        Check(TooFewObjects.GetScriptObjectCount() == 1,
              "object limit rejection allocated ScriptObjects");

        Vm Failed(Compiled.Program);
        auto& FailedType = Failed.CreateNativeObjectType<CheckpointType>(Saved);
        FailedType.FailDecode = true;
        Reject([&] { Failed.ResumeSnapshot(Saved); }, "host decode failure accepted");
        FailedType.FailDecode = false;
        Check(Failed.GetScriptObjectCount() == 1 && Failed.GetGlobal("shared").IsError(),
              "host decode failure did not roll back VM");
        Check(Failed.ResumeSnapshot(Saved).AsNumber() == 10,
              "restore retry after rollback");

        auto Different = Compile("def outer() { return 12; }");
        Vm Wrong(Different.Program);
        Reject([&] { Wrong.ResumeSnapshot(Saved); }, "different module accepted");
        auto Corrupt = Saved;
        Corrupt[0] = 0;
        Vm BadMagic(Compiled.Program);
        Reject([&] { BadMagic.ResumeSnapshot(Corrupt); }, "bad magic accepted");
        Corrupt = Saved;
        Corrupt[4] = 1;
        Vm BadVersion(Compiled.Program);
        Reject([&] { BadVersion.ResumeSnapshot(Corrupt); }, "bad version accepted");
        Corrupt = Saved;
        Corrupt[20] ^= 1;
        Vm BadChecksum(Compiled.Program);
        RejectWith([&] { BadChecksum.ResumeSnapshot(Corrupt); },
                   "checksum mismatch");
        Check(BadChecksum.GetScriptObjectCount() == 1,
              "checksum rejection changed VM state");
        Corrupt = Saved;
        Corrupt.pop_back();
        Vm Truncated(Compiled.Program);
        Reject([&] { Truncated.ResumeSnapshot(Corrupt); }, "truncated data accepted");
        Corrupt = Saved;
        std::size_t ObjectStart = 8;
        ObjectStart += 4 + ReadU32(Corrupt, ObjectStart); // Skip the module image.
        ObjectStart += 4;
        Corrupt[ObjectStart] = 0xff;
        Corrupt[ObjectStart + 1] = 0xff;
        Corrupt[ObjectStart + 2] = 0;
        Corrupt[ObjectStart + 3] = 0;
        SealSnapshot(Corrupt);
        Vm ImpossibleCount(Compiled.Program);
        RejectWith([&] { ImpossibleCount.ResumeSnapshot(Corrupt); },
                   "object count exceeds section size");
        Check(ImpossibleCount.GetScriptObjectCount() == 1,
              "impossible object count mutated VM");
        Corrupt = Saved;
        Corrupt = Saved;
        std::size_t At = 8;
        for (int I = 0; I < 3; ++I) At += 4 + ReadU32(Corrupt, At);
        At += 4 + 4 + 1; // frame section length, count, pending-result marker
        for (int I = 0; I < 4; ++I) Corrupt[At + I] = 0xff;
        SealSnapshot(Corrupt);
        Vm BadFrame(Compiled.Program);
        BadFrame.CreateNativeObjectType<CheckpointType>(Saved);
        Reject([&] { BadFrame.ResumeSnapshot(Corrupt); }, "invalid frame index accepted");
        Corrupt = Saved;
        At = 8;
        for (int I = 0; I < 3; ++I) At += 4 + ReadU32(Corrupt, At);
        At += 4 + 4 + 1 + 4 + 4 + 4 + 4; // first frame stack count; outer has no locals
        Corrupt[At] = 1;
        SealSnapshot(Corrupt);
        Vm BadStack(Compiled.Program);
        BadStack.CreateNativeObjectType<CheckpointType>(Saved);
        RejectWith([&] { BadStack.ResumeSnapshot(Corrupt); }, "stack count");

        auto Nested = Compile("def save() { return checkpoint(); } def call() { return bridge(); }");
        Vm NestedVm(Nested.Program);
        auto& NestedType = NestedVm.CreateNativeObjectType<CheckpointType>(Saved);
        NestedVm.RegisterNativeFunction("checkpoint",
            NestedType.Create());
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
