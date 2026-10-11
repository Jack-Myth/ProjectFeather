#include "LineGeometry.hpp"
#include "Render2D.hpp"

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb/stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

#include "generated/vs_quad_dx11.h"
#include "generated/fs_quad_dx11.h"
#include "generated/vs_quad_spirv.h"
#include "generated/fs_quad_spirv.h"
#include "generated/vs_quad_glsl.h"
#include "generated/fs_quad_glsl.h"
#include "generated/vs_quad_essl.h"
#include "generated/fs_quad_essl.h"
#include "generated/vs_quad_metal.h"
#include "generated/fs_quad_metal.h"
#include "generated/vs_quad_wgsl.h"
#include "generated/fs_quad_wgsl.h"

namespace Feather {
namespace {

constexpr bgfx::ViewId MainView = 0;

struct Vertex {
    float X, Y, Z;
    std::uint32_t Color;
    float U, V;
};

struct DeviceState {
    bool Alive = false;
    std::unordered_set<std::uint16_t> Textures;
    bgfx::VertexLayout Layout;
    bgfx::ProgramHandle Program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle Sampler = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle White = BGFX_INVALID_HANDLE;
};

std::filesystem::path PathFromUtf8(std::string_view Input) {
    std::u8string Utf8;
    Utf8.reserve(Input.size());
    for (unsigned char Byte : Input) Utf8.push_back(static_cast<char8_t>(Byte));
    return std::filesystem::path(Utf8);
}

std::vector<std::uint8_t> ReadFile(std::string_view Path) {
    std::ifstream Input(PathFromUtf8(Path), std::ios::binary | std::ios::ate);
    if (!Input) throw std::runtime_error("cannot open render2d resource: " + std::string(Path));
    auto End = Input.tellg();
    if (End < 0 || static_cast<std::uint64_t>(End) > 256ull * 1024 * 1024)
        throw std::runtime_error("render2d resource is too large");
    std::vector<std::uint8_t> Result(static_cast<std::size_t>(End));
    Input.seekg(0);
    if (!Result.empty() && !Input.read(reinterpret_cast<char*>(Result.data()), Result.size()))
        throw std::runtime_error("cannot read render2d resource: " + std::string(Path));
    return Result;
}

std::uint32_t PackColor(Render2DColor Color) {
    auto Byte = [](float Value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
    };
    return Byte(Color.Red) | (Byte(Color.Green) << 8) | (Byte(Color.Blue) << 16) |
           (Byte(Color.Alpha) << 24);
}

std::uint32_t PackClearColor(Render2DColor Color) {
    auto Byte = [](float Value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
    };
    return (Byte(Color.Red) << 24) | (Byte(Color.Green) << 16) |
           (Byte(Color.Blue) << 8) | Byte(Color.Alpha);
}

template<std::size_t N>
bgfx::ShaderHandle Shader(const std::uint8_t (&Bytes)[N]) {
    return bgfx::createShader(bgfx::makeRef(Bytes, static_cast<std::uint32_t>(N)));
}

bgfx::ProgramHandle CreateProgram(bgfx::RendererType::Enum Renderer) {
    bgfx::ShaderHandle Vertex = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle Fragment = BGFX_INVALID_HANDLE;
    switch (Renderer) {
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
        Vertex = Shader(vs_quad_dx11); Fragment = Shader(fs_quad_dx11); break;
    case bgfx::RendererType::Vulkan:
        Vertex = Shader(vs_quad_spirv); Fragment = Shader(fs_quad_spirv); break;
    case bgfx::RendererType::OpenGL:
        Vertex = Shader(vs_quad_glsl); Fragment = Shader(fs_quad_glsl); break;
    case bgfx::RendererType::OpenGLES:
        Vertex = Shader(vs_quad_essl); Fragment = Shader(fs_quad_essl); break;
    case bgfx::RendererType::Metal:
        Vertex = Shader(vs_quad_metal); Fragment = Shader(fs_quad_metal); break;
    case bgfx::RendererType::WebGPU:
        Vertex = Shader(vs_quad_wgsl); Fragment = Shader(fs_quad_wgsl); break;
    default: throw std::runtime_error("selected bgfx renderer has no render2d shader");
    }
    if (!bgfx::isValid(Vertex) || !bgfx::isValid(Fragment)) {
        if (bgfx::isValid(Vertex)) bgfx::destroy(Vertex);
        if (bgfx::isValid(Fragment)) bgfx::destroy(Fragment);
        throw std::runtime_error("cannot create render2d shaders");
    }
    auto Program = bgfx::createProgram(Vertex, Fragment, true);
    if (!bgfx::isValid(Program)) throw std::runtime_error("cannot link render2d shader program");
    return Program;
}

class Texture final : public Render2DTexture {
public:
    Texture(std::weak_ptr<DeviceState> Device, bgfx::TextureHandle Handle,
            std::uint16_t Width, std::uint16_t Height)
        : Device(std::move(Device)), Handle(Handle), Width(Width), Height(Height) {
        auto Owner = this->Device.lock();
        if (!Owner || !Owner->Alive) throw std::runtime_error("render device is closed");
        Owner->Textures.insert(Handle.idx);
    }
    ~Texture() override {
        if (auto Owner = Device.lock(); Owner && Owner->Alive && Owner->Textures.erase(Handle.idx))
            bgfx::destroy(Handle);
    }
    bgfx::TextureHandle GetHandle() const {
        auto Owner = Device.lock();
        if (!Owner || !Owner->Alive) throw std::runtime_error("render device is closed");
        return Handle;
    }
    std::uint16_t GetWidth() const { return Width; }
    std::uint16_t GetHeight() const { return Height; }
private:
    std::weak_ptr<DeviceState> Device;
    bgfx::TextureHandle Handle;
    std::uint16_t Width;
    std::uint16_t Height;
};

// Adopt a newly created GPU handle, including allocation-failure cleanup.
std::shared_ptr<Texture> AdoptTexture(std::weak_ptr<DeviceState> Device, bgfx::TextureHandle Handle,
                                      std::uint16_t Width, std::uint16_t Height) {
    try { return std::make_shared<Texture>(Device, Handle, Width, Height); }
    catch (...) {
        if (auto Owner = Device.lock(); Owner && Owner->Alive) bgfx::destroy(Handle);
        throw;
    }
}

struct Glyph {
    std::shared_ptr<Texture> Image;
    float XOffset = 0, YOffset = 0, Advance = 0;
};

class Font final : public Render2DFont {
public:
    Font(std::weak_ptr<DeviceState> Device, std::vector<std::uint8_t> Bytes)
        : Device(std::move(Device)), Bytes(std::move(Bytes)) {
        auto Offset = stbtt_GetFontOffsetForIndex(this->Bytes.data(), 0);
        if (Offset < 0 || !stbtt_InitFont(&Info, this->Bytes.data(), Offset))
            throw std::runtime_error("invalid TrueType/OpenType font");
    }
    Glyph& GetGlyph(std::uint32_t Codepoint, float Size) {
        auto SizeKey = static_cast<std::uint32_t>(std::lround(Size * 64.0f));
        auto Key = (std::uint64_t(SizeKey) << 32) | Codepoint;
        auto DeviceOwner = Device.lock();
        if (!DeviceOwner || !DeviceOwner->Alive) throw std::runtime_error("render device is closed");
        if (auto Found = Glyphs.find(Key); Found != Glyphs.end()) return Found->second;
        float Scale = stbtt_ScaleForPixelHeight(&Info, Size);
        int Advance = 0, Bearing = 0;
        stbtt_GetCodepointHMetrics(&Info, static_cast<int>(Codepoint), &Advance, &Bearing);
        int Width = 0, Height = 0, XOffset = 0, YOffset = 0;
        auto* Bitmap = stbtt_GetCodepointBitmap(&Info, Scale, Scale,
            static_cast<int>(Codepoint), &Width, &Height, &XOffset, &YOffset);
        Glyph Result;
        Result.XOffset = static_cast<float>(XOffset);
        Result.YOffset = static_cast<float>(YOffset);
        Result.Advance = Advance * Scale;
        if (Bitmap && Width > 0 && Height > 0 && Width <= 65535 && Height <= 65535) {
            std::vector<std::uint8_t> Pixels(static_cast<std::size_t>(Width) * Height * 4);
            for (std::size_t I = 0; I < static_cast<std::size_t>(Width) * Height; ++I) {
                Pixels[I * 4] = Pixels[I * 4 + 1] = Pixels[I * 4 + 2] = 255;
                Pixels[I * 4 + 3] = Bitmap[I];
            }
            auto Handle = bgfx::createTexture2D(static_cast<std::uint16_t>(Width),
                static_cast<std::uint16_t>(Height), false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                bgfx::copy(Pixels.data(), static_cast<std::uint32_t>(Pixels.size())));
            if (!bgfx::isValid(Handle)) {
                stbtt_FreeBitmap(Bitmap, nullptr);
                throw std::runtime_error("cannot create font glyph texture");
            }
            Result.Image = AdoptTexture(Device, Handle,
                static_cast<std::uint16_t>(Width), static_cast<std::uint16_t>(Height));
        }
        if (Bitmap) stbtt_FreeBitmap(Bitmap, nullptr);
        return Glyphs.emplace(Key, std::move(Result)).first->second;
    }
    float Baseline(float Size) const {
        int Ascent = 0, Descent = 0, Gap = 0;
        stbtt_GetFontVMetrics(&Info, &Ascent, &Descent, &Gap);
        return Ascent * stbtt_ScaleForPixelHeight(&Info, Size);
    }
    float Kerning(std::uint32_t Left, std::uint32_t Right, float Size) const {
        return stbtt_GetCodepointKernAdvance(&Info, static_cast<int>(Left),
            static_cast<int>(Right)) * stbtt_ScaleForPixelHeight(&Info, Size);
    }
private:
    std::weak_ptr<DeviceState> Device;
    std::vector<std::uint8_t> Bytes;
    stbtt_fontinfo Info{};
    std::unordered_map<std::uint64_t, Glyph> Glyphs;
};

std::vector<std::uint32_t> DecodeUtf8(std::string_view Text) {
    std::vector<std::uint32_t> Result;
    for (std::size_t I = 0; I < Text.size();) {
        auto First = static_cast<unsigned char>(Text[I++]);
        if (First < 0x80) { Result.push_back(First); continue; }
        int Extra = First < 0xe0 ? 1 : First < 0xf0 ? 2 : 3;
        std::uint32_t Codepoint = First & (Extra == 1 ? 0x1f : Extra == 2 ? 0x0f : 0x07);
        for (int N = 0; N < Extra; ++N)
            Codepoint = (Codepoint << 6) | (static_cast<unsigned char>(Text[I++]) & 0x3f);
        Result.push_back(Codepoint);
    }
    return Result;
}

class Context final : public Render2DContext {
public:
    explicit Context(const Render2DConfig& Input) : Config(Input), Device(std::make_shared<DeviceState>()) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
            throw std::runtime_error(std::string("cannot initialize SDL video: ") + SDL_GetError());
        SdlInitialized = true;
        auto Flags = Config.Resizable ? SDL_WINDOW_RESIZABLE : 0;
        Window = SDL_CreateWindow(Config.Title.c_str(), static_cast<int>(Config.WindowWidth),
                                  static_cast<int>(Config.WindowHeight), Flags);
        if (!Window) { Close(); throw std::runtime_error(std::string("cannot create window: ") + SDL_GetError()); }
        try { InitializeBgfx(); }
        catch (...) { Close(); throw; }
    }
    ~Context() override { Close(); }
    bool IsOpen() const override { return Window && WindowOpen; }
    void Close() override {
        if (Device && Device->Alive) {
            // Release images and cached font glyphs before shutting down bgfx.
            // Wrappers can outlive the context; their later destructors see Alive=false.
            for (auto Id : Device->Textures) bgfx::destroy(bgfx::TextureHandle{Id});
            Device->Textures.clear();
            if (bgfx::isValid(Device->Program)) bgfx::destroy(Device->Program);
            if (bgfx::isValid(Device->Sampler)) bgfx::destroy(Device->Sampler);
            if (bgfx::isValid(Device->White)) bgfx::destroy(Device->White);
            bgfx::frame();
            bgfx::shutdown();
            Device->Alive = false;
        }
        if (Window) { SDL_DestroyWindow(Window); Window = nullptr; }
        if (SdlInitialized) { SDL_QuitSubSystem(SDL_INIT_VIDEO); SdlInitialized = false; }
        WindowOpen = false;
        FrameOpen = false;
    }
    Render2DWindowMetrics GetWindowMetrics() const override {
        RequireOpen();
        Render2DWindowMetrics M;
        if (!SDL_GetWindowSize(Window, &M.WindowWidth, &M.WindowHeight) ||
            !SDL_GetWindowSizeInPixels(Window, &M.PixelWidth, &M.PixelHeight))
            throw std::runtime_error("cannot get window dimensions");
        M.WindowWidth = std::max(0, M.WindowWidth); M.WindowHeight = std::max(0, M.WindowHeight);
        M.PixelWidth = std::max(0, M.PixelWidth); M.PixelHeight = std::max(0, M.PixelHeight);
        M.PixelDensity = SDL_GetWindowPixelDensity(Window);
        M.DisplayScale = SDL_GetWindowDisplayScale(Window);
        M.Minimized = (SDL_GetWindowFlags(Window) & SDL_WINDOW_MINIMIZED) != 0;
        return M;
    }
    Render2DRect EffectiveViewport(const Render2DWindowMetrics& M) const {
        return Viewport.value_or(Render2DRect{0, 0, static_cast<float>(M.WindowWidth),
                                                  static_cast<float>(M.WindowHeight)});
    }
    void SetViewport(std::optional<Render2DRect> Input) override {
        RequireOpen();
        if (FrameOpen) throw std::logic_error("SetViewport must precede BeginRender");
        if (Input && (!std::isfinite(Input->X) || !std::isfinite(Input->Y) ||
            !std::isfinite(Input->Width) || !std::isfinite(Input->Height) ||
            Input->X < 0 || Input->Y < 0 || Input->Width < 0 || Input->Height < 0))
            throw std::invalid_argument("invalid viewport rectangle");
        Viewport = Input;
    }
    Render2DPoint WindowToCanvas(double X, double Y) const override {
        auto M = GetWindowMetrics();
        if (M.Minimized || M.WindowWidth <= 0 || M.WindowHeight <= 0 ||
            M.PixelWidth <= 0 || M.PixelHeight <= 0) return {};
        auto P = Render2DMapPoint(X, Y, EffectiveViewport(M), Config.RenderWidth, Config.RenderHeight);
        P.Inside = P.Inside && X >= 0 && Y >= 0 && X < M.WindowWidth && Y < M.WindowHeight;
        return P;
    }
    void BeginRender(Render2DColor Clear) override {
        RequireOpen();
        auto M = GetWindowMetrics();
        int Width = M.PixelWidth, Height = M.PixelHeight;
        auto V = EffectiveViewport(M);
        SkipFrame = M.Minimized || Width <= 0 || Height <= 0 ||
                    M.WindowWidth <= 0 || M.WindowHeight <= 0 || V.Width <= 0 || V.Height <= 0;
        if (SkipFrame) { FrameOpen = true; return; }
        if (Width != BackbufferWidth || Height != BackbufferHeight) {
            BackbufferWidth = Width; BackbufferHeight = Height;
            bgfx::SwapChain SwapChain;
            SwapChain.width = static_cast<std::uint32_t>(Width);
            SwapChain.height = static_cast<std::uint32_t>(Height);
            bgfx::reset(ResetFlags(), &SwapChain);
        }
        bgfx::setViewClear(MainView, BGFX_CLEAR_COLOR, PackClearColor(Clear));
        bgfx::setViewRect(MainView, 0, 0, static_cast<std::uint16_t>(std::min(Width, 65535)),
                          static_cast<std::uint16_t>(std::min(Height, 65535)));
        float View[16], Projection[16];
        bx::mtxIdentity(View);
        float SX = Config.RenderWidth / V.Width, SY = Config.RenderHeight / V.Height;
        bx::mtxOrtho(Projection, -V.X * SX, (M.WindowWidth - V.X) * SX,
            (M.WindowHeight - V.Y) * SY, -V.Y * SY, 0.0f, 100.0f, 0.0f,
            bgfx::getCaps()->homogeneousDepth);
        for (float Component : Projection)
            if (!std::isfinite(Component)) throw std::invalid_argument("viewport projection is not finite");
        bgfx::setViewTransform(MainView, View, Projection);
        bgfx::setViewMode(MainView, bgfx::ViewMode::Sequential);
        bgfx::touch(MainView);
        auto PX = static_cast<double>(Width) / M.WindowWidth;
        auto PY = static_cast<double>(Height) / M.WindowHeight;
        auto Edge = [](double N, int Limit) { return static_cast<std::uint16_t>(
            std::clamp(std::round(N), 0.0, static_cast<double>(std::min(Limit, 65535)))); };
        ClipX = Edge(V.X * PX, Width); ClipY = Edge(V.Y * PY, Height);
        ClipW = Edge((static_cast<double>(V.X) + V.Width) * PX, Width) - ClipX;
        ClipH = Edge((static_cast<double>(V.Y) + V.Height) * PY, Height) - ClipY;
        SkipFrame = ClipW == 0 || ClipH == 0;
        FrameOpen = true;
    }
    void EndRender() override {
        RequireOpen();
        if (!FrameOpen) throw std::logic_error("no backend render frame is open");
        bgfx::frame();
        FrameOpen = false;
    }
    std::shared_ptr<Render2DTexture> LoadTexture(std::string_view Path) override {
        RequireOpen();
        auto Bytes = ReadFile(Path);
        int Width = 0, Height = 0, Components = 0;
        auto* Pixels = stbi_load_from_memory(Bytes.data(), static_cast<int>(Bytes.size()),
                                             &Width, &Height, &Components, 4);
        if (!Pixels || Width <= 0 || Height <= 0 || Width > 65535 || Height > 65535) {
            auto Reason = stbi_failure_reason();
            throw std::runtime_error(std::string("cannot decode texture: ") +
                                     (Reason ? Reason : "unsupported image"));
        }
        auto Size = static_cast<std::size_t>(Width) * Height * 4;
        auto Handle = bgfx::createTexture2D(static_cast<std::uint16_t>(Width),
            static_cast<std::uint16_t>(Height), false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(Pixels, static_cast<std::uint32_t>(Size)));
        stbi_image_free(Pixels);
        if (!bgfx::isValid(Handle)) throw std::runtime_error("cannot create texture");
        return std::make_shared<Texture>(Device, Handle, static_cast<std::uint16_t>(Width),
                                         static_cast<std::uint16_t>(Height));
    }
    std::shared_ptr<Render2DFont> LoadFont(std::string_view Path) override {
        RequireOpen();
        return std::make_shared<Font>(Device, ReadFile(Path));
    }
    void DrawTexture(Render2DTexture& Input, Render2DRect Destination,
                     Render2DColor Color) override {
        RequireFrame();
        auto* Image = dynamic_cast<Texture*>(&Input);
        if (!Image) throw std::invalid_argument("texture belongs to another render backend");
        Submit(Image->GetHandle(), Destination, Color);
    }
    void DrawLine(float X1, float Y1, float X2, float Y2, float Width,
                  Render2DColor Color) override {
        RequireFrame();
        const auto Quad = Render2DLineQuad(X1, Y1, X2, Y2, Width);
        if (Quad) SubmitQuad(Device->White, *Quad, Color);
    }
    void DrawText(Render2DFont& Input, std::string_view Text, float X, float Y,
                  float Size, Render2DColor Color) override {
        RequireFrame();
        auto* Typeface = dynamic_cast<Font*>(&Input);
        if (!Typeface) throw std::invalid_argument("font belongs to another render backend");
        auto Codepoints = DecodeUtf8(Text);
        float StartX = X;
        float PenX = X;
        float Baseline = Y + Typeface->Baseline(Size);
        std::uint32_t Previous = 0;
        for (auto Codepoint : Codepoints) {
            if (Codepoint == '\n') { PenX = StartX; Baseline += Size; Previous = 0; continue; }
            if (Previous) PenX += Typeface->Kerning(Previous, Codepoint, Size);
            auto& Glyph = Typeface->GetGlyph(Codepoint, Size);
            if (Glyph.Image) Submit(Glyph.Image->GetHandle(),
                Render2DRect{PenX + Glyph.XOffset, Baseline + Glyph.YOffset,
                    static_cast<float>(Glyph.Image->GetWidth()),
                    static_cast<float>(Glyph.Image->GetHeight())}, Color);
            PenX += Glyph.Advance;
            Previous = Codepoint;
        }
    }
    std::optional<Render2DEvent> PollEvent() override {
        RequireOpen();
        SDL_Event Event;
        if (!SDL_PollEvent(&Event)) return std::nullopt;
        return ConvertEvent(Event);
    }
    std::optional<Render2DEvent> WaitEvent(std::int32_t TimeoutMilliseconds) override {
        RequireOpen();
        SDL_Event Event;
        if (!SDL_WaitEventTimeout(&Event, TimeoutMilliseconds)) return std::nullopt;
        return ConvertEvent(Event);
    }
private:
    std::uint32_t ResetFlags() const { return Config.VSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE; }
    void RequireOpen() const {
        if (!Window || !Device->Alive) throw std::runtime_error("render backend is closed");
    }
    void RequireFrame() const {
        RequireOpen();
        if (!FrameOpen) throw std::logic_error("no backend render frame is open");
    }
    void InitializeBgfx() {
        bgfx::Init Init;
        Init.type = bgfx::RendererType::Count;
        auto Properties = SDL_GetWindowProperties(Window);
#if defined(_WIN32)
        Init.swapChain.nwh = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
        Init.swapChain.nwh = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
        if (SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
            Init.swapChain.ndt = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            Init.swapChain.nwh = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
            Init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        } else {
            Init.swapChain.ndt = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            Init.swapChain.nwh = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                SDL_GetNumberProperty(Properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
        }
#endif
        if (!Init.swapChain.nwh) throw std::runtime_error("SDL did not expose a native window handle");
        int Width = 0, Height = 0;
        if (!SDL_GetWindowSizeInPixels(Window, &Width, &Height))
            throw std::runtime_error("cannot get initial drawable size");
        Init.swapChain.width = static_cast<std::uint32_t>(Width);
        Init.swapChain.height = static_cast<std::uint32_t>(Height);
        Init.reset = ResetFlags();
        if (!bgfx::init(Init)) throw std::runtime_error("cannot initialize bgfx");
        Device->Alive = true;
        BackbufferWidth = Width; BackbufferHeight = Height;
        Device->Layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        Device->Program = CreateProgram(bgfx::getRendererType());
        Device->Sampler = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
        const std::uint32_t White = 0xffffffffu;
        Device->White = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bgfx::copy(&White, sizeof(White)));
        if (!bgfx::isValid(Device->Sampler) || !bgfx::isValid(Device->White))
            throw std::runtime_error("cannot create render2d built-in resources");
    }
    void Submit(bgfx::TextureHandle Texture, Render2DRect Rect, Render2DColor Color) {
        if (SkipFrame) return;
        if (Rect.Width < 0 || Rect.Height < 0)
            throw std::invalid_argument("draw width and height must be non-negative");
        if (Rect.Width == 0 || Rect.Height == 0) return;
        SubmitQuad(Texture, {{{Rect.X, Rect.Y}, {Rect.X + Rect.Width, Rect.Y},
            {Rect.X + Rect.Width, Rect.Y + Rect.Height}, {Rect.X, Rect.Y + Rect.Height}}}, Color);
    }
    void SubmitQuad(bgfx::TextureHandle Texture, const std::array<Render2DPoint, 4>& Quad,
                    Render2DColor Color) {
        if (SkipFrame) return;
        if (bgfx::getAvailTransientVertexBuffer(4, Device->Layout) < 4 ||
            bgfx::getAvailTransientIndexBuffer(6) < 6)
            throw std::runtime_error("render2d transient buffer exhausted");
        bgfx::TransientVertexBuffer Vertices;
        bgfx::TransientIndexBuffer Indices;
        bgfx::allocTransientVertexBuffer(&Vertices, 4, Device->Layout);
        bgfx::allocTransientIndexBuffer(&Indices, 6);
        auto Packed = PackColor(Color);
        auto* V = reinterpret_cast<Vertex*>(Vertices.data);
        V[0] = {float(Quad[0].X), float(Quad[0].Y), 0, Packed, 0, 0};
        V[1] = {float(Quad[1].X), float(Quad[1].Y), 0, Packed, 1, 0};
        V[2] = {float(Quad[2].X), float(Quad[2].Y), 0, Packed, 1, 1};
        V[3] = {float(Quad[3].X), float(Quad[3].Y), 0, Packed, 0, 1};
        auto* I = reinterpret_cast<std::uint16_t*>(Indices.data);
        const std::uint16_t Values[] = {0, 1, 2, 0, 2, 3};
        std::copy(std::begin(Values), std::end(Values), I);
        bgfx::setVertexBuffer(0, &Vertices);
        bgfx::setIndexBuffer(&Indices);
        bgfx::setTexture(0, Device->Sampler, Texture);
        bgfx::setScissor(ClipX, ClipY, ClipW, ClipH);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(MainView, Device->Program);
    }
    std::optional<Render2DEvent> ConvertEvent(const SDL_Event& Event) {
        switch (Event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            WindowOpen = false;
            return Render2DEvent{"Close", 0, 0, {}};
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION: {
            float X = Event.type == SDL_EVENT_MOUSE_MOTION ? Event.motion.x : Event.button.x;
            float Y = Event.type == SDL_EVENT_MOUSE_MOTION ? Event.motion.y : Event.button.y;
            auto Point = WindowToCanvas(X, Y);
            auto Type = Event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "PointerDown" :
                        Event.type == SDL_EVENT_MOUSE_BUTTON_UP ? "PointerUp" : "PointerMove";
            Render2DEvent Result{Type, Point.X, Point.Y, {}};
            Result.WindowX = X; Result.WindowY = Y; Result.InsideViewport = Point.Inside;
            return Result;
        }
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED: {
            const char* Type = Event.type == SDL_EVENT_WINDOW_RESIZED ? "WindowResized" :
                Event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ? "PixelSizeChanged" :
                Event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ? "DisplayScaleChanged" :
                Event.type == SDL_EVENT_WINDOW_MINIMIZED ? "Minimized" : "Restored";
            Render2DEvent Result{Type};
            Result.Metrics = GetWindowMetrics();
            return Result;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            return Render2DEvent{Event.type == SDL_EVENT_KEY_DOWN ? "KeyDown" : "KeyUp",
                0, 0, SDL_GetKeyName(Event.key.key)};
        default: return Render2DEvent{"Other", 0, 0, {}};
        }
    }

    Render2DConfig Config;
    std::shared_ptr<DeviceState> Device;
    SDL_Window* Window = nullptr;
    bool SdlInitialized = false;
    bool WindowOpen = true;
    bool FrameOpen = false;
    bool SkipFrame = false;
    std::optional<Render2DRect> Viewport;
    std::uint16_t ClipX = 0, ClipY = 0, ClipW = 0, ClipH = 0;
    int BackbufferWidth = 0;
    int BackbufferHeight = 0;
};

class Host final : public Render2DHost {
public:
    std::shared_ptr<Render2DContext> Create(const Render2DConfig& Config) override {
        return std::make_shared<Context>(Config);
    }
};

} // namespace

std::shared_ptr<Render2DHost> CreateBgfxRender2DHost() {
    return std::make_shared<Host>();
}

} // namespace Feather
