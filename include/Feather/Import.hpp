#pragma once

#include <Feather/Runtime.hpp>

#include <functional>
#include <string_view>

namespace Feather {

// Module-aware adapter. The host resolves the specifier and may load a Feather module
// into this VM; same-VM script objects/functions may be returned. VM ownership checks
// still reject direct values from another VM.
using ModuleImportCallback = std::function<Value(std::string_view CallerModuleId,
                                                  std::string_view Specifier)>;
FEATHER_API void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback);

} // namespace Feather
