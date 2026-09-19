#include "NativeModules.hpp"
#include "Support.hpp"

#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <dlfcn.h>
#else
#include <dlfcn.h>
#endif

namespace Feather::Cli {
namespace {

std::filesystem::path ExecutablePath() {
#ifdef _WIN32
    std::wstring Buffer(260, L'\0');
    while (true) {
        auto Count = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (!Count) throw std::runtime_error("cannot find executable path");
        if (Count < Buffer.size()) {
            Buffer.resize(Count);
            return std::filesystem::path(Buffer);
        }
        Buffer.resize(Buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t Length = 0;
    _NSGetExecutablePath(nullptr, &Length);
    std::string Buffer(Length, '\0');
    if (_NSGetExecutablePath(Buffer.data(), &Length) != 0)
        throw std::runtime_error("cannot find executable path");
    return std::filesystem::canonical(Buffer.c_str());
#else
    return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

std::string NativeSuffix() {
#ifdef _WIN32
    return ".felib.dll";
#elif defined(__APPLE__)
    return ".felib.dylib";
#else
    return ".felib.so";
#endif
}

std::shared_ptr<void> OpenLibrary(const std::filesystem::path& Path) {
#ifdef _WIN32
    auto* Handle = LoadLibraryExW(Path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!Handle) throw std::runtime_error("cannot load Native module " + PathText(Path) +
                                           " (Windows error " + std::to_string(GetLastError()) + ")");
    return {Handle, [](void* Input) { FreeLibrary(static_cast<HMODULE>(Input)); }};
#else
    auto* Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!Handle) throw std::runtime_error("cannot load Native module " + PathText(Path) +
                                           ": " + dlerror());
    return {Handle, [](void* Input) { dlclose(Input); }};
#endif
}

NativeModuleEntry GetEntry(void* Handle) {
#ifdef _WIN32
    auto* Symbol = GetProcAddress(static_cast<HMODULE>(Handle), NativeModuleEntryName);
#else
    auto* Symbol = dlsym(Handle, NativeModuleEntryName);
#endif
    if (!Symbol) throw std::runtime_error("Native module has no FeatherNativeModuleV2 entry");
    return reinterpret_cast<NativeModuleEntry>(Symbol);
}

} // namespace

NativeModules::NativeModules(std::istream& Input, std::ostream& Output)
    : Input(Input), Output(Output) {
    Paths.push_back((ExecutablePath().parent_path() / "modules").lexically_normal());
}

void NativeModules::ReleaseRoots() noexcept {
    ByName.clear();
    for (auto& Library : Libraries) Library.Root = {};
}

std::vector<NativeModuleIdentity> NativeModules::LoadedIdentities() const {
    std::vector<NativeModuleIdentity> Result;
    Result.reserve(Libraries.size());
    for (const auto& Library : Libraries)
        Result.push_back(NativeModuleIdentity{Library.Name, Library.Id});
    return Result;
}

bool NativeModules::HasIdentity(std::string_view Name, const NativeModuleGuid& Id) const {
    for (const auto& Library : Libraries)
        if (Library.Name == Name) return Library.Id == Id;
    return false;
}

std::optional<Value> NativeModules::Resolve(Vm& Machine, std::string_view Name) {
    if (auto Found = ByName.find(std::string(Name)); Found != ByName.end()) return Found->second;
    for (const auto& Directory : Paths) {
        auto Candidate = Directory / (std::string(Name) + NativeSuffix());
        if (!std::filesystem::is_regular_file(Candidate)) continue;
        auto Path = std::filesystem::canonical(Candidate);
        auto Handle = OpenLibrary(Path);
        auto* Descriptor = GetEntry(Handle.get())();
        bool AnyGuidByte = false;
        if (Descriptor)
            for (auto Byte : Descriptor->Id.Bytes) AnyGuidByte = AnyGuidByte || Byte != 0;
        if (!Descriptor || Descriptor->InterfaceVersion != NativeModuleInterfaceVersion ||
            !AnyGuidByte || !Descriptor->Name || Name != Descriptor->Name || !Descriptor->Create)
            throw std::runtime_error("Native module descriptor mismatch: " + PathText(Path));
        for (const auto& Library : Libraries)
            if (Library.Id == Descriptor->Id)
                throw std::runtime_error("duplicate Native module GUID: " + PathText(Path));
        auto Root = Descriptor->Create(NativeModuleContext{Machine, &Input, &Output, Snapshots});
        if (Root.IsError()) return Root;
        if (Root.GetType() != ValueType::Object || Root.IsScriptObject() ||
            Root.GetObjectType() != ObjectType::Host)
            throw std::runtime_error("Native module must return a Host object: " + PathText(Path));
        Libraries.push_back(Loaded{std::move(Handle), Descriptor->Id,
                                   std::string(Name), Root});
        ByName.emplace(std::string(Name), Root);
        return Root;
    }
    return std::nullopt;
}

} // namespace Feather::Cli
