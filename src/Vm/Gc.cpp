#include "Internal.hpp"

#include <algorithm>
#include <limits>

namespace Feather {
namespace {
constexpr std::size_t MinimumCollectionGrowth = 1024;

std::size_t NextSweepSize(std::size_t Live) {
    auto Growth = std::max(MinimumCollectionGrowth, Live);
    auto Maximum = std::numeric_limits<std::size_t>::max();
    return Live > Maximum - Growth ? Maximum : Live + Growth;
}
}

void Vm::UpdateCollectionThresholds() {
    NextCollectionObjectCount = std::min(NextSweepSize(ScriptHeap.size()), ScriptObjectLimit);
    CollectionPending = ScriptHeap.size() >= NextCollectionObjectCount;
    NextNativeRegistrySweep = NextSweepSize(NativeRegistry.size());
}

void Vm::RegisterNativeObject(const std::shared_ptr<NativeObject>& Input) {
    if (!Input) throw std::invalid_argument("null NativeObject");
    if (Input->Type && &Input->Type->GetVm() != this)
        throw std::invalid_argument("NativeObject type belongs to another VM");
    if (NativeRegistry.size() >= NextNativeRegistrySweep) {
        std::erase_if(NativeRegistry, [](const auto& Entry) { return Entry.expired(); });
        NextNativeRegistrySweep = NextSweepSize(NativeRegistry.size());
    }
    NativeRegistry.push_back(Input);
}

void Vm::CollectGarbageIfSafe() {
    if (ActiveRuns != 1 || ActiveNativeCalls != 0 || !ActiveExecution)
        return;
    (void)CollectGarbageImpl();
}

std::size_t Vm::CollectGarbage() {
    if (ActiveRuns != 0 || ActiveNativeCalls != 0)
        throw std::logic_error("GC requires an idle VM");
    return CollectGarbageImpl();
}

std::size_t Vm::CollectGarbageImpl() {
    struct MarkReset {
        std::vector<std::unique_ptr<ScriptObject>>& Heap;
        ~MarkReset() { for (auto& Entry : Heap) Entry->Marked = false; }
    } Reset{ScriptHeap};
    std::vector<ScriptObject*> ScriptWork;
    std::vector<std::shared_ptr<NativeObject>> NativeWork;
    std::unordered_set<NativeObject*> SeenNative;
    auto MarkScript = [&](ScriptObject* Input) {
        if (!ScriptObjects.contains(Input))
            throw std::invalid_argument("foreign or stale ScriptObject in GC graph");
        if (!Input->Marked) { Input->Marked = true; ScriptWork.push_back(Input); }
    };
    auto MarkValue = [&](const Value& Input) {
        if (Input.GetType() != ValueType::Object) return;
        if (Input.IsScriptObject()) MarkScript(Input.AsScriptObject());
        else {
            auto Native = std::dynamic_pointer_cast<NativeObject>(Input.AsNativeObject());
            if (Native && SeenNative.insert(Native.get()).second) NativeWork.push_back(std::move(Native));
        }
    };
    MarkScript(RootMetaObject);
    for (const auto& Entry : ConstantValues) MarkValue(Entry);
    for (const auto& [_, Entry] : Globals) MarkValue(Entry);
    for (const auto& [_, Instance] : LoadedModules) {
        for (const auto& Entry : Instance->ConstantValues) MarkValue(Entry);
        for (const auto& [__, Entry] : Instance->Globals) MarkValue(Entry);
        MarkValue(Instance->InitializationError);
        MarkValue(Value::FromObject(Instance->Namespace));
    }
    for (const auto& [_, Entry] : HostRoots) MarkValue(Entry);
    if (ActiveExecution)
        for (const auto& Current : ActiveExecution->Frames) {
            for (const auto& Entry : Current.Arguments) MarkValue(Entry);
            for (const auto& Entry : Current.Locals) MarkValue(Entry);
            for (const auto& Entry : Current.Stack) MarkValue(Entry);
        }
    std::erase_if(NativeRegistry, [&](const auto& Entry) {
        auto Native = Entry.lock();
        if (!Native) return true;
        if (SeenNative.insert(Native.get()).second) NativeWork.push_back(std::move(Native));
        return false;
    });
    while (!ScriptWork.empty() || !NativeWork.empty()) {
        while (!ScriptWork.empty()) {
            auto Script = ScriptWork.back(); ScriptWork.pop_back();
            if (Script->MetaObject) MarkScript(Script->MetaObject);
            for (const auto& [_, Entry] : Script->Members) MarkValue(Entry);
            for (const auto& Entry : Script->GetArrayElements()) MarkValue(Entry);
        }
        while (!NativeWork.empty()) {
            auto Native = std::move(NativeWork.back()); NativeWork.pop_back();
            for (const auto& [_, Entry] : Native->GcVisibleMembers) MarkValue(Entry);
        }
    }
    auto Before = ScriptHeap.size();
    std::erase_if(ScriptHeap, [&](const auto& Entry) {
        if (Entry->Marked) return false;
        ScriptObjects.erase(Entry.get());
        return true;
    });
    auto Collected = Before - ScriptHeap.size();
    ++CollectionCount;
    TotalCollectedScriptObjects += Collected;
    UpdateCollectionThresholds();
    return Collected;
}
GcStatistics Vm::GetGcStatistics() const {
    GcStatistics Result;
    Result.ScriptObjectCount = ScriptHeap.size();
    Result.EstimatedScriptBytes = ScriptHeap.size() * sizeof(ScriptObject);
    for (const auto& Script : ScriptHeap) {
        if (Script->Elements)
            Result.EstimatedScriptBytes += sizeof(*Script->Elements) + Script->Elements->capacity() * sizeof(Value);
        Result.ScriptMemberCount += Script->Members.size();
        Result.EstimatedScriptBytes += Script->Members.bucket_count() * sizeof(void*);
        for (const auto& [Key, _] : Script->Members)
            Result.EstimatedScriptBytes += sizeof(Key) + sizeof(Value) + 2 * sizeof(void*) + Key.Text.capacity();
    }
    Result.CollectionCount = CollectionCount;
    Result.TotalAllocatedScriptObjects = TotalAllocatedScriptObjects;
    Result.TotalCollectedScriptObjects = TotalCollectedScriptObjects;
    Result.NextCollectionObjectCount = NextCollectionObjectCount;
    Result.NativeRegistryEntries = NativeRegistry.size();
    return Result;
}

} // namespace Feather
