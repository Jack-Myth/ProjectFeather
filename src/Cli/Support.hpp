#pragma once

#include <Feather/Compiler.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Feather::Cli {

constexpr std::size_t MaxFileBytes = 64 * 1024 * 1024;

std::string PathText(const std::filesystem::path& Path);
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& Path);
std::string ReadSource(const std::filesystem::path& Path);
void WriteFile(const std::filesystem::path& Path, std::span<const std::uint8_t> Bytes);
int RunProgram(const CompiledProgram& Program);

} // namespace Feather::Cli
