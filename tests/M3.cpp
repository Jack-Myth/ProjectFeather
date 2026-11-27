#include <Feather/Compiler.hpp>
#include <Feather/ByteBuffer.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace Feather;

namespace {
void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}
template<class Action> void Reject(Action Run, const char* Message) {
    try { Run(); } catch (const std::exception&) { return; }
    throw std::runtime_error(Message);
}
std::shared_ptr<Module> Program() {
    auto ModuleValue = std::make_shared<Module>();
    auto Body = std::make_shared<FunctionPrototype>();
    Builder Code; Code.Emit(Op::Null); Code.Emit(Op::Return);
    Body->Code = std::move(Code).Finish();
    ModuleValue->AddFunction(Body);
    return ModuleValue;
}
class HostNative final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
};
class GcDuringCall final : public NativeObject {
public:
    explicit GcDuringCall(Vm& Owner) : Owner(Owner) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        if (Owner.GetNativeCallDepth() != 1) return Value::Bool(false);
        try { Owner.CollectGarbage(); }
        catch (const std::logic_error&) { return Value::Bool(true); }
        return Value::Bool(false);
    }
private:
    Vm& Owner;
};
class AllocateDuringCall final : public NativeObject {
public:
    explicit AllocateDuringCall(Vm& Owner) : Owner(Owner) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        Value Last;
        for (int I = 0; I < 7; ++I)
            Last = Value::FromScript(Owner.CreateScriptObject());
        return Last;
    }
private:
    Vm& Owner;
};
void CyclesAndRoots() {
    Vm Machine(Program());
    auto A = Machine.CreateScriptObject();
    auto B = Machine.CreateScriptObject();
    A->SetRaw(Value::String("peer"), Value::FromScript(B));
    B->SetRaw(Value::String("peer"), Value::FromScript(A));
    Check(Machine.GetScriptObjectCount() == 3, "root and cycle allocations");
    auto BeforeGc = Machine.GetGcStatistics();
    Check(BeforeGc.ScriptObjectCount == 3 && BeforeGc.ScriptMemberCount == 2 &&
          BeforeGc.EstimatedScriptBytes >= 3 * sizeof(void*), "GC statistics before collection");
    Check(Machine.CollectGarbage() == 2, "unrooted cycle should collect");
    Check(Machine.GetScriptObjectCount() == 1, "only RootMetaObject remains");
    Check(Machine.GetGcStatistics().ScriptMemberCount == 0, "GC statistics after collection");

    auto Array = Machine.CreateArray();
    auto EmptyBytes = Machine.GetGcStatistics().EstimatedScriptBytes;
    for (int I = 0; I < 256; ++I) Array->AppendArray(Value::Number(I));
    Check(Machine.GetGcStatistics().EstimatedScriptBytes >= EmptyBytes + 256 * sizeof(Value),
          "array storage missing from GC statistics");
    Machine.CollectGarbage();

    A = Machine.CreateScriptObject();
    A->SetRaw(Value::String("self"), Value::FromScript(A));
    auto Handle = Machine.AddToRoot(Value::FromScript(A));
    Check(Machine.CollectGarbage() == 0, "RootHandle should retain object");
    Check(Handle.Get().AsScriptObject() == A, "RootHandle Get");
    Handle.Reset();
    Reject([&] { Handle.Reset(); }, "double RootHandle release accepted");
    Check(Machine.CollectGarbage() == 1, "released root should collect");

    A = Machine.CreateScriptObject();
    Machine.GetRootMetaObject()->SetRaw(Value::String("kept"), Value::FromScript(A));
    Check(Machine.CollectGarbage() == 0, "RootMetaObject member should retain");
    Machine.GetRootMetaObject()->SetRaw(Value::String("kept"), Value{});
    Check(Machine.CollectGarbage() == 1, "replaced RootMetaObject member should collect");
}
void NativeVisibilityAndBoundaries() {
    Vm Machine(Program());
    auto Native = std::make_shared<HostNative>();
    for (int I = 0; I < 64; ++I) Machine.RegisterNativeObject(std::make_shared<HostNative>());
    Machine.RegisterNativeObject(Native);
    for (int I = 0; I < 64; ++I) Machine.RegisterNativeObject(std::make_shared<HostNative>());
    auto Script = Machine.CreateScriptObject();
    Native->SetGcVisibleMember("held", Value::FromScript(Script));
    Check(Machine.CollectGarbage() == 0, "registered NativeObject member should retain");
    Native->RemoveGcVisibleMember("held");
    Check(Machine.CollectGarbage() == 1, "cleared native member should release");

    auto Foreign = std::make_unique<Vm>(Program());
    auto ForeignScript = Foreign->CreateScriptObject();
    Reject([&] { Machine.ValidateOwnedValue(Value::FromScript(ForeignScript)); }, "host validation accepted a foreign object");
    Reject([&] { Machine.SetMetaObject(Machine.CreateScriptObject(), ForeignScript); },
           "foreign MetaObject accepted");
    Native->SetGcVisibleMember("foreign", Value::FromScript(ForeignScript));
    Reject([&] { Machine.CollectGarbage(); }, "foreign native GC member accepted");
    Native->RemoveGcVisibleMember("foreign");
    Check(Machine.CollectGarbage() == 1, "GC should recover after rejected foreign member");

    RootHandle Old;
    {
        auto Temporary = std::make_unique<Vm>(Program());
        Old = Temporary->AddToRoot(Value::FromScript(Temporary->CreateScriptObject()));
    }
    Reject([&] { Old.Get(); }, "RootHandle usable after VM destruction");
    Old.Reset();

    auto CallbackProgram = std::make_shared<Module>();
    auto Body = std::make_shared<FunctionPrototype>();
    Builder Code; Code.EmitU32(Op::GetLocal, 0); Code.EmitU16(Op::Call, 0); Code.Emit(Op::Return);
    Body->Code = std::move(Code).Finish(); Body->ParameterCount = 1; Body->LocalCount = 1;
    Body->Defaults.resize(1);
    auto Id = CallbackProgram->AddFunction(Body);
    Vm CallbackVm(CallbackProgram);
    Check(CallbackVm.Run(Id, {Value::FromObject(std::make_shared<GcDuringCall>(CallbackVm))}).AsBool(),
          "GC during active VM call should reject");
    Check(CallbackVm.GetNativeCallDepth() == 0, "native call depth must unwind");
}
void NativeRegistryChurn() {
    auto Source = Compile(R"(
        def read(buffer, count) {
            var i = 0;
            while (i < count) { buffer.Get(0); i = i + 1; }
            return buffer.Get(0);
        }
    )");
    Vm Machine(Source.Program);
    Source.Initialize(Machine);
    auto Native = std::make_shared<HostNative>();
    Machine.RegisterNativeObject(Native);
    Native->SetGcVisibleMember("held", Value::FromScript(Machine.CreateScriptObject()));
    auto Buffer = ByteBufferLibrary(Machine).Create("x");
    const auto Before = Machine.GetGcStatistics();
    auto Result = Machine.Run(Source.Functions.at("read"), {Buffer, Value::Number(100'000)});
    Check(Result.AsByte() == 'x', "buffer read failed during registry churn");
    const auto After = Machine.GetGcStatistics();
    Check(After.CollectionCount == Before.CollectionCount, "registry churn triggered script GC");
    Check(After.NativeRegistryEntries <= Before.NativeRegistryEntries + 1024,
          "short-lived native method registrations accumulated");
    Check(Machine.CollectGarbage() == 0, "registry pruning lost a live native root");
    Native.reset();
    Check(Machine.CollectGarbage() == 1, "expired native registration retained a script object");
}
void GlobalRoots() {
    auto ModuleValue = std::make_shared<Module>();
    auto Name = ModuleValue->AddString("saved");
    auto Store = std::make_shared<FunctionPrototype>();
    Builder StoreCode;
    StoreCode.EmitU32(Op::GetLocal, 0);
    StoreCode.EmitU32(Op::SetGlobal, Name); StoreCode.Emit(Op::Pop);
    StoreCode.Emit(Op::Null); StoreCode.Emit(Op::Return);
    Store->Code = std::move(StoreCode).Finish();
    Store->ParameterCount = 1; Store->LocalCount = 1; Store->Defaults.resize(1);
    auto StoreId = ModuleValue->AddFunction(Store);
    auto Clear = std::make_shared<FunctionPrototype>();
    Builder ClearCode;
    ClearCode.Emit(Op::Null); ClearCode.EmitU32(Op::SetGlobal, Name);
    ClearCode.Emit(Op::Pop); ClearCode.Emit(Op::Null); ClearCode.Emit(Op::Return);
    Clear->Code = std::move(ClearCode).Finish();
    auto ClearId = ModuleValue->AddFunction(Clear);
    Vm Machine(ModuleValue);
    auto Script = Machine.CreateScriptObject();
    Machine.Run(StoreId, {Value::FromScript(Script)});
    Check(Machine.CollectGarbage() == 0, "global value should retain ScriptObject");
    Machine.Run(ClearId);
    Check(Machine.CollectGarbage() == 1, "cleared global should release ScriptObject");
}
void AutomaticCollection() {
    auto Source = Compile(R"(
        def churn(n) {
            var i = 0;
            while (i < n) { object(); i = i + 1; }
        }
        def keepLocal(n) {
            var kept = object();
            kept.answer = 42;
            churn(n);
            return kept.answer;
        }
        def first(a, b) { return a; }
        def keepOnStack(n) { return first(object(), churn(n)); }
    )");
    Vm Machine(Source.Program, 8);
    Source.Initialize(Machine);
    Check(Machine.Run(Source.Functions.at("keepLocal"), {Value::Number(100)}).AsNumber() == 42,
          "automatic GC lost a local root");
    auto Stacked = Machine.Run(Source.Functions.at("keepOnStack"), {Value::Number(100)});
    Check(Stacked.IsScriptObject(), "automatic GC lost an expression stack root");
    auto Root = Machine.AddToRoot(Stacked);
    (void)Machine.CollectGarbage();
    Check(Root.Get().AsScriptObject() == Stacked.AsScriptObject(),
          "automatic GC result could not be rooted");
    auto Statistics = Machine.GetGcStatistics();
    Check(Statistics.CollectionCount > 1 && Statistics.TotalCollectedScriptObjects >= 200 &&
          Statistics.TotalAllocatedScriptObjects > Statistics.ScriptObjectCount &&
          Statistics.NextCollectionObjectCount <= 8,
          "automatic GC statistics are incomplete");
}
void CollectionPendingAtThreshold() {
    auto Source = Compile("def idle() { return 7; }");
    auto Idle = Source.Functions.at("idle");
    Vm Machine(Source.Program, 3);
    Machine.Run(Idle);
    Check(Machine.GetGcStatistics().CollectionCount == 0, "root creation requested a premature GC");

    Machine.CreateScriptObject();
    Machine.CreateScriptObject();
    Check(Machine.GetGcStatistics().CollectionCount == 0, "host allocation collected while idle");
    Machine.Run(Idle);
    Check(Machine.GetGcStatistics().ScriptObjectCount == 1, "idle allocation missed the next safe point");

    auto Kept = Machine.AddToRoot(Value::FromScript(Machine.CreateScriptObject()));
    {
        auto Temporary = Machine.AddToRoot(Value::FromScript(Machine.CreateScriptObject()));
        Check(Machine.CollectGarbage() == 0, "rooted objects must survive collection at the limit");
    }
    Machine.Run(Idle);
    Check(Machine.GetGcStatistics().ScriptObjectCount == 2,
          "full live heap lost its pending collection when the threshold was recomputed");
}
void NativeCollectionDeferral() {
    auto ModuleValue = std::make_shared<Module>();
    auto Body = std::make_shared<FunctionPrototype>();
    Builder Code;
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU16(Op::Call, 0); Code.Emit(Op::Return);
    Body->Code = std::move(Code).Finish(); Body->ParameterCount = 1; Body->LocalCount = 1;
    Body->Defaults.resize(1);
    auto Id = ModuleValue->AddFunction(Body);
    Vm Machine(ModuleValue, 8);
    auto Result = Machine.Run(Id, {Value::FromObject(std::make_shared<AllocateDuringCall>(Machine))});
    Check(Result.IsScriptObject(), "deferred GC lost the native result");
    auto Statistics = Machine.GetGcStatistics();
    Check(Statistics.CollectionCount == 1 && Statistics.TotalCollectedScriptObjects == 6 &&
          Statistics.ScriptObjectCount == 2,
          "native allocations were not collected at the next safe point");
}
void FatalAllocationPath() {
    auto ModuleValue = std::make_shared<Module>();
    auto Body = std::make_shared<FunctionPrototype>();
    Builder Code; Code.Emit(Op::NewObject); Code.Emit(Op::Return);
    Body->Code = std::move(Code).Finish();
    auto Id = ModuleValue->AddFunction(Body);
    Vm Limited(ModuleValue, 1); // The permanent RootMetaObject occupies the only slot.
    try { Limited.Run(Id); }
    catch (const std::bad_alloc&) { return; }
    throw std::runtime_error("allocation limit did not reach fatal host path");
}
} // namespace

int main() {
    try {
        CyclesAndRoots(); NativeVisibilityAndBoundaries(); GlobalRoots();
        AutomaticCollection(); CollectionPendingAtThreshold(); NativeCollectionDeferral();
        FatalAllocationPath(); NativeRegistryChurn();
        std::cout << "M3 tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "M3 tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
