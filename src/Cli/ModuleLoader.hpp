#pragma once

#include "Support.hpp"
#include "NativeModules.hpp"

#include <memory>
#include <string_view>
#include <unordered_map>

namespace Feather::Cli {

class ModuleLoader final : public std::enable_shared_from_this<ModuleLoader> {
public:
    ModuleLoader(Vm& Machine, const std::filesystem::path& EntryPath,
                 ModuleFileKind Kind, NativeModules& Native);
    void InstallRootImport();

private:
    Value Resolve(std::string_view Caller, std::string_view Specifier);
    Value LoadFile(const std::filesystem::path& Path);
    void InstallModuleImport(std::string_view Id);

    Vm& Machine;
    std::filesystem::path RootPath;
    ModuleFileKind Kind;
    NativeModules& Native;
    std::unordered_map<std::string, CompiledProgram> Cache;
};

} // namespace Feather::Cli
