#include "ModuleLoader.hpp"

#include <Feather/Import.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Feather::Cli {
namespace {
std::filesystem::path Utf8Path(std::string_view Text) {
    std::u8string Bytes;
    Bytes.reserve(Text.size());
    for (unsigned char Byte : Text) Bytes.push_back(static_cast<char8_t>(Byte));
    return std::filesystem::path(Bytes);
}
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
Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}
}

ModuleLoader::ModuleLoader(Vm& Machine, const std::filesystem::path& EntryPath,
                           ModuleFileKind Kind, NativeModules& Native)
    : Machine(Machine), RootPath(std::filesystem::canonical(EntryPath)),
      RootDirectory(RootPath.parent_path()), Kind(Kind), Native(Native) {}

void ModuleLoader::InstallRootImport() { InstallModuleImport({}); }

std::vector<std::string> ModuleLoader::LoadedFiles() const {
    std::vector<std::string> Result;
    Result.reserve(Cache.size());
    for (const auto& [Id, _] : Cache) Result.push_back(Id);
    std::sort(Result.begin(), Result.end());
    return Result;
}

void ModuleLoader::PrepareSnapshotModules(const std::vector<std::string>& Files) {
    for (const auto& File : Files) {
        constexpr std::string_view Prefix = "file:";
        if (!std::string_view(File).starts_with(Prefix))
            throw std::invalid_argument("invalid snapshot Feather module ID");
        auto Relative = Utf8Path(std::string_view(File).substr(Prefix.size()));
        if (Relative.empty() || Relative.is_absolute())
            throw std::invalid_argument("invalid snapshot Feather module path");
        auto Candidate = std::filesystem::canonical(RootDirectory / Relative);
        if (LogicalId(Candidate) != File)
            throw std::invalid_argument("snapshot Feather module path mismatch");
        (void)LoadFile(Candidate, false);
    }
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
        if (!BareName(Specifier)) return Error("invalid module name: " + std::string(Specifier));
        if (auto Result = Native.Resolve(Machine, Specifier)) return *Result;
        auto Extension = Kind == ModuleFileKind::Source ? ".fe" : ".fbc";
        for (const auto& Directory : Native.SearchPaths()) {
            auto Candidate = Directory / (std::string(Specifier) + Extension);
            if (std::filesystem::is_regular_file(Candidate)) return LoadFile(Candidate);
        }
        return Error("module not found: " + std::string(Specifier));
    }

    auto Relative = Utf8Path(Specifier);
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

std::string ModuleLoader::LogicalId(const std::filesystem::path& Path) const {
    auto Relative = Path.lexically_relative(RootDirectory);
    if (Relative.empty() || Relative.is_absolute())
        throw std::invalid_argument(
            "Feather module cannot be represented relative to the entry directory");
    auto Utf8 = Relative.generic_u8string();
    std::string Result = "file:";
    Result.reserve(Result.size() + Utf8.size());
    for (char8_t Byte : Utf8) Result.push_back(static_cast<char>(Byte));
    return Result;
}

Value ModuleLoader::LoadFile(const std::filesystem::path& Candidate, bool Initialize) {
    auto Path = std::filesystem::canonical(Candidate);
    if (Path == RootPath)
        throw std::invalid_argument("entry file cannot import itself as a module");
    auto Id = LogicalId(Path);
    if (auto Found = Cache.find(Id); Found != Cache.end()) {
        if (!Initialize) return Machine.GetModuleNamespace(Id);
        auto Result = Found->second.InitializeModule(Machine, Id);
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
    auto Result = Found->second.InitializeModule(Machine, Id);
    return Result.IsError() ? Result : Machine.GetModuleNamespace(Id);
}

} // namespace Feather::Cli
