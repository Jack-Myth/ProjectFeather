#include "Render2D.hpp"
#include "LineGeometry.hpp"
#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>

#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

struct Texture final : Render2DTexture {};
struct Font final : Render2DFont {};

struct Context final : Render2DContext {
    bool IsOpen() const override { return Open; }
    Render2DWindowMetrics GetWindowMetrics() const override { return {800, 600, 1600, 1200, 2, 2, false}; }
    void SetViewport(std::optional<Render2DRect> V) override { View = V; }
    Render2DPoint WindowToCanvas(double X, double Y) const override {
        return Render2DMapPoint(X, Y, View.value_or(Render2DRect{0,0,800,600}), 1600, 900);
    }
    std::optional<Render2DRect> View;
    void Close() override { Open = false; Frame = false; }
    void BeginRender(Render2DColor) override {
        if (!Open || Frame) throw std::logic_error("bad begin");
        Frame = true; ++Begins;
    }
    void EndRender() override {
        if (!Frame) throw std::logic_error("bad end");
        Frame = false; ++Ends;
    }
    std::shared_ptr<Render2DTexture> LoadTexture(std::string_view) override {
        ++TextureLoads; return std::make_shared<Texture>();
    }
    std::shared_ptr<Render2DFont> LoadFont(std::string_view) override {
        ++FontLoads; return std::make_shared<Font>();
    }
    void DrawTexture(Render2DTexture&, Render2DRect, Render2DColor) override { ++Textures; }
    void DrawLine(float X1, float Y1, float X2, float Y2, float Width, Render2DColor C) override {
        ++Lines; LastLine = {X1, Y1, X2, Y2, Width}; LineColor = C;
    }
    std::array<float, 5> LastLine{};
    Render2DColor LineColor;
    int Lines = 0;
    void DrawText(Render2DFont&, std::string_view, float, float, float,
                  Render2DColor) override { ++Texts; }
    Render2DTextMetrics MeasureText(Render2DFont&, std::string_view Text, float Size) override {
        return {Text.size() * Size / 2, Size, Size * 0.8, Size};
    }
    void FillRect(Render2DRect R, Render2DColor) override { ++Fills; LastRect = R; }
    void SetClipRect(std::optional<Render2DRect> R) override { Clip = R; }
    void CaptureMouse(bool Enabled) override { Captured = Enabled; }
    int Fills = 0;
    Render2DRect LastRect;
    std::optional<Render2DRect> Clip;
    bool Captured = false;
    std::optional<Render2DEvent> PollEvent() override {
        if (NextEvent) { auto E = NextEvent; NextEvent.reset(); return E; }
        return Render2DEvent{"PointerDown", 12, 34, {}};
    }
    std::optional<Render2DEvent> WaitEvent(std::int32_t) override { return std::nullopt; }
    std::optional<Render2DEvent> NextEvent;
    bool Open = true;
    bool Frame = false;
    int Begins = 0, Ends = 0, TextureLoads = 0, FontLoads = 0;
    int Textures = 0, Texts = 0;
};

struct Host final : Render2DHost {
    std::shared_ptr<Render2DContext> Create(const Render2DConfig& Input) override {
        ++Creates;
        Config = Input;
        Last = std::make_shared<Context>();
        return Last;
    }
    int Creates = 0;
    Render2DConfig Config;
    std::shared_ptr<Context> Last;
};

Value Member(const Value& Object, const char* Name) {
    if (Object.IsScriptObject()) return Object.AsScriptObject()->GetRaw(Value::String(Name)).value_or(Value{});
    return dynamic_cast<NativeObject*>(Object.AsObject())->GetMember(Value::String(Name));
}

Value Call(const Value& Module, const char* Name, std::vector<Value> Arguments) {
    auto Function = Member(Module, Name);
    Check(!Function.IsError() && Function.AsObject()->GetObjectType() == ObjectType::Host,
          "render2d function missing");
    return dynamic_cast<NativeObject*>(Function.AsObject())->Call(Arguments);
}

void Set(ScriptObject* Object, const char* Name, Value Input) {
    auto Result = Object->SetRaw(Value::String(Name), Input);
    Check(!Result.IsError(), "cannot set config property");
}
}

int main() {
    try {
        auto Program = Compile("def main() { return null; }");
        Vm Machine(Program.Program);
        auto HostBackend = std::make_shared<Host>();
        Render2DLibrary Library(Machine, HostBackend);
        auto ModuleValue = Library.GetModule();

        auto* Config = Machine.CreateScriptObject();
        Set(Config, "Title", Value::String("Render test"));
        Set(Config, "WindowWidth", Value::Number(800));
        Set(Config, "WindowHeight", Value::Number(450));
        Set(Config, "RenderWidth", Value::Number(1600));
        Set(Config, "RenderHeight", Value::Number(900));
        auto ContextValue = Call(ModuleValue, "CreateRenderContext", {Value::FromScript(Config)});
        Check(!ContextValue.IsError() && HostBackend->Config.Title == "Render test" &&
              HostBackend->Config.WindowWidth == 800 && HostBackend->Config.RenderWidth == 1600,
              "context config was not passed to backend");
        Check(Call(ModuleValue, "CreateRenderContext", {Value::FromScript(Config)}).IsError(),
              "second active context was accepted");

        auto Metrics = Call(ModuleValue, "GetWindowMetrics", {ContextValue});
        Check(Member(Metrics, "WindowWidth").AsNumber() == 800 &&
              Member(Metrics, "PixelWidth").AsNumber() == 1600 &&
              Member(Metrics, "DisplayScale").AsNumber() == 2, "window units lost");
        auto SetView = [&](double X, double Y, double W, double H) {
            return Call(ModuleValue, "SetViewport", {ContextValue, Value::Number(X),
                Value::Number(Y), Value::Number(W), Value::Number(H)});
        };
        Check(!SetView(0, 75, 800, 450).IsError(), "viewport setup");
        auto Point = Call(ModuleValue, "WindowToCanvas", {ContextValue, Value::Number(400), Value::Number(300)});
        Check(Member(Point, "X").AsNumber() == 800 && Member(Point, "Y").AsNumber() == 450 &&
              Member(Point, "InsideViewport").AsBool(), "viewport inverse mapping");
        Point = Call(ModuleValue, "WindowToCanvas", {ContextValue, Value::Number(400), Value::Number(50)});
        Check(!Member(Point, "InsideViewport").AsBool(), "letterbox accepted pointer");
        Check(SetView(0, 0, -1, 10).IsError(), "negative viewport accepted");
        Check(!SetView(0, 0, 0, 0).IsError(), "zero viewport rejected");
        Point = Call(ModuleValue, "WindowToCanvas", {ContextValue, Value::Number(0), Value::Number(0)});
        Check(!Member(Point, "InsideViewport").AsBool() && Member(Point, "X").AsNumber() == 0,
              "zero viewport divided by zero");
        Check(!Call(ModuleValue, "SetViewport", {ContextValue, Value{}}).IsError(), "reset viewport");
        Point = Call(ModuleValue, "WindowToCanvas", {ContextValue, Value::Number(400), Value::Number(300)});
        Check(Member(Point, "X").AsNumber() == 800 && Member(Point, "Y").AsNumber() == 450,
              "default stretch changed");
        for (auto Type : {"WindowResized", "PixelSizeChanged", "DisplayScaleChanged", "Minimized", "Restored"}) {
            Render2DEvent E{Type}; E.Metrics = HostBackend->Last->GetWindowMetrics();
            HostBackend->Last->NextEvent = E;
            auto Event = Call(ModuleValue, "PollEvent", {ContextValue});
            Check(Member(Event, "Type").AsString() == Type &&
                  Member(Member(Event, "Metrics"), "PixelHeight").AsNumber() == 1200,
                  "window event metrics lost");
        }

        // Geometry uses flat-ended quads, including stable diagonals and degenerate lines.
        const auto H = *Render2DLineQuad(0, 0, 10, 0, 4);
        Check(H[0].X == 0 && H[0].Y == 2 && H[2].X == 10 && H[2].Y == -2, "horizontal quad");
        const auto V = *Render2DLineQuad(0, 0, 0, 10, 4);
        Check(V[0].X == -2 && V[0].Y == 0 && V[2].X == 2 && V[2].Y == 10, "vertical quad");
        const auto D = *Render2DLineQuad(0, 0, 3, 4, 10);
        Check(std::abs(D[0].X + 4) < 1e-9 && std::abs(D[0].Y - 3) < 1e-9, "diagonal perpendicular width");
        Check(!Render2DLineQuad(2, 3, 2, 3, 1), "zero-length line must draw nothing");
        const auto Huge = *Render2DLineQuad(-3e38f, 0, 3e38f, 0, 1);
        Check(std::isfinite(Huge[0].Y), "large endpoint difference overflowed");
        bool Rejected = false;
        try { (void)Render2DLineQuad(0, 0, 1, 1, 0); } catch (const std::invalid_argument&) { Rejected = true; }
        Check(Rejected, "zero width accepted");
        Rejected = false;
        try { (void)Render2DLineQuad(std::numeric_limits<float>::max(), 0,
                                   std::numeric_limits<float>::max(), 1,
                                   std::numeric_limits<float>::max()); }
        catch (const std::invalid_argument&) { Rejected = true; }
        Check(Rejected, "unrepresentable quad accepted");
        std::vector<Value> LineArgs{ContextValue, Value::Number(1), Value::Number(2),
            Value::Number(31), Value::Number(42), Value::Number(3.5),
            Value::Number(1), Value::Number(0.25), Value::Number(0), Value::Number(0.5)};
        Check(Call(ModuleValue, "DrawLine", LineArgs).IsError(), "line outside frame accepted");

        auto TextureValue = Call(ModuleValue, "LoadTexture",
            {ContextValue, Value::String("assets/../assets/picture.png")});
        auto FontValue = Call(ModuleValue, "LoadFont",
            {ContextValue, Value::String("assets/font.ttf")});
        Check(!TextureValue.IsError() && !FontValue.IsError(),
              "resource objects were not created");
        auto Measurement = Call(ModuleValue, "MeasureText", {ContextValue, FontValue,
            Value::String("hello"), Value::Number(24)});
        Check(Member(Measurement, "Width").AsNumber() == 60 &&
              std::abs(Member(Measurement, "Baseline").AsNumber() - 19.2) < 1e-9,
              "text metrics were not forwarded");
        Check(Call(ModuleValue, "MeasureText", {ContextValue, FontValue,
            Value::String("hello"), Value::Number(0)}).IsError(), "zero font size accepted");
        Check(Call(ModuleValue, "SetClipRect", {ContextValue, Value{}}).IsError(),
              "clip outside frame accepted");
        Check(Call(ModuleValue, "CaptureMouse", {ContextValue, Value::Bool(true)}).AsBool() &&
              HostBackend->Last->Captured, "capture not forwarded");
        Check(Call(ModuleValue, "CaptureMouse", {ContextValue, Value::Bool(false)}).AsBool() &&
              !HostBackend->Last->Captured, "capture release not forwarded");
        Check(Call(ModuleValue, "CaptureMouse", {ContextValue, Value::Number(1)}).IsError(),
              "non-bool capture accepted");
        Check(HostBackend->Last->TextureLoads == 0, "texture was not lazily loaded");

        Check(Call(ModuleValue, "DrawTexture", {ContextValue, TextureValue,
            Value::Number(0), Value::Number(0), Value::Number(10), Value::Number(10),
            Value::Number(1), Value::Number(1), Value::Number(1), Value::Number(1)}).IsError(),
            "draw outside a frame was accepted");
        Check(!Call(ModuleValue, "BeginRender", {ContextValue, Value::Number(0), Value::Number(0),
            Value::Number(0), Value::Number(1)}).IsError(), "begin failed");
        Check(Call(ModuleValue, "DrawLine", LineArgs).AsBool(), "DrawLine failed");
        Check(Call(ModuleValue, "SetClipRect", {ContextValue, Value::Number(-5), Value::Number(7),
            Value::Number(30), Value::Number(40)}).AsBool() &&
            HostBackend->Last->Clip->X == -5, "clip not forwarded");
        Check(Call(ModuleValue, "SetClipRect", {ContextValue, Value::Number(0), Value::Number(0),
            Value::Number(-1), Value::Number(40)}).IsError(), "negative clip accepted");
        Check(Call(ModuleValue, "FillRect", {ContextValue, Value::Number(1), Value::Number(2),
            Value::Number(3), Value::Number(4), Value::Number(1), Value::Number(0),
            Value::Number(0), Value::Number(1)}).AsBool() && HostBackend->Last->Fills == 1 &&
            HostBackend->Last->LastRect.Height == 4, "fill not forwarded");
        Check(Call(ModuleValue, "SetClipRect", {ContextValue, Value{}}).AsBool() &&
            !HostBackend->Last->Clip, "clip reset failed");
        Check(HostBackend->Last->Lines == 1 && HostBackend->Last->LastLine ==
              std::array<float,5>{1,2,31,42,3.5f} && HostBackend->Last->LineColor.Alpha == 0.5f,
              "line geometry/color not passed to backend");
        Check(Call(ModuleValue, "DrawLine", {ContextValue}).IsError(), "bad line arity");
        for (auto Bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
            auto Args = LineArgs; Args[5] = Value::Number(Bad);
            Check(Call(ModuleValue, "DrawLine", Args).IsError(), "bad line width accepted");
        }
        for (int At : {1,2,3,4,6,7,8,9}) {
            auto Args = LineArgs; Args[At] = Value::Number(std::numeric_limits<double>::infinity());
            Check(Call(ModuleValue, "DrawLine", Args).IsError(), "nonfinite line parameter accepted");
        }
        auto BadColor = LineArgs; BadColor[9] = Value::Number(1.1);
        Check(Call(ModuleValue, "DrawLine", BadColor).IsError(), "invalid alpha accepted");
        auto BadType = LineArgs; BadType[1] = Value::String("1");
        Check(Call(ModuleValue, "DrawLine", BadType).IsError(), "string coordinate accepted");
        Check(Call(ModuleValue, "SetViewport", {ContextValue, Value{}}).IsError(),
              "viewport changed inside frame");
        Check(!Call(ModuleValue, "DrawTexture", {ContextValue, TextureValue,
            Value::Number(0), Value::Number(0), Value::Number(10), Value::Number(10),
            Value::Number(1), Value::Number(1), Value::Number(1), Value::Number(1)}).IsError(),
            "texture draw failed");
        Check(!Call(ModuleValue, "DrawText", {ContextValue, FontValue, Value::String("hello"),
            Value::Number(1), Value::Number(2), Value::Number(24), Value::Number(1),
            Value::Number(1), Value::Number(1), Value::Number(1)}).IsError(), "text draw failed");
        Check(!Call(ModuleValue, "EndRender", {ContextValue}).IsError(), "end failed");
        Check(HostBackend->Last->TextureLoads == 1 && HostBackend->Last->FontLoads == 1 &&
              HostBackend->Last->Textures == 1 && HostBackend->Last->Texts == 1,
              "backend did not receive render commands");

        auto Event = Call(ModuleValue, "PollEvent", {ContextValue});
        Check(Event.IsScriptObject() &&
              Event.AsScriptObject()->GetRaw(Value::String("Type"))->AsString() == "PointerDown" &&
              Event.AsScriptObject()->GetRaw(Value::String("X"))->AsNumber() == 12,
              "event was not converted to a script object");
        Render2DEvent RightClick{"PointerDown", 1, 2, {}}; RightClick.Button = 3;
        HostBackend->Last->NextEvent = RightClick;
        Check(Member(Call(ModuleValue, "PollEvent", {ContextValue}), "Button").AsNumber() == 3,
              "pointer button was lost");
        Check(Call(ModuleValue, "WaitEvent", {ContextValue, Value::Number(1)}).GetType() == ValueType::Null,
              "empty event wait should return null");
        Check(Call(ModuleValue, "Close", {ContextValue}).AsBool() &&
              !Call(ModuleValue, "IsOpen", {ContextValue}).AsBool(), "close failed");
        Check(Call(ModuleValue, "Close", {ContextValue}).AsBool(), "close was not idempotent");

        std::cout << "Render2D tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << Failure.what() << '\n';
        return 1;
    }
}
