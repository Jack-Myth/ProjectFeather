#include <Feather/NativeModule.hpp>
#include <Feather/Snapshot.hpp>

#include <memory>

namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    if (!Context.Snapshots)
        return Feather::Value::FromObject(std::make_shared<Feather::ErrorObject>(
            "snapshot requires host snapshot services"));
    Feather::SnapshotLibrary Library(Context.Machine, *Context.Snapshots);
    return Library.GetModule();
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::SnapshotModuleGuid,
        "snapshot", &Create};
    return &Descriptor;
}
