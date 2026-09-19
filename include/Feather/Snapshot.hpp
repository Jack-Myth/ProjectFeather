#pragma once

#include <Feather/Runtime.hpp>

#include <memory>
#include <string_view>

namespace Feather {

inline constexpr NativeModuleGuid SnapshotModuleGuid{{
    0x75, 0xcb, 0xe4, 0x89, 0x6e, 0xac, 0x49, 0xb5,
    0xb4, 0xb6, 0x4d, 0x2d, 0x98, 0x4c, 0x2f, 0x65}};

// Implemented by a host that can persist a VM snapshot and replace the active
// execution session. Restore may perform a non-local control transfer on success.
class FEATHER_API SnapshotHost {
public:
    virtual ~SnapshotHost() = default;
    virtual Value Checkpoint(Vm& Machine, std::string_view Path) = 0;
    virtual Value Restore(std::string_view Path) = 0;
};

class FEATHER_API SnapshotLibrary final {
public:
    SnapshotLibrary(Vm& Machine, SnapshotHost& Host);
    ~SnapshotLibrary();
    SnapshotLibrary(const SnapshotLibrary&) = delete;
    SnapshotLibrary& operator=(const SnapshotLibrary&) = delete;

    Value GetModule() const;

private:
    std::shared_ptr<NativeObject> ModuleObject;
};

} // namespace Feather
