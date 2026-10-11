#pragma once
#include "Render2D.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Feather {
// Two triangles with flat end caps, computed in double to avoid float overflow
// while subtracting distant endpoints. The backend's vertex format is float.
inline std::optional<std::array<Render2DPoint, 4>> Render2DLineQuad(
    float X1, float Y1, float X2, float Y2, float Width) {
    if (!std::isfinite(X1) || !std::isfinite(Y1) || !std::isfinite(X2) ||
        !std::isfinite(Y2) || !std::isfinite(Width) || Width <= 0)
        throw std::invalid_argument("line coordinates must be finite and width must be positive");
    const double DX = double(X2) - X1, DY = double(Y2) - Y1;
    const double Length = std::hypot(DX, DY);
    if (Length == 0) return std::nullopt;
    const double NX = -DY / Length * (double(Width) / 2);
    const double NY = DX / Length * (double(Width) / 2);
    std::array<Render2DPoint, 4> Quad{{
        {X1 + NX, Y1 + NY}, {X2 + NX, Y2 + NY},
        {X2 - NX, Y2 - NY}, {X1 - NX, Y1 - NY}}};
    for (const auto& P : Quad)
        if (!std::isfinite(P.X) || !std::isfinite(P.Y) ||
            std::abs(P.X) > std::numeric_limits<float>::max() ||
            std::abs(P.Y) > std::numeric_limits<float>::max())
            throw std::invalid_argument("line geometry exceeds the render coordinate range");
    return Quad;
}
} // namespace Feather
