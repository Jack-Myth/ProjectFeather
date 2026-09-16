#pragma once

#include <Feather/Runtime.hpp>

#include <cstdint>
#include <iosfwd>

namespace Feather {

struct NativeModuleContext {
    Vm& Machine;
    std::istream* Input = nullptr;
    std::ostream* Output = nullptr;
};

struct NativeModuleDescriptor {
    std::uint32_t InterfaceVersion;
    const char* Name;
    Value (*Create)(const NativeModuleContext&);
};

inline constexpr std::uint32_t NativeModuleInterfaceVersion = 1;
inline constexpr const char* NativeModuleEntryName = "FeatherNativeModuleV1";
using NativeModuleEntry = const NativeModuleDescriptor* (*)();

} // namespace Feather

// A native module defines this function with FEATHER_NATIVE_EXPORT. Its C linkage
// fixes the symbol name; the descriptor and Feather values still use the C++ ABI.
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModuleV1();
