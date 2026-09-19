#pragma once

#include "ModuleLoader.hpp"
#include <Feather/Snapshot.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Feather::Cli {

struct SnapshotArchive {
    std::vector<NativeModuleIdentity> NativeModules;
    std::vector<std::string> FeatherModules;
    std::vector<std::uint8_t> VmBytes;
};

struct SnapshotRestoreRequest {
    SnapshotArchive Archive;
};

class CliSnapshotHost final : public SnapshotHost {
public:
    CliSnapshotHost(ModuleLoader& Loader, NativeModules& Native)
        : Loader(Loader), Native(Native) {}
    Value Checkpoint(Vm& Machine, std::string_view Path) override;
    Value Restore(std::string_view Path) override;

private:
    ModuleLoader& Loader;
    NativeModules& Native;
};

} // namespace Feather::Cli
