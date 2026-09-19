#include "Render2D.hpp"
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
    void DrawText(Render2DFont&, std::string_view, float, float, float,
                  Render2DColor) override { ++Texts; }
    std::optional<Render2DEvent> PollEvent() override {
        return Render2DEvent{"PointerDown", 12, 34, {}};
    }
    std::optional<Render2DEvent> WaitEvent(std::int32_t) override { return std::nullopt; }
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

constexpr NativeModuleGuid CheckpointGuid{{
    0xba, 0x77, 0x8d, 0x2a, 0xf0, 0x32, 0x4e, 0x54,
    0x9c, 0x41, 0x66, 0xe1, 0x93, 0x0d, 0x7f, 0xb8}};

class Checkpoint final : public NativeObject {
public:
    Checkpoint(NativeObjectType& Type, Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObject(Type), Machine(Machine), Saved(Saved) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override {
        try { Saved = Machine.CaptureSnapshot(); }
        catch (const std::exception& Failure) {
            return Value::FromObject(std::make_shared<ErrorObject>(Failure.what()));
        }
        return Value::Bool(false);
    }
private:
    Vm& Machine;
    std::vector<std::uint8_t>& Saved;
};

class CheckpointType final : public NativeObjectType {
public:
    CheckpointType(Vm& Machine, std::vector<std::uint8_t>& Saved)
        : NativeObjectType(Machine, CheckpointGuid, "Checkpoint"), Saved(Saved) {}
    std::shared_ptr<Checkpoint> Create() {
        return CreateObject<Checkpoint>(GetVm(), Saved);
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || !Payload.empty())
            throw std::invalid_argument("invalid test checkpoint");
        return Create();
    }
private:
    std::vector<std::uint8_t>& Saved;
};

Value Member(const Value& Object, const char* Name) {
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

        auto TextureValue = Call(ModuleValue, "LoadTexture",
            {ContextValue, Value::String("assets/../assets/picture.png")});
        auto FontValue = Call(ModuleValue, "LoadFont",
            {ContextValue, Value::String("assets/font.ttf")});
        Check(!TextureValue.IsError() && !FontValue.IsError(),
              "resource objects were not created");
        Check(HostBackend->Last->TextureLoads == 0, "texture was not lazily loaded");

        Check(Call(ModuleValue, "DrawTexture", {ContextValue, TextureValue,
            Value::Number(0), Value::Number(0), Value::Number(10), Value::Number(10),
            Value::Number(1), Value::Number(1), Value::Number(1), Value::Number(1)}).IsError(),
            "draw outside a frame was accepted");
        Check(!Call(ModuleValue, "BeginRender", {ContextValue, Value::Number(0), Value::Number(0),
            Value::Number(0), Value::Number(1)}).IsError(), "begin failed");
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
        Check(Call(ModuleValue, "WaitEvent", {ContextValue, Value::Number(1)}).GetType() == ValueType::Null,
              "empty event wait should return null");
        Check(Call(ModuleValue, "Close", {ContextValue}).AsBool() &&
              !Call(ModuleValue, "IsOpen", {ContextValue}).AsBool(), "close failed");
        Check(Call(ModuleValue, "Close", {ContextValue}).AsBool(), "close was not idempotent");

        auto SnapshotProgram = Compile(
            "var render = import(\"render2d\"); var context = null; "
            "var texture = null; var font = null; "
            "def main() { var config = object(); config.Title = \"Snapshot\"; "
            "config.WindowWidth = 640; config.WindowHeight = 360; "
            "context = render.CreateRenderContext(config); "
            "texture = render.LoadTexture(context, \"assets/picture.png\"); "
            "font = render.LoadFont(context, \"assets/font.ttf\"); "
            "render.BeginRender(context, 0, 0, 0, 1); render.EndRender(context); "
            "if (checkpoint()) { render.BeginRender(context, 0, 0, 0, 1); "
            "render.DrawTexture(context, texture, 0, 0, 10, 10, 1, 1, 1, 1); "
            "render.DrawText(context, font, \"restored\", 1, 2, 24, 1, 1, 1, 1); "
            "render.EndRender(context); return render.IsOpen(context); } return false; }");
        std::vector<std::uint8_t> Saved;
        {
            Vm Original(SnapshotProgram.Program);
            auto OriginalHost = std::make_shared<Host>();
            Render2DLibrary OriginalLibrary(Original, OriginalHost);
            RegisterImport(Original, [&](std::string_view Name) {
                return Name == "render2d" ? OriginalLibrary.GetModule() : Value{};
            });
            auto& Checkpoints = Original.CreateNativeObjectType<CheckpointType>(Saved);
            Original.RegisterNativeFunction("checkpoint", Checkpoints.Create());
            Check(!SnapshotProgram.Initialize(Original).IsError(),
                  "render2d snapshot initializer failed");
            Check(!Original.Run(SnapshotProgram.Functions.at("main")).AsBool() && !Saved.empty(),
                  "render2d snapshot save path failed");
            Check(OriginalHost->Creates == 1, "original context was not created once");
        }
        {
            Vm Restored(SnapshotProgram.Program);
            auto RestoredHost = std::make_shared<Host>();
            Render2DLibrary RestoredLibrary(Restored, RestoredHost);
            RegisterImport(Restored, [&](std::string_view Name) {
                return Name == "render2d" ? RestoredLibrary.GetModule() : Value{};
            });
            Restored.CreateNativeObjectType<CheckpointType>(Saved);
            auto Result = Restored.ResumeSnapshot(Saved);
            Check(Result.GetType() == ValueType::Bool && Result.AsBool(),
                  "render2d snapshot did not resume after the checkpoint");
            Check(RestoredHost->Creates == 1 && RestoredHost->Config.Title == "Snapshot",
                  "restored context was not recreated from its config");
            Check(RestoredHost->Last->TextureLoads == 1 && RestoredHost->Last->FontLoads == 1 &&
                  RestoredHost->Last->Textures == 1 && RestoredHost->Last->Texts == 1,
                  "restored resources were not lazily reloaded");
        }

        auto OpenFrameProgram = Compile(
            "var render = import(\"render2d\"); "
            "def main() { var config = object(); var context = render.CreateRenderContext(config); "
            "render.BeginRender(context, 0, 0, 0, 1); return checkpoint(); }");
        std::vector<std::uint8_t> RejectedSnapshot;
        Vm OpenFrame(OpenFrameProgram.Program);
        auto OpenFrameHost = std::make_shared<Host>();
        Render2DLibrary OpenFrameLibrary(OpenFrame, OpenFrameHost);
        RegisterImport(OpenFrame, [&](std::string_view Name) {
            return Name == "render2d" ? OpenFrameLibrary.GetModule() : Value{};
        });
        auto& OpenFrameCheckpoints = OpenFrame.CreateNativeObjectType<CheckpointType>(RejectedSnapshot);
        OpenFrame.RegisterNativeFunction("checkpoint", OpenFrameCheckpoints.Create());
        Check(!OpenFrameProgram.Initialize(OpenFrame).IsError(),
              "open-frame snapshot initializer failed");
        Check(OpenFrame.Run(OpenFrameProgram.Functions.at("main")).IsError() &&
              RejectedSnapshot.empty(), "snapshot during an open frame was accepted");

        std::cout << "Render2D tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << Failure.what() << '\n';
        return 1;
    }
}
