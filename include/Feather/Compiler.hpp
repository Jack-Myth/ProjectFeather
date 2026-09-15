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

struct CompiledProgram {
    std::shared_ptr<Module> Program;
    std::uint32_t Initializer = 0;
    std::unordered_map<std::string, std::uint32_t> Functions;
    bool UsesQuickOperators = false;

    // Bind functions and run top-level statements after the host registers native globals.
    // Repeating this call repeats top-level side effects.
    Value Initialize(Vm& Machine) const;
};

// Compile a complete UTF-8 source unit. Throws std::runtime_error on a source error.
CompiledProgram Compile(std::string_view Source);

// Experimental versioned bytecode artifact. Decode rejects malformed input before execution.
std::vector<std::uint8_t> SerializeProgram(const CompiledProgram& Input,
                                           std::size_t MaxBytes = 64 * 1024 * 1024);
CompiledProgram DeserializeProgram(std::span<const std::uint8_t> Bytes,
                                   std::size_t MaxBytes = 64 * 1024 * 1024);

} // namespace Feather
