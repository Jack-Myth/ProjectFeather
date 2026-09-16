#pragma once

#include <Feather/Runtime.hpp>

#include <functional>
#include <string_view>

namespace Feather {

// Host-defined resolution and loading. The VM does not inspect the specifier.
using ImportCallback = std::function<Value(std::string_view Specifier)>;

// Register the ordinary global function `import` before program initialization.
// Wrong arity/type yields an Error value. The callback owns IO, caching and proxies.
// This adapter accepts primitive values and host NativeObjects, never script objects/functions.
FEATHER_API void RegisterImport(Vm& Machine, ImportCallback Callback);

// Module-aware adapter. The host resolves the specifier and may load a Feather module
// into this VM; same-VM script objects/functions may be returned. VM ownership checks
// still reject direct values from another VM.
using ModuleImportCallback = std::function<Value(std::string_view CallerModuleId,
                                                  std::string_view Specifier)>;
FEATHER_API void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback);

} // namespace Feather
