#include <Feather/NativeModule.hpp>
#include "Time.hpp"

namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    Feather::TimeLibrary Library(Context.Machine);
    return Library.GetModule();
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModuleV2() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::NativeModuleInterfaceVersion,
        Feather::TimeModuleGuid,
        "time", &Create};
    return &Descriptor;
}
