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

// Owned by the CLI host; construct before Vm and destroy after Vm.
class NativeModules final {
public:
    NativeModules(std::istream& Input, std::ostream& Output);
    std::optional<Value> Resolve(Vm& Machine, std::string_view Name);
    void ReleaseRoots() noexcept;
    const std::vector<std::filesystem::path>& SearchPaths() const { return Paths; }

private:
    struct Loaded {
        std::shared_ptr<void> Handle;
        Value Root;
    };
    std::istream& Input;
    std::ostream& Output;
    std::vector<std::filesystem::path> Paths;
    std::vector<Loaded> Libraries;
    std::unordered_map<std::string, Value> ByName;
};

} // namespace Feather::Cli
