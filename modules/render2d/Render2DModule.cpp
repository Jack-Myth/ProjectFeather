#include <Feather/NativeModule.hpp>
#include "Render2D.hpp"

namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    Feather::Render2DLibrary Library(Context.Machine, Feather::CreateBgfxRender2DHost());
    return Library.GetModule();
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::Render2DModuleGuid,
        "render2d", &Create};
    return &Descriptor;
}
