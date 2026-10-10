#pragma once
#include <Feather/Runtime.hpp>
namespace Feather {
inline constexpr NativeModuleGuid NetworkModuleGuid{{
    0x29, 0xb8, 0x63, 0xaa, 0xf7, 0x08, 0x4f, 0xc3,
    0xad, 0x57, 0x73, 0x05, 0x10, 0xc8, 0x4d, 0x11}};
struct NetworkLimits {
    std::size_t MaxSockets = 256;
    std::size_t MaxTransferBytes = 1024 * 1024;
};
// Exposing this optional library grants network access, like exposing stdio grants file access.
// A host that disallows networking must not register/import it.
class NetworkLibrary final {
public:
    explicit NetworkLibrary(Vm& Machine, NetworkLimits Limits = {});
    ~NetworkLibrary();
    NetworkLibrary(const NetworkLibrary&) = delete;
    NetworkLibrary& operator=(const NetworkLibrary&) = delete;
    Value GetModule() const;
private:
    std::shared_ptr<NativeObject> ModuleObject;
};
} // namespace Feather
