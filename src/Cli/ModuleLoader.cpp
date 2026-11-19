#include "ModuleLoader.hpp"
#include "../FileModulePath.hpp"

#include <Feather/Import.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Feather::Cli {
namespace {
bool BareName(std::string_view Name) {
    if (Name.empty()) return false;
    for (std::size_t I = 0; I < Name.size(); ++I) {
        char C = Name[I];
        bool Letter = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
        bool Digit = C >= '0' && C <= '9';
        if (!Letter && C != '_' && !(I != 0 && Digit)) return false;
    }
    return true;
}
}

ModuleLoader::ModuleLoader(Vm& Machine, const std::filesystem::path& EntryPath,
                           ModuleFileKind Kind, NativeModules& Native,
                           const CompiledProgram* EntryProgram, std::size_t ObjectLimit)
    : Machine(Machine), RootPath(std::filesystem::canonical(EntryPath)),
      RootDirectory(RootPath.parent_path()), Kind(Kind), Native(Native),
      EntryProgram(EntryProgram), ObjectLimit(ObjectLimit) {
    Machine.SetModulePaths({PathText(Native.DefaultSearchPath())});
}

void ModuleLoader::InstallRootImport() { InstallModuleImport({}); }

void ModuleLoader::RunWorker(std::vector<std::string> Paths, NativeModuleGuid Owner,
                             void* State, const WorkerMain& Main) const {
    if (!EntryProgram) throw std::logic_error("host has no entry program for a worker VM");
    auto WorkerNative = Native.Fork();
    Vm WorkerVm(EntryProgram->Program, ObjectLimit);
    auto Loader = std::make_shared<ModuleLoader>(WorkerVm, RootPath, Kind, WorkerNative,
                                                 EntryProgram, ObjectLimit);
    NativeExecutionGuard Guard{*Loader, WorkerNative};
    Loader->WorkerOwner = Owner;
    Loader->WorkerState = State;
    Loader->EntryInitialized = false;
    WorkerVm.SetModulePaths(std::move(Paths));
    Loader->InstallRootImport();
    Main(*Loader);
    Loader->FinishNativeModules();
}

void* ModuleLoader::GetWorkerState(NativeModuleGuid Owner) const noexcept {
    return Owner == WorkerOwner ? WorkerState : nullptr;
}

void ModuleLoader::RegisterShutdown(std::function<void()> Finish, std::function<void()> Stop) {
    Shutdown.emplace_back(std::move(Finish), std::move(Stop));
}

void ModuleLoader::FinishNativeModules() {
    for (std::size_t I = 0; I < Shutdown.size(); ++I) {
        auto Finish = Shutdown[I].first;
        Finish();
    }
}

void ModuleLoader::StopNativeModules() noexcept {
    for (auto I = Shutdown.rbegin(); I != Shutdown.rend(); ++I) I->second();
    Shutdown.clear();
}

Value ModuleLoader::DispatchScript(const ScriptFunctionAddress& Address,
                                   std::span<const std::uint8_t> Arguments) {
    if (!Address.ModuleId.empty()) {
        auto Result = LoadFunctionModule(Address);
        if (Result.IsError()) return Result;
    } else if (!EntryInitialized) {
        if (EntryFailure) std::rethrow_exception(EntryFailure);
        if (EntryError.IsError()) return EntryError;
        try { EntryError = EntryProgram->Initialize(Machine); }
        catch (...) { EntryFailure = std::current_exception(); throw; }
        if (EntryError.IsError()) return EntryError;
        EntryError = {};
        EntryInitialized = true;
    }
    auto Values = Machine.DeserializeArguments(Arguments);
    return Address.ModuleId.empty() ? Machine.Run(Address.Constant, Values)
                                    : Machine.RunModule(Address.ModuleId, Address.Constant, Values);
}

Value ModuleLoader::LoadFunctionModule(const ScriptFunctionAddress& Address) {
    const auto& Id = Address.ModuleId;
    if (!Address.ModuleIdentity)
        throw std::invalid_argument("message module has no program identity");
    if (!Cache.contains(Id)) {
        auto Path = FileModulePath::Resolve(RootDirectory, Id);
        if (!Path)
            throw std::invalid_argument("message module has no file location: " + Id);
        LoadFile(*Path, false);
    }
    if (!Machine.IsModuleBuiltFrom(Id, nullptr, *Address.ModuleIdentity))
        throw std::runtime_error("message module differs from the sender's program: " + Id);
    return Machine.InitializeModule(Id, Cache.at(Id).Initializer);
}

std::vector<std::string> ModuleLoader::LoadedFiles() const {
    std::vector<std::string> Result;
    Result.reserve(Cache.size());
    for (const auto& [Id, _] : Cache) Result.push_back(Id);
    std::sort(Result.begin(), Result.end());
    return Result;
}

void ModuleLoader::InstallModuleImport(std::string_view Id) {
    auto Self = shared_from_this();
    RegisterModuleImport(Machine, Id,
        [Self](std::string_view Caller, std::string_view Specifier) {
            return Self->Resolve(Caller, Specifier);
        });
}

Value ModuleLoader::Resolve(std::string_view Caller, std::string_view Specifier) {
    if (!Specifier.starts_with("./") && !Specifier.starts_with("../")) {
        if (!BareName(Specifier)) return Value::Error("invalid module name: " + std::string(Specifier));
        std::vector<std::filesystem::path> Paths;
        for (const auto& Directory : Machine.GetModulePaths()) Paths.push_back(FileModulePath::FromUtf8(Directory));
        if (auto Result = Native.Resolve(Machine, Specifier, Paths, this)) return *Result;
        auto Extension = Kind == ModuleFileKind::Source ? ".fe" : ".fbc";
        for (const auto& Directory : Paths) {
            auto Candidate = Directory / (std::string(Specifier) + Extension);
            if (std::filesystem::is_regular_file(Candidate)) return LoadFile(Candidate);
        }
        return Value::Error("module not found: " + std::string(Specifier));
    }

    auto Relative = FileModulePath::FromUtf8(Specifier);
    if (Relative.is_absolute()) throw std::invalid_argument("file import must be relative");
    auto Extension = Kind == ModuleFileKind::Source ? ".fe" : ".fbc";
    if (Relative.extension().empty()) Relative += Extension;
    else if (Kind == ModuleFileKind::Bytecode && Relative.extension() == ".fe")
        Relative.replace_extension(".fbc");
    else if (Relative.extension() != Extension)
        throw std::invalid_argument("file import has the wrong extension for this runner");

    auto CallerPath = RootPath;
    if (!Caller.empty()) {
        auto Found = PathsById.find(std::string(Caller));
        if (Found == PathsById.end())
            throw std::logic_error("caller module has no file location");
        CallerPath = Found->second;
    }
    return LoadFile(CallerPath.parent_path() / Relative);
}

Value ModuleLoader::LoadFile(const std::filesystem::path& Candidate, bool Initialize) {
    auto Path = std::filesystem::canonical(Candidate);
    if (Path == RootPath)
        throw std::invalid_argument("entry file cannot import itself as a module");
    auto Id = FileModulePath::Id(RootDirectory, Path);
    if (auto Found = Cache.find(Id); Found != Cache.end()) {
        if (!Initialize) return Machine.GetModuleNamespace(Id);
        auto Result = Machine.InitializeModule(Id, Found->second.Initializer);
        return Result.IsError() ? Result : Machine.GetModuleNamespace(Id);
    }

    CompiledProgram Program;
    try {
#ifdef FEATHER_CLI_SOURCE_LOADER
        Program = Kind == ModuleFileKind::Source ? LoadSourceProgram(Path) :
            DeserializeProgram(ReadFile(Path));
#else
        if (Kind != ModuleFileKind::Bytecode)
            throw std::logic_error("source loading is not available in this runner");
        Program = DeserializeProgram(ReadFile(Path));
#endif
    } catch (const std::exception& Failure) {
        throw std::runtime_error("module " + Id + ": " + Failure.what());
    }
    if (Program.UsesQuickOperators)
        throw std::runtime_error("module " + Id +
                                 ": quick operators require host-provided __QuickOperator functions");
    Program.LoadInto(Machine, Id);
    auto [Found, Inserted] = Cache.emplace(Id, std::move(Program));
    (void)Inserted;
    PathsById.emplace(Id, Path);
    InstallModuleImport(Id);
    if (!Initialize) return Machine.GetModuleNamespace(Id);
    auto Result = Machine.InitializeModule(Id, Found->second.Initializer);
    return Result.IsError() ? Result : Machine.GetModuleNamespace(Id);
}

} // namespace Feather::Cli
