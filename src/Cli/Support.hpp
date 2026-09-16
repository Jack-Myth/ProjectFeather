#pragma once

#include <Feather/Compiler.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Feather::Cli {

constexpr std::size_t MaxFileBytes = 64 * 1024 * 1024;

std::string PathText(const std::filesystem::path& Path);
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& Path);
std::string ReadSource(const std::filesystem::path& Path);
void WriteFile(const std::filesystem::path& Path, std::span<const std::uint8_t> Bytes);
#ifdef FEATHER_CLI_SOURCE_LOADER
// Prefer a validated, sibling .fbc that is at least as new as SourcePath.
// Missing, stale, unreadable, or invalid cache files fall back to source compilation.
CompiledProgram LoadSourceProgram(const std::filesystem::path& SourcePath);
#endif
enum class ModuleFileKind { Source, Bytecode };
int RunProgram(const CompiledProgram& Program, const std::filesystem::path& EntryPath,
               ModuleFileKind Kind,
               std::shared_ptr<VmDebugController> DebugController = {});

} // namespace Feather::Cli
