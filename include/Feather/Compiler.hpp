#pragma once

#include <Feather/Runtime.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Feather {

struct FEATHER_API CompiledProgram {
    std::shared_ptr<Module> Program;
    std::uint32_t Initializer = 0;
    std::unordered_map<std::string, std::uint32_t> Functions;
    std::vector<std::string> Exports;
    bool UsesQuickOperators = false;

    // Bind functions and run top-level statements after the host registers native globals.
    // Repeating this call repeats top-level side effects.
    Value Initialize(Vm& Machine) const;
    void LoadInto(Vm& Machine, std::string Id) const;
    Value InitializeModule(Vm& Machine, std::string_view Id) const;
};

// Compile a complete UTF-8 source unit. Throws std::runtime_error on a source error.
FEATHER_API CompiledProgram Compile(std::string_view Source);

// Experimental versioned bytecode artifact. Decode rejects malformed input before execution.
FEATHER_API std::vector<std::uint8_t> SerializeProgram(const CompiledProgram& Input,
                                           std::size_t MaxBytes = 64 * 1024 * 1024);
FEATHER_API CompiledProgram DeserializeProgram(std::span<const std::uint8_t> Bytes,
                                   std::size_t MaxBytes = 64 * 1024 * 1024);

// Optional companion symbol artifact. The bytecode format and VM semantics stay unchanged.
// Attach before constructing a VM; rejects damaged or mismatched symbols without mutation.
FEATHER_API std::vector<std::uint8_t> SerializeSymbols(const CompiledProgram& Input,
                                           std::size_t MaxBytes = 64 * 1024 * 1024);
FEATHER_API void AttachSymbols(CompiledProgram& Input, std::span<const std::uint8_t> Bytes,
                   std::size_t MaxBytes = 64 * 1024 * 1024);

} // namespace Feather
