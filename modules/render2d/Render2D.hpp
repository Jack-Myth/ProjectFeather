#pragma once

#include <Feather/Runtime.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Feather {

inline constexpr NativeModuleGuid Render2DModuleGuid{{
    0x52, 0xe4, 0x12, 0x7b, 0x9d, 0xa8, 0x43, 0x75,
    0xa4, 0x1f, 0xc7, 0x63, 0xb8, 0x2e, 0x90, 0x16}};

struct Render2DConfig {
    std::string Title = "Feather";
    std::uint32_t WindowWidth = 1280;
    std::uint32_t WindowHeight = 720;
    std::uint32_t RenderWidth = 1280;
    std::uint32_t RenderHeight = 720;
    bool VSync = true;
    bool Resizable = true;
};

struct Render2DColor {
    float Red = 1.0f;
    float Green = 1.0f;
    float Blue = 1.0f;
    float Alpha = 1.0f;
};

struct Render2DRect {
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
};

struct Render2DWindowMetrics {
    int WindowWidth = 0, WindowHeight = 0;
    int PixelWidth = 0, PixelHeight = 0;
    float PixelDensity = 1, DisplayScale = 1;
    bool Minimized = false;
};
struct Render2DPoint {
    double X = 0, Y = 0;
    bool Inside = false;
};
// Viewport coordinates are window logical units; canvas coordinates use RenderWidth/Height.
inline Render2DPoint Render2DMapPoint(double X, double Y, Render2DRect View,
                                    double CanvasWidth, double CanvasHeight) {
    if (View.Width <= 0 || View.Height <= 0) return {};
    return {(X - View.X) * CanvasWidth / View.Width,
            (Y - View.Y) * CanvasHeight / View.Height,
            X >= View.X && Y >= View.Y && X < View.X + View.Width && Y < View.Y + View.Height};
}
struct Render2DEvent {
    std::string Type;
    double X = 0.0;
    double Y = 0.0;
    std::string Key;
    double WindowX = 0, WindowY = 0;
    bool InsideViewport = false;
    std::optional<Render2DWindowMetrics> Metrics;
};

class Render2DTexture {
public:
    virtual ~Render2DTexture() = default;
};

class Render2DFont {
public:
    virtual ~Render2DFont() = default;
};

class Render2DContext {
public:
    virtual ~Render2DContext() = default;
    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;
    virtual Render2DWindowMetrics GetWindowMetrics() const = 0;
    virtual void SetViewport(std::optional<Render2DRect> Viewport) = 0;
    virtual Render2DPoint WindowToCanvas(double X, double Y) const = 0;
    virtual void BeginRender(Render2DColor Clear) = 0;
    virtual void EndRender() = 0;
    virtual std::shared_ptr<Render2DTexture> LoadTexture(std::string_view Path) = 0;
    virtual std::shared_ptr<Render2DFont> LoadFont(std::string_view Path) = 0;
    virtual void DrawTexture(Render2DTexture& Texture, Render2DRect Destination,
                             Render2DColor Color) = 0;
    virtual void DrawLine(float X1, float Y1, float X2, float Y2, float Width,
                          Render2DColor Color) = 0;
    virtual void DrawText(Render2DFont& Font, std::string_view Text, float X, float Y,
                          float Size, Render2DColor Color) = 0;
    virtual std::optional<Render2DEvent> PollEvent() = 0;
    virtual std::optional<Render2DEvent> WaitEvent(std::int32_t TimeoutMilliseconds) = 0;
};

// Platform and graphics implementation used by the script-facing render2d module.
// Implementations may use SDL/bgfx, another renderer, or a test double.
class Render2DHost {
public:
    virtual ~Render2DHost() = default;
    virtual std::shared_ptr<Render2DContext> Create(const Render2DConfig& Config) = 0;
};

class Render2DLibrary final {
public:
    Render2DLibrary(Vm& Machine, std::shared_ptr<Render2DHost> Host);
    ~Render2DLibrary();
    Render2DLibrary(const Render2DLibrary&) = delete;
    Render2DLibrary& operator=(const Render2DLibrary&) = delete;

    Value GetModule() const;

private:
    std::shared_ptr<void> Shared;
    std::shared_ptr<NativeObject> ModuleObject;
};

// The standard SDL3 + bgfx backend. It is defined only when the optional
// render2d native module is built.
std::shared_ptr<Render2DHost> CreateBgfxRender2DHost();

} // namespace Feather
