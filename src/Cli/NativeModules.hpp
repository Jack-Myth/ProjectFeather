#pragma once

#include <Feather/NativeModule.hpp>

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Feather::Cli {

struct NativeModuleIdentity {
    std::string Name;
    NativeModuleGuid Id;
};

// Owned by the CLI host; construct before Vm and destroy after Vm.
class NativeModules final {
public:
    NativeModules(std::istream& Input, std::ostream& Output);
    void SetSnapshotHost(SnapshotHost* Input) { Snapshots = Input; }
    std::optional<Value> Resolve(Vm& Machine, std::string_view Name);
    void ReleaseRoots() noexcept;
    const std::vector<std::filesystem::path>& SearchPaths() const { return Paths; }
    std::vector<NativeModuleIdentity> LoadedIdentities() const;
    bool HasIdentity(std::string_view Name, const NativeModuleGuid& Id) const;

private:
    struct Loaded {
        std::shared_ptr<void> Handle;
        NativeModuleGuid Id;
        std::string Name;
        Value Root;
    };
    std::istream& Input;
    std::ostream& Output;
    std::vector<std::filesystem::path> Paths;
    std::vector<Loaded> Libraries;
    std::unordered_map<std::string, Value> ByName;
    SnapshotHost* Snapshots = nullptr;
};

} // namespace Feather::Cli
