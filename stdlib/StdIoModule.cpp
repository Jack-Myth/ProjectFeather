#include <Feather/NativeModule.hpp>
#include <Feather/StdIo.hpp>

#include <memory>

namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    if (!Context.Input || !Context.Output)
        return Feather::Value::FromObject(std::make_shared<Feather::ErrorObject>(
            "stdio requires input and output streams"));
    Feather::StdIoLibrary Library(Context.Machine, *Context.Input, *Context.Output);
    return Library.GetModule();
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::StdIoModuleGuid,
        "stdio", &Create};
    return &Descriptor;
}
