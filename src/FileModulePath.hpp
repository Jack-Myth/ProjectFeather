#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Feather::FileModulePath {

inline std::filesystem::path FromUtf8(std::string_view Text) {
    return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

inline std::string ToUtf8(const std::filesystem::path& Path) {
    auto Bytes = Path.u8string();
    return {Bytes.begin(), Bytes.end()};
}

inline std::string Id(const std::filesystem::path& Directory, const std::filesystem::path& File) {
    auto Relative = File.lexically_relative(Directory);
#ifdef _WIN32
    // A UNC root_name identifies the server. Each share has its own root directory.
    if (!Relative.empty() && File.has_relative_path() && Directory.has_relative_path() &&
        File.root_name().native().size() > 2 &&
        *File.relative_path().begin() != *Directory.relative_path().begin())
        Relative.clear();
#endif
    auto Text = ToUtf8(Relative.empty() ? File : Relative);
#ifdef _WIN32
    std::replace(Text.begin(), Text.end(), '\\', '/');
#endif
    if (!Relative.empty() && *Relative.begin() != "..") Text.insert(0, "./");
    return Text;
}

inline std::optional<std::filesystem::path> Resolve(const std::filesystem::path& Directory,
                                                    std::string_view Id) {
    auto Path = FromUtf8(Id);
    if (Id.starts_with("./") || Id.starts_with("../"))
        return (Directory / Path).lexically_normal();
    if (Path.is_absolute()) return Path;
    return std::nullopt;
}

} // namespace Feather::FileModulePath
