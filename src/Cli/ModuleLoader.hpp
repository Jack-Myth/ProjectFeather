#pragma once

#include "Support.hpp"
#include "NativeModules.hpp"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Feather::Cli {

class ModuleLoader final : public std::enable_shared_from_this<ModuleLoader> {
public:
    ModuleLoader(Vm& Machine, const std::filesystem::path& EntryPath,
                 ModuleFileKind Kind, NativeModules& Native);
    void InstallRootImport();
    std::vector<std::string> LoadedFiles() const;
    void PrepareSnapshotModules(const std::vector<std::string>& Files);

private:
    Value Resolve(std::string_view Caller, std::string_view Specifier);
    Value LoadFile(const std::filesystem::path& Path, bool Initialize = true);
    void InstallModuleImport(std::string_view Id);
    std::string LogicalId(const std::filesystem::path& Path) const;

    Vm& Machine;
    std::filesystem::path RootPath;
    std::filesystem::path RootDirectory;
    ModuleFileKind Kind;
    NativeModules& Native;
    std::unordered_map<std::string, CompiledProgram> Cache;
    std::unordered_map<std::string, std::filesystem::path> PathsById;
};

} // namespace Feather::Cli
