#include <Feather/NativeModule.hpp>
#include "Network.hpp"
namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    Feather::NetworkLibrary Library(Context.Machine);
    return Library.GetModule();
}
}
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::NetworkModuleGuid, "network", &Create};
    return &Descriptor;
}
