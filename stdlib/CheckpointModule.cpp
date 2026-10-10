#include <Feather/Checkpoint.hpp>
#include <Feather/NativeModule.hpp>

namespace {

Feather::Value Create(const Feather::NativeModuleContext& Context) {
    Feather::CheckpointLibrary Library(Context.Machine);
    return Library.GetModule();
}

} // namespace

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::CheckpointModuleGuid,
        "checkpoint",
        &Create};
    return &Descriptor;
}
