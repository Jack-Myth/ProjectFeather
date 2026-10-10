#pragma once

#include <Feather/Runtime.hpp>

namespace Feather {

inline constexpr NativeModuleGuid CheckpointModuleGuid{{
    0x62, 0x75, 0x2d, 0x1c, 0x29, 0x9b, 0x48, 0x85,
    0xa8, 0x4d, 0x6e, 0x63, 0xd0, 0xce, 0x73, 0x11}};

class FEATHER_API CheckpointLibrary final {
public:
    explicit CheckpointLibrary(Vm& Machine);
    ~CheckpointLibrary();
    CheckpointLibrary(const CheckpointLibrary&) = delete;
    CheckpointLibrary& operator=(const CheckpointLibrary&) = delete;
    Value GetModule() const;
private:
    std::shared_ptr<NativeObject> ModuleObject;
};

} // namespace Feather
