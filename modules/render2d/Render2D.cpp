#include "Render2D.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Feather {
namespace {

Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}

using Method = std::function<Value(const std::vector<Value>&)>;

class Function final : public NativeObject {
public:
    Function(NativeObjectType& Type, Method Body)
        : NativeObject(Type), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        try { return Body(Arguments); }
        catch (const std::exception& Failure) { return Error(Failure.what()); }
    }
private:
    Method Body;
};

class Namespace final : public NativeObject {
public:
    explicit Namespace(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    void Add(std::string Name, Value Input) {
        Members.emplace(std::move(Name), std::move(Input));
    }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String) return Error("member name must be a string");
        auto Found = Members.find(Key.AsString());
        return Found == Members.end() ? Error("unknown render2d member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};

struct LibraryState {
    LibraryState(Vm& InputMachine, std::shared_ptr<Render2DHost> InputHost)
        : Machine(InputMachine), Host(std::move(InputHost)) {}
    Vm& Machine;
    std::shared_ptr<Render2DHost> Host;
    std::uint64_t NextContextId = 1;
    std::uint64_t ActiveContextId = 0;
};

void AppendU32(std::vector<std::uint8_t>& Output, std::uint32_t Value) {
    for (int Shift = 0; Shift < 32; Shift += 8)
        Output.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

void AppendU64(std::vector<std::uint8_t>& Output, std::uint64_t Value) {
    for (int Shift = 0; Shift < 64; Shift += 8)
        Output.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> Input, std::size_t& At) {
    if (At > Input.size() || Input.size() - At < 4)
        throw std::invalid_argument("truncated render2d payload");
    std::uint32_t Result = 0;
    for (int Shift = 0; Shift < 32; Shift += 8) Result |= std::uint32_t(Input[At++]) << Shift;
    return Result;
}

std::uint64_t ReadU64(std::span<const std::uint8_t> Input, std::size_t& At) {
    if (At > Input.size() || Input.size() - At < 8)
        throw std::invalid_argument("truncated render2d payload");
    std::uint64_t Result = 0;
    for (int Shift = 0; Shift < 64; Shift += 8) Result |= std::uint64_t(Input[At++]) << Shift;
    return Result;
}

void AppendString(std::vector<std::uint8_t>& Output, std::string_view Value) {
    if (Value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("render2d string is too large");
    AppendU32(Output, static_cast<std::uint32_t>(Value.size()));
    Output.insert(Output.end(), Value.begin(), Value.end());
}

std::string ReadString(std::span<const std::uint8_t> Input, std::size_t& At) {
    auto Size = ReadU32(Input, At);
    if (At > Input.size() || Input.size() - At < Size)
        throw std::invalid_argument("truncated render2d string");
    std::string Result(reinterpret_cast<const char*>(Input.data() + At), Size);
    At += Size;
    (void)Value::String(Result);
    return Result;
}

bool ExactInteger(const Value& Input, std::uint32_t& Output, std::uint32_t Minimum,
                  std::uint32_t Maximum) {
    if (Input.GetType() != ValueType::Number) return false;
    auto Number = Input.AsNumber();
    if (!std::isfinite(Number) || std::trunc(Number) != Number ||
        Number < Minimum || Number > Maximum) return false;
    Output = static_cast<std::uint32_t>(Number);
    return true;
}

float FiniteFloat(const Value& Input, const char* Name) {
    if (Input.GetType() != ValueType::Number || !std::isfinite(Input.AsNumber()) ||
        Input.AsNumber() < -std::numeric_limits<float>::max() ||
        Input.AsNumber() > std::numeric_limits<float>::max())
        throw std::invalid_argument(std::string(Name) + " must be a finite number");
    return static_cast<float>(Input.AsNumber());
}

Render2DColor Color(const std::vector<Value>& A, std::size_t At) {
    Render2DColor Result{FiniteFloat(A[At], "red"), FiniteFloat(A[At + 1], "green"),
                         FiniteFloat(A[At + 2], "blue"), FiniteFloat(A[At + 3], "alpha")};
    if (Result.Red < 0 || Result.Red > 1 || Result.Green < 0 || Result.Green > 1 ||
        Result.Blue < 0 || Result.Blue > 1 || Result.Alpha < 0 || Result.Alpha > 1)
        throw std::invalid_argument("color components must be between 0 and 1");
    return Result;
}

std::optional<Value> Property(const ScriptObject& Object, std::string_view Name) {
    return Object.GetRaw(Value::String(std::string(Name)));
}

Render2DConfig Config(const Value& Input) {
    if (!Input.IsScriptObject()) throw std::invalid_argument("CreateRenderContext expects a config object");
    auto& Object = *Input.AsScriptObject();
    Render2DConfig Result;
    if (auto Value = Property(Object, "Title")) {
        if (Value->GetType() != ValueType::String) throw std::invalid_argument("Title must be a string");
        Result.Title = Value->AsString();
    }
    auto ReadDimension = [&](const char* Name, std::uint32_t& Target) {
        if (auto Value = Property(Object, Name))
            if (!ExactInteger(*Value, Target, 1, 16384))
                throw std::invalid_argument(std::string(Name) + " must be an integer from 1 to 16384");
    };
    ReadDimension("WindowWidth", Result.WindowWidth);
    ReadDimension("WindowHeight", Result.WindowHeight);
    ReadDimension("RenderWidth", Result.RenderWidth);
    ReadDimension("RenderHeight", Result.RenderHeight);
    auto ReadBool = [&](const char* Name, bool& Target) {
        if (auto Value = Property(Object, Name)) {
            if (Value->GetType() != ValueType::Bool)
                throw std::invalid_argument(std::string(Name) + " must be a bool");
            Target = Value->AsBool();
        }
    };
    ReadBool("VSync", Result.VSync);
    ReadBool("Resizable", Result.Resizable);
    return Result;
}

std::string NormalizeResourcePath(std::string_view Input) {
    if (Input.empty()) throw std::invalid_argument("resource path must not be empty");
    std::u8string Utf8;
    Utf8.reserve(Input.size());
    for (unsigned char Byte : Input) Utf8.push_back(static_cast<char8_t>(Byte));
    auto Normalized = std::filesystem::path(Utf8).lexically_normal().generic_u8string();
    std::string Result;
    Result.reserve(Normalized.size());
    for (char8_t Byte : Normalized) Result.push_back(static_cast<char>(Byte));
    (void)Value::String(Result);
    return Result;
}

class ContextObject final : public NativeObject {
public:
    ContextObject(NativeObjectType& Type, std::shared_ptr<LibraryState> State,
                  std::uint64_t Id, Render2DConfig Config, bool WasOpen)
        : NativeObject(Type), State(std::move(State)), Id(Id), Options(std::move(Config)),
          ShouldOpen(WasOpen) {}
    ~ContextObject() override { CloseNoThrow(); }
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    std::uint64_t GetId() const { return Id; }
    const Render2DConfig& GetConfig() const { return Options; }
    bool IsFrameOpen() const { return FrameOpen; }
    bool IsOpen() const { return Backend && Backend->IsOpen(); }
    Render2DContext& RequireOpen() const {
        if (!IsOpen()) throw std::runtime_error("render context is closed");
        return *Backend;
    }
    void Open() {
        if (!ShouldOpen) return;
        if (State->ActiveContextId != 0 && State->ActiveContextId != Id)
            throw std::runtime_error("only one render context may be open");
        Backend = State->Host->Create(Options);
        if (!Backend || !Backend->IsOpen()) {
            Backend.reset();
            throw std::runtime_error("render backend did not create an open context");
        }
        State->ActiveContextId = Id;
    }
    void Close() {
        if (Backend) Backend->Close();
        Backend.reset();
        FrameOpen = false;
        ShouldOpen = false;
        if (State->ActiveContextId == Id) State->ActiveContextId = 0;
    }
    void Begin(Render2DColor Clear) {
        if (FrameOpen) throw std::logic_error("a render frame is already open");
        RequireOpen().BeginRender(Clear);
        FrameOpen = true;
    }
    void End() {
        if (!FrameOpen) throw std::logic_error("no render frame is open");
        RequireOpen().EndRender();
        FrameOpen = false;
    }
private:
    void CloseNoThrow() noexcept { try { Close(); } catch (...) {} }
    std::shared_ptr<LibraryState> State;
    std::uint64_t Id;
    Render2DConfig Options;
    bool ShouldOpen = true;
    bool FrameOpen = false;
    std::shared_ptr<Render2DContext> Backend;
};

class ContextType final : public NativeObjectType {
public:
    ContextType(Vm& Machine, std::shared_ptr<LibraryState> State)
        : NativeObjectType(Machine, Render2DModuleGuid, "RenderContext"), State(std::move(State)) {}
    std::shared_ptr<ContextObject> Create(Render2DConfig Config, bool OpenNow,
                                           std::uint64_t SavedId = 0, bool WasOpen = true) {
        auto Id = SavedId ? SavedId : State->NextContextId;
        if (Id == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("render context id space is exhausted");
        State->NextContextId = std::max(State->NextContextId, Id + 1);
        auto Result = CreateObject<ContextObject>(State, Id, std::move(Config), WasOpen);
        if (OpenNow) Result->Open();
        return Result;
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        auto* Context = dynamic_cast<const ContextObject*>(&Input);
        if (!Context || Context->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid render context");
        if (Context->IsFrameOpen())
            throw std::logic_error("cannot snapshot while a render frame is open");
        const auto& Config = Context->GetConfig();
        std::vector<std::uint8_t> Result;
        AppendU64(Result, Context->GetId());
        Result.push_back(Context->IsOpen() ? 1 : 0);
        Result.push_back(Config.VSync ? 1 : 0);
        Result.push_back(Config.Resizable ? 1 : 0);
        AppendU32(Result, Config.WindowWidth); AppendU32(Result, Config.WindowHeight);
        AppendU32(Result, Config.RenderWidth); AppendU32(Result, Config.RenderHeight);
        AppendString(Result, Config.Title);
        return Result;
    }
    std::shared_ptr<NativeObject> Deserialize(std::uint32_t Version,
        std::span<const std::uint8_t> Payload) override {
        if (Version != 1) throw std::invalid_argument("unsupported RenderContext version");
        std::size_t At = 0;
        auto Id = ReadU64(Payload, At);
        if (!Id || At > Payload.size() || Payload.size() - At < 3)
            throw std::invalid_argument("invalid RenderContext payload");
        if (Payload[At] > 1 || Payload[At + 1] > 1 || Payload[At + 2] > 1)
            throw std::invalid_argument("invalid RenderContext flags");
        bool Open = Payload[At++] != 0;
        Render2DConfig Config;
        Config.VSync = Payload[At++] != 0;
        Config.Resizable = Payload[At++] != 0;
        Config.WindowWidth = ReadU32(Payload, At); Config.WindowHeight = ReadU32(Payload, At);
        Config.RenderWidth = ReadU32(Payload, At); Config.RenderHeight = ReadU32(Payload, At);
        Config.Title = ReadString(Payload, At);
        if (At != Payload.size() || !Config.WindowWidth || !Config.WindowHeight ||
            !Config.RenderWidth || !Config.RenderHeight || Config.WindowWidth > 16384 ||
            Config.WindowHeight > 16384 || Config.RenderWidth > 16384 || Config.RenderHeight > 16384)
            throw std::invalid_argument("invalid RenderContext payload");
        return Create(std::move(Config), false, Id, Open);
    }
    void FinalizeRestore(NativeObject& Input) override {
        auto* Context = dynamic_cast<ContextObject*>(&Input);
        if (!Context || Context->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid restored RenderContext");
        Context->Open();
    }
private:
    std::shared_ptr<LibraryState> State;
};

template<class BackendResource>
class ResourceObject final : public NativeObject {
public:
    ResourceObject(NativeObjectType& Type, std::uint64_t ContextId, std::string Path)
        : NativeObject(Type), ContextId(ContextId), Path(std::move(Path)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    std::uint64_t GetContextId() const { return ContextId; }
    const std::string& GetPath() const { return Path; }
    std::shared_ptr<BackendResource>& GetLoaded() { return Loaded; }
private:
    std::uint64_t ContextId;
    std::string Path;
    std::shared_ptr<BackendResource> Loaded;
};

template<class BackendResource>
class ResourceType final : public NativeObjectType {
public:
    ResourceType(Vm& Machine, std::string Name)
        : NativeObjectType(Machine, Render2DModuleGuid, std::move(Name)) {}
    std::shared_ptr<ResourceObject<BackendResource>> Create(std::uint64_t ContextId,
                                                             std::string Path) {
        return CreateObject<ResourceObject<BackendResource>>(ContextId, std::move(Path));
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        auto* Resource = dynamic_cast<const ResourceObject<BackendResource>*>(&Input);
        if (!Resource || Resource->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid render2d resource");
        std::vector<std::uint8_t> Result;
        AppendU64(Result, Resource->GetContextId());
        AppendString(Result, Resource->GetPath());
        return Result;
    }
    std::shared_ptr<NativeObject> Deserialize(std::uint32_t Version,
        std::span<const std::uint8_t> Payload) override {
        if (Version != 1) throw std::invalid_argument("unsupported render2d resource version");
        std::size_t At = 0;
        auto ContextId = ReadU64(Payload, At);
        auto Path = ReadString(Payload, At);
        if (!ContextId || Path.empty() || At != Payload.size())
            throw std::invalid_argument("invalid render2d resource payload");
        return Create(ContextId, std::move(Path));
    }
};

using TextureObject = ResourceObject<Render2DTexture>;
using FontObject = ResourceObject<Render2DFont>;

ContextObject& ContextArgument(const Value& Input) {
    if (Input.GetType() != ValueType::Object || Input.IsScriptObject())
        throw std::invalid_argument("expected RenderContext");
    auto* Result = dynamic_cast<ContextObject*>(Input.AsObject());
    if (!Result) throw std::invalid_argument("expected RenderContext");
    return *Result;
}

template<class Resource>
Resource& ResourceArgument(const Value& Input, std::uint64_t ContextId, const char* Name) {
    if (Input.GetType() != ValueType::Object || Input.IsScriptObject())
        throw std::invalid_argument(std::string("expected ") + Name);
    auto* Result = dynamic_cast<Resource*>(Input.AsObject());
    if (!Result) throw std::invalid_argument(std::string("expected ") + Name);
    if (Result->GetContextId() != ContextId)
        throw std::invalid_argument(std::string(Name) + " belongs to another RenderContext");
    return *Result;
}

Value EventValue(Vm& Machine, const std::optional<Render2DEvent>& Event) {
    if (!Event) return {};
    auto* Result = Machine.CreateScriptObject();
    Result->SetRaw(Value::String("Type"), Value::String(Event->Type));
    if (!Event->Key.empty()) Result->SetRaw(Value::String("Key"), Value::String(Event->Key));
    if (Event->Type == "PointerDown" || Event->Type == "PointerUp" || Event->Type == "PointerMove") {
        Result->SetRaw(Value::String("X"), Value::Number(Event->X));
        Result->SetRaw(Value::String("Y"), Value::Number(Event->Y));
    }
    return Value::FromScript(Result);
}

} // namespace

Render2DLibrary::Render2DLibrary(Vm& Machine, std::shared_ptr<Render2DHost> Host) {
    if (!Host) throw std::invalid_argument("render2d requires a host backend");
    auto State = std::make_shared<LibraryState>(Machine, std::move(Host));
    Shared = State;
    auto& Contexts = Machine.CreateNativeObjectType<ContextType>(State);
    auto& Textures = Machine.CreateNativeObjectType<ResourceType<Render2DTexture>>("Texture");
    auto& Fonts = Machine.CreateNativeObjectType<ResourceType<Render2DFont>>("Font");
    auto& ModuleType = Machine.CreateNativeObjectType<NativeSingletonType>(
        Render2DModuleGuid, "Module");
    auto Module = ModuleType.Create<Namespace>();
    auto Add = [&](std::string Name, Method Body) {
        auto TypeName = Name;
        auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
            Render2DModuleGuid, std::move(TypeName));
        Module->Add(std::move(Name), Value::FromObject(Type.Create<Function>(std::move(Body))));
    };

    Add("CreateRenderContext", [&Contexts](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("CreateRenderContext expects one config object");
        return Value::FromObject(Contexts.Create(Config(A[0]), true));
    });
    Add("Close", [](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Close expects one RenderContext");
        ContextArgument(A[0]).Close();
        return Value::Bool(true);
    });
    Add("IsOpen", [](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("IsOpen expects one RenderContext");
        return Value::Bool(ContextArgument(A[0]).IsOpen());
    });
    Add("LoadTexture", [&Textures](const std::vector<Value>& A) {
        if (A.size() != 2 || A[1].GetType() != ValueType::String)
            return Error("LoadTexture expects RenderContext and path");
        auto& Context = ContextArgument(A[0]); Context.RequireOpen();
        return Value::FromObject(Textures.Create(Context.GetId(), NormalizeResourcePath(A[1].AsString())));
    });
    Add("LoadFont", [&Fonts](const std::vector<Value>& A) {
        if (A.size() != 2 || A[1].GetType() != ValueType::String)
            return Error("LoadFont expects RenderContext and path");
        auto& Context = ContextArgument(A[0]); Context.RequireOpen();
        return Value::FromObject(Fonts.Create(Context.GetId(), NormalizeResourcePath(A[1].AsString())));
    });
    Add("BeginRender", [](const std::vector<Value>& A) {
        if (A.size() != 5) return Error("BeginRender expects context and RGBA");
        ContextArgument(A[0]).Begin(Color(A, 1));
        return Value::Bool(true);
    });
    Add("DrawTexture", [](const std::vector<Value>& A) {
        if (A.size() != 10) return Error("DrawTexture expects context, texture, rectangle, and RGBA");
        auto& Context = ContextArgument(A[0]);
        if (!Context.IsFrameOpen()) return Error("DrawTexture requires an open render frame");
        auto& Texture = ResourceArgument<TextureObject>(A[1], Context.GetId(), "Texture");
        auto& Loaded = Texture.GetLoaded();
        if (!Loaded) Loaded = Context.RequireOpen().LoadTexture(Texture.GetPath());
        Render2DRect Rect{FiniteFloat(A[2], "x"), FiniteFloat(A[3], "y"),
                          FiniteFloat(A[4], "width"), FiniteFloat(A[5], "height")};
        Context.RequireOpen().DrawTexture(*Loaded, Rect, Color(A, 6));
        return Value::Bool(true);
    });
    Add("DrawText", [](const std::vector<Value>& A) {
        if (A.size() != 10 || A[2].GetType() != ValueType::String)
            return Error("DrawText expects context, font, text, position, size, and RGBA");
        auto& Context = ContextArgument(A[0]);
        if (!Context.IsFrameOpen()) return Error("DrawText requires an open render frame");
        auto& Font = ResourceArgument<FontObject>(A[1], Context.GetId(), "Font");
        auto& Loaded = Font.GetLoaded();
        if (!Loaded) Loaded = Context.RequireOpen().LoadFont(Font.GetPath());
        auto Size = FiniteFloat(A[5], "size");
        if (Size <= 0) return Error("text size must be positive");
        Context.RequireOpen().DrawText(*Loaded, A[2].AsString(), FiniteFloat(A[3], "x"),
            FiniteFloat(A[4], "y"), Size, Color(A, 6));
        return Value::Bool(true);
    });
    Add("EndRender", [](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("EndRender expects one RenderContext");
        ContextArgument(A[0]).End();
        return Value::Bool(true);
    });
    Add("PollEvent", [State](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("PollEvent expects one RenderContext");
        return EventValue(State->Machine, ContextArgument(A[0]).RequireOpen().PollEvent());
    });
    Add("WaitEvent", [State](const std::vector<Value>& A) {
        std::uint32_t Timeout = 0;
        if (A.size() != 2 || !ExactInteger(A[1], Timeout, 0,
                static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())))
            return Error("WaitEvent expects RenderContext and a non-negative integer timeout");
        return EventValue(State->Machine, ContextArgument(A[0]).RequireOpen().WaitEvent(
            static_cast<std::int32_t>(Timeout)));
    });
    ModuleObject = std::move(Module);
}

Render2DLibrary::~Render2DLibrary() = default;
Value Render2DLibrary::GetModule() const { return Value::FromObject(ModuleObject); }

} // namespace Feather
