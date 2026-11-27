#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace Feather {
inline std::optional<std::size_t> InvalidUtf8(std::string_view Input) {
    for (std::size_t I = 0; I < Input.size();) {
        auto Lead = static_cast<unsigned char>(Input[I]);
        if (Lead < 0x80) { ++I; continue; }
        std::size_t Width = Lead >= 0xC2 && Lead <= 0xDF ? 2 :
                            Lead >= 0xE0 && Lead <= 0xEF ? 3 :
                            Lead >= 0xF0 && Lead <= 0xF4 ? 4 : 0;
        if (Width == 0 || I + Width > Input.size()) return I;
        for (std::size_t J = 1; J < Width; ++J) {
            auto Byte = static_cast<unsigned char>(Input[I + J]);
            if (Byte < 0x80 || Byte > 0xBF) return I + J;
        }
        auto Second = static_cast<unsigned char>(Input[I + 1]);
        if ((Lead == 0xE0 && Second < 0xA0) || (Lead == 0xED && Second > 0x9F) ||
            (Lead == 0xF0 && Second < 0x90) || (Lead == 0xF4 && Second > 0x8F)) return I;
        I += Width;
    }
    return std::nullopt;
}

inline void ValidateUtf8(std::string_view Input) {
    if (InvalidUtf8(Input)) throw std::invalid_argument("invalid UTF-8 string");
}
} // namespace Feather
