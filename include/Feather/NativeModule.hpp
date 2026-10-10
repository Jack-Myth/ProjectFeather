#pragma once

#include <Feather/Runtime.hpp>

#include <iosfwd>

namespace Feather {

class SnapshotHost;

struct NativeModuleContext {
    Vm& Machine;
    std::istream* Input = nullptr;
    std::ostream* Output = nullptr;
    SnapshotHost* Snapshots = nullptr;
};

struct NativeModuleDescriptor {
    NativeModuleGuid Id;
    const char* Name;
    Value (*Create)(const NativeModuleContext&);
};

inline constexpr const char* NativeModuleEntryName = "FeatherNativeModule";
using NativeModuleEntry = const NativeModuleDescriptor* (*)();

} // namespace Feather

// A native module defines this function with FEATHER_NATIVE_EXPORT. Its C linkage
// fixes the symbol name; the descriptor and Feather values still use the C++ ABI.
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule();
