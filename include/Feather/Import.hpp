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
void RegisterImport(Vm& Machine, ImportCallback Callback);

} // namespace Feather
