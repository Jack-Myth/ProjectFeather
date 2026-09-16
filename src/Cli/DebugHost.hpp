#pragma once

#include "Support.hpp"

#include <string_view>

namespace Feather::Cli {

int RunDebugProgram(const CompiledProgram& Program, const std::filesystem::path& EntryPath,
                    ModuleFileKind Kind, std::string_view ListenEndpoint,
                    bool WaitForDebugger);

} // namespace Feather::Cli
