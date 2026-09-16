#include <Feather/NativeModule.hpp>
#include <Feather/StdIo.hpp>

#include <memory>

namespace {
Feather::Value Create(const Feather::NativeModuleContext& Context) {
    if (!Context.Input || !Context.Output)
        return Feather::Value::FromObject(std::make_shared<Feather::ErrorObject>(
            "stdio requires input and output streams"));
    Feather::StdIoLibrary Library(*Context.Input, *Context.Output);
    return Library.GetModule();
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModuleV1() {
    static const Feather::NativeModuleDescriptor Descriptor{
        Feather::NativeModuleInterfaceVersion, "stdio", &Create};
    return &Descriptor;
}
