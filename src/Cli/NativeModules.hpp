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
    std::optional<Value> Resolve(Vm& Machine, std::string_view Name,
                                 const std::vector<std::filesystem::path>& Paths,
                                 NativeModuleHost* Host = nullptr);
    NativeModules Fork() const { return NativeModules(Input, Output); }
    void ReleaseRoots() noexcept;
    std::filesystem::path DefaultSearchPath() const;

private:
    struct Loaded {
        NativeModuleGuid Id;
        std::string Name;
        Value Root;
    };
    std::istream& Input;
    std::ostream& Output;
    std::unordered_map<std::filesystem::path, std::shared_ptr<void>> Handles;
    std::vector<Loaded> Libraries;
    std::unordered_map<std::string, Value> ByName;
};

} // namespace Feather::Cli
