#pragma once

#include <Feather/Runtime.hpp>

#include <functional>
#include <string_view>

namespace Feather {

// Module-aware adapter. The host resolves the specifier and may load a Feather module
// into this VM. The callback must return script objects and functions from this VM.
// Hosts can use Vm::ValidateOwnedValue at this boundary.
using ModuleImportCallback = std::function<Value(std::string_view CallerModuleId,
                                                  std::string_view Specifier)>;
FEATHER_API void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback);

} // namespace Feather
