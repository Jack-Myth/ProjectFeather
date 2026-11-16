#pragma once

#include "Support.hpp"
#include "NativeModules.hpp"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Feather::Cli {

class ModuleLoader final : public NativeModuleHost, public std::enable_shared_from_this<ModuleLoader> {
public:
    ModuleLoader(Vm& Machine, const std::filesystem::path& EntryPath,
                 ModuleFileKind Kind, NativeModules& Native,
                 const CompiledProgram* EntryProgram = nullptr,
                 std::size_t ObjectLimit = std::numeric_limits<std::size_t>::max());
    void InstallRootImport();
    void FinishNativeModules();
    void StopNativeModules() noexcept;
    std::vector<std::string> LoadedFiles() const;

private:
    Value Resolve(std::string_view Caller, std::string_view Specifier);
    Value LoadFile(const std::filesystem::path& Path, bool Initialize = true);
    Value LoadFunctionModule(const ScriptFunctionAddress& Address);
    void InstallModuleImport(std::string_view Id);
    std::string LogicalId(const std::filesystem::path& Path) const;
    void RunWorker(std::vector<std::string> Paths, NativeModuleGuid Owner,
                   void* State, const WorkerMain& Main) const override;
    void* GetWorkerState(NativeModuleGuid Owner) const noexcept override;
    Value DispatchScript(const ScriptFunctionAddress& Address,
                         std::span<const std::uint8_t> Arguments) override;
    void RegisterShutdown(std::function<void()> Finish, std::function<void()> Stop) override;

    Vm& Machine;
    std::filesystem::path RootPath;
    std::filesystem::path RootDirectory;
    ModuleFileKind Kind;
    NativeModules& Native;
    const CompiledProgram* EntryProgram;
    std::size_t ObjectLimit;
    std::unordered_map<std::string, CompiledProgram> Cache;
    std::unordered_map<std::string, std::filesystem::path> PathsById;
    NativeModuleGuid WorkerOwner{};
    void* WorkerState = nullptr;
    bool EntryInitialized = true;
    Value EntryError;
    std::exception_ptr EntryFailure;
    std::vector<std::pair<std::function<void()>, std::function<void()>>> Shutdown;
};

// Destroy host callbacks and native roots while their VM and DLLs are still alive.
struct NativeExecutionGuard {
    ModuleLoader& Loader;
    NativeModules& Native;
    ~NativeExecutionGuard() { Loader.StopNativeModules(); Native.ReleaseRoots(); }
};

} // namespace Feather::Cli
