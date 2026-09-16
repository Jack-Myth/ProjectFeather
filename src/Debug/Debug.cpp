#include <Feather/Debug.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Feather {
namespace {

class Json final {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    Json() : Data(nullptr) {}
    Json(std::nullptr_t) : Data(nullptr) {}
    Json(bool Input) : Data(Input) {}
    Json(double Input) : Data(Input) {}
    Json(std::uint64_t Input) : Data(static_cast<double>(Input)) {}
    Json(std::string Input) : Data(std::move(Input)) {}
    Json(const char* Input) : Data(std::string(Input)) {}
    Json(Object Input) : Data(std::move(Input)) {}
    Json(Array Input) : Data(std::move(Input)) {}

    bool IsNumber() const { return std::holds_alternative<double>(Data); }
    bool IsString() const { return std::holds_alternative<std::string>(Data); }
    bool IsObject() const { return std::holds_alternative<Object>(Data); }
    double Number() const { return std::get<double>(Data); }
    const std::string& String() const { return std::get<std::string>(Data); }
    const Object& Members() const { return std::get<Object>(Data); }
    const Storage& Get() const { return Data; }
private:
    Storage Data;
};

void AppendUtf8(std::string& Output, std::uint32_t Point) {
    if (Point <= 0x7f) Output.push_back(static_cast<char>(Point));
    else if (Point <= 0x7ff) {
        Output.push_back(static_cast<char>(0xc0 | (Point >> 6)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    } else if (Point <= 0xffff) {
        Output.push_back(static_cast<char>(0xe0 | (Point >> 12)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 6) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    } else {
        Output.push_back(static_cast<char>(0xf0 | (Point >> 18)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 12) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 6) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    }
}

class JsonParser final {
public:
    explicit JsonParser(std::string_view Input) : Input(Input) {}
    Json Parse() {
        auto Result = ParseValue(0);
        Space();
        if (Position != Input.size()) Fail();
        return Result;
    }
private:
    [[noreturn]] void Fail() const { throw std::invalid_argument("invalid JSON protocol message"); }
    void Space() {
        while (Position < Input.size() &&
               (Input[Position] == ' ' || Input[Position] == '\t' ||
                Input[Position] == '\r' || Input[Position] == '\n')) ++Position;
    }
    bool Take(char Expected) {
        Space();
        if (Position == Input.size() || Input[Position] != Expected) return false;
        ++Position;
        return true;
    }
    Json ParseValue(unsigned Depth) {
        if (Depth >= 64) Fail();
        Space();
        if (Position == Input.size()) Fail();
        switch (Input[Position]) {
        case '{': return ParseObject(Depth + 1);
        case '[': return ParseArray(Depth + 1);
        case '"': return Json(ParseString());
        case 't': Literal("true"); return Json(true);
        case 'f': Literal("false"); return Json(false);
        case 'n': Literal("null"); return Json();
        default: return ParseNumber();
        }
    }
    void Literal(std::string_view Text) {
        if (Input.substr(Position, Text.size()) != Text) Fail();
        Position += Text.size();
    }
    Json ParseObject(unsigned Depth) {
        ++Position;
        Json::Object Result;
        if (Take('}')) return Result;
        while (true) {
            Space();
            if (Position == Input.size() || Input[Position] != '"') Fail();
            auto Name = ParseString();
            if (!Take(':')) Fail();
            if (!Result.emplace(std::move(Name), ParseValue(Depth)).second) Fail();
            if (Take('}')) return Result;
            if (!Take(',')) Fail();
        }
    }
    Json ParseArray(unsigned Depth) {
        ++Position;
        Json::Array Result;
        if (Take(']')) return Result;
        while (true) {
            Result.push_back(ParseValue(Depth));
            if (Take(']')) return Result;
            if (!Take(',')) Fail();
        }
    }
    std::uint32_t Hex4() {
        if (Position + 4 > Input.size()) Fail();
        std::uint32_t Result = 0;
        for (unsigned I = 0; I < 4; ++I) {
            char C = Input[Position++];
            unsigned Digit = C >= '0' && C <= '9' ? C - '0' :
                C >= 'a' && C <= 'f' ? C - 'a' + 10 :
                C >= 'A' && C <= 'F' ? C - 'A' + 10 : 16;
            if (Digit == 16) Fail();
            Result = Result * 16 + Digit;
        }
        return Result;
    }
    std::string ParseString() {
        if (Input[Position++] != '"') Fail();
        std::string Result;
        while (Position < Input.size()) {
            unsigned char C = static_cast<unsigned char>(Input[Position++]);
            if (C == '"') {
                (void)Value::String(Result);
                return Result;
            }
            if (C < 0x20) Fail();
            if (C != '\\') { Result.push_back(static_cast<char>(C)); continue; }
            if (Position == Input.size()) Fail();
            switch (Input[Position++]) {
            case '"': Result.push_back('"'); break;
            case '\\': Result.push_back('\\'); break;
            case '/': Result.push_back('/'); break;
            case 'b': Result.push_back('\b'); break;
            case 'f': Result.push_back('\f'); break;
            case 'n': Result.push_back('\n'); break;
            case 'r': Result.push_back('\r'); break;
            case 't': Result.push_back('\t'); break;
            case 'u': {
                auto Point = Hex4();
                if (Point >= 0xd800 && Point <= 0xdbff) {
                    if (Position + 2 > Input.size() || Input[Position] != '\\' ||
                        Input[Position + 1] != 'u') Fail();
                    Position += 2;
                    auto Low = Hex4();
                    if (Low < 0xdc00 || Low > 0xdfff) Fail();
                    Point = 0x10000 + ((Point - 0xd800) << 10) + (Low - 0xdc00);
                } else if (Point >= 0xdc00 && Point <= 0xdfff) Fail();
                AppendUtf8(Result, Point);
                break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    Json ParseNumber() {
        auto Start = Position;
        if (Input[Position] == '-') ++Position;
        if (Position == Input.size()) Fail();
        if (Input[Position] == '0') ++Position;
        else {
            if (Input[Position] < '1' || Input[Position] > '9') Fail();
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
        }
        if (Position < Input.size() && Input[Position] == '.') {
            ++Position;
            auto Fraction = Position;
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
            if (Fraction == Position) Fail();
        }
        if (Position < Input.size() && (Input[Position] == 'e' || Input[Position] == 'E')) {
            ++Position;
            if (Position < Input.size() && (Input[Position] == '+' || Input[Position] == '-')) ++Position;
            auto Exponent = Position;
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
            if (Exponent == Position) Fail();
        }
        double Result = 0;
        auto Text = Input.substr(Start, Position - Start);
        auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Result);
        if (Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size() || !std::isfinite(Result)) Fail();
        return Json(Result);
    }
    std::string_view Input;
    std::size_t Position = 0;
};

void WriteJson(std::string& Output, const Json& Input);

void WriteString(std::string& Output, std::string_view Input) {
    constexpr char Hex[] = "0123456789abcdef";
    Output.push_back('"');
    for (unsigned char C : Input) {
        switch (C) {
        case '"': Output += "\\\""; break;
        case '\\': Output += "\\\\"; break;
        case '\b': Output += "\\b"; break;
        case '\f': Output += "\\f"; break;
        case '\n': Output += "\\n"; break;
        case '\r': Output += "\\r"; break;
        case '\t': Output += "\\t"; break;
        default:
            if (C < 0x20) {
                Output += "\\u00";
                Output.push_back(Hex[C >> 4]);
                Output.push_back(Hex[C & 15]);
            } else Output.push_back(static_cast<char>(C));
        }
    }
    Output.push_back('"');
}

void WriteJson(std::string& Output, const Json& Input) {
    const auto& Data = Input.Get();
    if (std::holds_alternative<std::nullptr_t>(Data)) { Output += "null"; return; }
    if (auto Value = std::get_if<bool>(&Data)) { Output += *Value ? "true" : "false"; return; }
    if (auto Value = std::get_if<double>(&Data)) {
        char Buffer[64];
        auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), *Value,
                                    std::chars_format::general);
        if (Result.ec != std::errc{}) throw std::runtime_error("cannot encode JSON number");
        Output.append(Buffer, Result.ptr);
        return;
    }
    if (auto Value = std::get_if<std::string>(&Data)) { WriteString(Output, *Value); return; }
    if (auto Value = std::get_if<Json::Object>(&Data)) {
        Output.push_back('{');
        bool First = true;
        for (const auto& [Name, Item] : *Value) {
            if (!First) Output.push_back(',');
            First = false;
            WriteString(Output, Name); Output.push_back(':'); WriteJson(Output, Item);
        }
        Output.push_back('}');
        return;
    }
    const auto& Value = std::get<Json::Array>(Data);
    Output.push_back('[');
    for (std::size_t I = 0; I < Value.size(); ++I) {
        if (I) Output.push_back(',');
        WriteJson(Output, Value[I]);
    }
    Output.push_back(']');
}

std::string Encode(const Json& Input) {
    std::string Result;
    WriteJson(Result, Input);
    return Result;
}

const Json* Find(const Json::Object& Object, std::string_view Name) {
    auto Found = Object.find(std::string(Name));
    return Found == Object.end() ? nullptr : &Found->second;
}

std::uint64_t Integer(const Json& Input, std::string_view Name) {
    if (!Input.IsNumber() || Input.Number() < 0 || std::floor(Input.Number()) != Input.Number() ||
        Input.Number() > 9'007'199'254'740'991.0)
        throw std::invalid_argument(std::string(Name) + " must be a nonnegative integer");
    return static_cast<std::uint64_t>(Input.Number());
}

const Json::Object& Parameters(const Json::Object& Request) {
    auto Input = Find(Request, "params");
    if (!Input) {
        static const Json::Object Empty;
        return Empty;
    }
    if (!Input->IsObject()) throw std::invalid_argument("params must be an object");
    return Input->Members();
}

std::string NumberDescription(double Input) {
    if (std::isnan(Input)) return "NaN";
    if (std::isinf(Input)) return Input < 0 ? "-Infinity" : "Infinity";
    char Buffer[64];
    auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), Input, std::chars_format::general);
    if (Result.ec != std::errc{}) return "number";
    return std::string(Buffer, Result.ptr);
}

} // namespace

struct DebugTarget::Impl final {
    enum class Step { None, Into, Over, Out };
    struct LocationKey {
        std::string ModuleId;
        std::uint32_t FunctionId = 0;
        std::size_t Pc = 0;
        bool operator==(const LocationKey&) const = default;
    };
    struct Breakpoint {
        std::uint64_t Id = 0;
        std::string ModuleId;
        std::size_t Line = 0;
        std::optional<std::size_t> Column;
        std::optional<LocationKey> Resolved;
    };
    struct PendingCommand {
        std::uint64_t Id = 0;
        std::string Method;
        Json::Object Params;
    };

    Impl(std::shared_ptr<DebugChannel> Input, std::size_t InputMaxMessageBytes)
        : Channel(std::move(Input)), MaxMessageBytes(InputMaxMessageBytes) {
        if (!Channel) throw std::invalid_argument("null debug channel");
        if (MaxMessageBytes == 0) throw std::invalid_argument("debug message limit must be positive");
    }

    void Send(const Json& Message) noexcept {
        try {
            auto Text = Encode(Message);
            Channel->SendProtocolMessage(Text);
        } catch (...) {}
    }
    void Result(std::uint64_t Id, Json::Object Value = {}) noexcept {
        Send(Json::Object{{"id", Json(Id)}, {"result", Json(std::move(Value))}});
    }
    void Error(std::optional<std::uint64_t> Id, std::string Code, std::string Message) noexcept {
        if (!Id) {
            Notify("Debugger.protocolError", {{"code", Json(std::move(Code))},
                                                {"message", Json(std::move(Message))}});
            return;
        }
        Send(Json::Object{{"error", Json::Object{{"code", Json(std::move(Code))},
                                                  {"message", Json(std::move(Message))}}},
                          {"id", Json(*Id)}});
    }
    void Notify(std::string Method, Json::Object Params = {}) noexcept {
        Send(Json::Object{{"method", Json(std::move(Method))},
                          {"params", Json(std::move(Params))}});
    }

    static Json::Object SourceFields(const DebugFrameView& Frame) {
        Json::Object Result{{"frameId", Json(static_cast<std::uint64_t>(Frame.FrameId))},
                            {"functionId", Json(static_cast<std::uint64_t>(Frame.FunctionId))},
                            {"pc", Json(static_cast<std::uint64_t>(Frame.Pc))}};
        if (Frame.Source) {
            Result.emplace("byteOffset", Json(static_cast<std::uint64_t>(Frame.Source->ByteOffset)));
            Result.emplace("column", Json(static_cast<std::uint64_t>(Frame.Source->Column)));
            Result.emplace("line", Json(static_cast<std::uint64_t>(Frame.Source->Line)));
            Result.emplace("moduleId", Json(Frame.Source->ModuleId));
        }
        return Result;
    }
    static LocationKey Key(const DebugFrameView& Frame) {
        return {Frame.Source ? Frame.Source->ModuleId : std::string{}, Frame.FunctionId, Frame.Pc};
    }

    Json::Object ValueSummary(const Value& Input) {
        Json::Object Result;
        switch (Input.GetType()) {
        case ValueType::Null:
            Result = {{"description", Json("null")}, {"type", Json("null")}};
            break;
        case ValueType::Bool:
            Result = {{"description", Json(Input.AsBool() ? "true" : "false")},
                      {"type", Json("bool")}, {"value", Json(Input.AsBool())}};
            break;
        case ValueType::Number:
            Result = {{"description", Json(NumberDescription(Input.AsNumber()))},
                      {"type", Json("number")}};
            if (std::isfinite(Input.AsNumber())) Result.emplace("value", Json(Input.AsNumber()));
            break;
        case ValueType::String:
            Result = {{"description", Json(Input.AsString())}, {"type", Json("string")},
                      {"value", Json(Input.AsString())}};
            break;
        case ValueType::Object: {
            auto* Identity = Input.AsObject();
            auto Found = ObjectIds.find(Identity);
            std::uint64_t Id;
            if (Found == ObjectIds.end()) {
                Id = Objects.size() + 1;
                ObjectIds.emplace(Identity, Id);
                Objects.push_back(Input);
            } else Id = Found->second;
            std::string Description;
            switch (Input.GetObjectType()) {
            case ObjectType::Script: Description = "ScriptObject"; break;
            case ObjectType::Function: Description = "Function"; break;
            case ObjectType::Error: {
                auto* Error = dynamic_cast<ErrorObject*>(Identity);
                Description = Error ? "Error: " + Error->GetMessage() : "Error";
                break;
            }
            case ObjectType::Host: Description = "HostObject"; break;
            }
            Result = {{"description", Json(std::move(Description))},
                      {"objectId", Json(Id)}, {"type", Json("object")}};
            break;
        }
        }
        return Result;
    }

    Json::Object StackTrace(const VmDebugContext& Context) {
        Json::Array Frames;
        Frames.reserve(Context.GetFrameCount());
        for (std::size_t I = 0; I < Context.GetFrameCount(); ++I)
            Frames.emplace_back(SourceFields(Context.GetFrame(I)));
        return {{"frames", Json(std::move(Frames))}, {"stopId", Json(StopId)}};
    }

    Json::Object Variables(const VmDebugContext& Context, const Json::Object& Params) {
        auto FrameValue = Find(Params, "frameId");
        auto ScopeValue = Find(Params, "scope");
        if (!FrameValue || !ScopeValue || !ScopeValue->IsString())
            throw std::invalid_argument("frameId and scope are required");
        auto FrameId = Integer(*FrameValue, "frameId");
        auto Frame = Context.GetFrame(static_cast<std::size_t>(FrameId));
        Json::Array Variables;
        if (ScopeValue->String() == "locals" || ScopeValue->String() == "stack") {
            auto Values = ScopeValue->String() == "locals" ? Frame.Locals : Frame.Stack;
            Variables.reserve(Values.size());
            for (std::size_t I = 0; I < Values.size(); ++I) {
                auto Summary = ValueSummary(Values[I]);
                Summary.emplace("name", Json("$" + std::to_string(I)));
                Variables.emplace_back(std::move(Summary));
            }
        } else if (ScopeValue->String() == "globals") {
            auto Values = Context.GetGlobals(static_cast<std::size_t>(FrameId));
            Variables.reserve(Values.size());
            for (const auto& Item : Values) {
                auto Summary = ValueSummary(Item.Data);
                Summary.emplace("name", Json(Item.Name));
                Variables.emplace_back(std::move(Summary));
            }
        } else throw std::invalid_argument("unknown variable scope");
        return {{"stopId", Json(StopId)}, {"variables", Json(std::move(Variables))}};
    }

    Json::Object Properties(const VmDebugContext& Context, const Json::Object& Params) {
        auto ObjectValue = Find(Params, "objectId");
        if (!ObjectValue) throw std::invalid_argument("objectId is required");
        auto Id = Integer(*ObjectValue, "objectId");
        if (Id == 0 || Id > Objects.size()) throw std::invalid_argument("objectId is not valid for this stop");
        auto Values = Context.GetProperties(Objects[static_cast<std::size_t>(Id - 1)]);
        Json::Array Properties;
        Properties.reserve(Values.size());
        for (const auto& Item : Values) {
            auto Summary = ValueSummary(Item.Data);
            Summary.emplace("key", Json(ValueSummary(Item.Key)));
            Properties.emplace_back(std::move(Summary));
        }
        return {{"properties", Json(std::move(Properties))}, {"stopId", Json(StopId)}};
    }

    void Dispatch(std::string_view Message) noexcept {
        std::optional<std::uint64_t> RequestId;
        try {
            if (Message.size() > MaxMessageBytes)
                throw std::invalid_argument("debug protocol message exceeds its size limit");
            auto Root = JsonParser(Message).Parse();
            if (!Root.IsObject()) throw std::invalid_argument("request must be an object");
            const auto& Request = Root.Members();
            auto IdValue = Find(Request, "id");
            if (!IdValue) throw std::invalid_argument("request id is required");
            RequestId = Integer(*IdValue, "id");
            auto MethodValue = Find(Request, "method");
            if (!MethodValue || !MethodValue->IsString())
                throw std::invalid_argument("request method is required");
            auto Method = MethodValue->String();
            const auto& Params = Parameters(Request);

            bool IsClosed;
            { std::lock_guard Lock(Mutex); IsClosed = Closed; }
            if (IsClosed) { Error(RequestId, "Closed", "debug target is closed"); return; }

            if (Method == "Debugger.enable") {
                bool Accepted;
                {
                    std::lock_guard Lock(Mutex);
                    Accepted = !Closed;
                    if (Accepted) Enabled = true;
                }
                if (!Accepted) { Error(RequestId, "Closed", "debug target is closed"); return; }
                Result(*RequestId, {{"protocolVersion", Json("1.0")}});
                return;
            }
            if (Method == "Debugger.disable") {
                std::uint64_t OldStop = 0;
                bool WasPaused = false;
                std::deque<PendingCommand> Cancelled;
                {
                    std::lock_guard Lock(Mutex);
                    Enabled = false; PauseRequested = false; CurrentStep = Step::None;
                    WasPaused = Paused; OldStop = StopId; Paused = false;
                    Cancelled.swap(Pending);
                }
                Wake.notify_all();
                for (const auto& Command : Cancelled)
                    Error(Command.Id, "Cancelled", "debugger was disabled");
                Result(*RequestId);
                if (WasPaused) Notify("Debugger.resumed", {{"stopId", Json(OldStop)}});
                return;
            }

            bool IsEnabled;
            { std::lock_guard Lock(Mutex); IsEnabled = Enabled; }
            if (!IsEnabled) { Error(RequestId, "NotEnabled", "Debugger.enable is required"); return; }
            if (Method == "Debugger.setBreakpoint") {
                auto Module = Find(Params, "moduleId");
                auto Line = Find(Params, "line");
                auto Column = Find(Params, "column");
                if (!Module || !Module->IsString() || !Line)
                    throw std::invalid_argument("moduleId and line are required");
                auto LineNumber = Integer(*Line, "line");
                if (LineNumber == 0) throw std::invalid_argument("line is one-based");
                std::optional<std::size_t> ColumnNumber;
                if (Column) {
                    auto Value = Integer(*Column, "column");
                    if (Value == 0) throw std::invalid_argument("column is one-based");
                    ColumnNumber = static_cast<std::size_t>(Value);
                }
                std::uint64_t BreakpointId;
                {
                    std::lock_guard Lock(Mutex);
                    BreakpointId = NextBreakpointId++;
                    Breakpoints.push_back({BreakpointId, Module->String(),
                        static_cast<std::size_t>(LineNumber), ColumnNumber, std::nullopt});
                }
                Result(*RequestId, {{"breakpointId", Json(BreakpointId)}});
                return;
            }
            if (Method == "Debugger.removeBreakpoint") {
                auto Id = Find(Params, "breakpointId");
                if (!Id) throw std::invalid_argument("breakpointId is required");
                auto BreakpointId = Integer(*Id, "breakpointId");
                bool Removed;
                {
                    std::lock_guard Lock(Mutex);
                    auto Before = Breakpoints.size();
                    std::erase_if(Breakpoints, [&](const auto& Item) { return Item.Id == BreakpointId; });
                    Removed = Breakpoints.size() != Before;
                }
                if (!Removed) Error(RequestId, "UnknownBreakpoint", "breakpointId was not found");
                else Result(*RequestId);
                return;
            }
            if (Method == "Debugger.pause") {
                {
                    std::lock_guard Lock(Mutex);
                    if (!Paused) PauseRequested = true;
                }
                Result(*RequestId);
                return;
            }
            if (Method == "Debugger.resume" || Method == "Debugger.stepInto" ||
                Method == "Debugger.stepOver" || Method == "Debugger.stepOut" ||
                Method == "Debugger.getStackTrace" || Method == "Debugger.getVariables" ||
                Method == "Debugger.getProperties") {
                bool IsPaused;
                {
                    std::lock_guard Lock(Mutex);
                    IsPaused = Paused;
                    if (IsPaused) Pending.push_back({*RequestId, std::move(Method), Params});
                }
                if (!IsPaused) { Error(RequestId, "NotPaused", "the VM is not paused"); return; }
                Wake.notify_all();
                return;
            }
            Error(RequestId, "MethodNotFound", "unknown debug protocol method");
        } catch (const std::exception& Failure) {
            Error(RequestId, "InvalidRequest", Failure.what());
        } catch (...) {
            Error(RequestId, "InternalError", "unknown protocol failure");
        }
    }

    void ProcessPaused(const VmDebugContext& Context, PendingCommand Command) {
        try {
            if (Command.Method == "Debugger.getStackTrace") {
                Result(Command.Id, StackTrace(Context)); return;
            }
            if (Command.Method == "Debugger.getVariables") {
                Result(Command.Id, Variables(Context, Command.Params)); return;
            }
            if (Command.Method == "Debugger.getProperties") {
                Result(Command.Id, Properties(Context, Command.Params)); return;
            }
            Step NextStep = Step::None;
            if (Command.Method == "Debugger.stepInto") NextStep = Step::Into;
            else if (Command.Method == "Debugger.stepOver") NextStep = Step::Over;
            else if (Command.Method == "Debugger.stepOut") NextStep = Step::Out;
            auto Frame = Context.GetFrame(0);
            std::uint64_t OldStop;
            std::deque<PendingCommand> Cancelled;
            {
                std::lock_guard Lock(Mutex);
                CurrentStep = NextStep;
                StepStart = Key(Frame);
                StepDepth = Context.GetFrameCount();
                OldStop = StopId;
                Paused = false;
                Objects.clear(); ObjectIds.clear();
                Cancelled.swap(Pending);
            }
            Result(Command.Id);
            for (const auto& Item : Cancelled)
                Error(Item.Id, "NotPaused", "the VM resumed before this request was handled");
            Notify("Debugger.resumed", {{"stopId", Json(OldStop)}});
            Wake.notify_all();
        } catch (const std::exception& Failure) {
            Error(Command.Id, "InvalidRequest", Failure.what());
        }
    }

    void SafePoint(const VmDebugContext& Context) {
        auto Frame = Context.GetFrame(0);
        auto Current = Key(Frame);
        std::string Reason;
        std::vector<Json::Object> ResolvedEvents;
        {
            std::lock_guard Lock(Mutex);
            if (!Enabled) return;
            if (PauseRequested) {
                PauseRequested = false;
                Reason = "pause";
            }
            if (Reason.empty() && CurrentStep != Step::None) {
                bool Different = !(Current == StepStart);
                bool Stop = CurrentStep == Step::Into ? Different :
                    CurrentStep == Step::Over ? Different && Context.GetFrameCount() <= StepDepth :
                    Context.GetFrameCount() < StepDepth;
                if (Stop) { Reason = "step"; CurrentStep = Step::None; }
            }
            if (Frame.Source) {
                for (auto& Breakpoint : Breakpoints) {
                    bool Match = Breakpoint.Resolved ? *Breakpoint.Resolved == Current :
                        Breakpoint.ModuleId == Frame.Source->ModuleId &&
                        Breakpoint.Line == Frame.Source->Line &&
                        (!Breakpoint.Column || *Breakpoint.Column == Frame.Source->Column);
                    if (!Match) continue;
                    if (!Breakpoint.Resolved) {
                        Breakpoint.Resolved = Current;
                        auto Fields = SourceFields(Frame);
                        Fields.emplace("breakpointId", Json(Breakpoint.Id));
                        ResolvedEvents.push_back(std::move(Fields));
                    }
                    Reason = "breakpoint";
                    break;
                }
            }
            if (Reason.empty()) return;
            Paused = true;
            ++StopId;
            Objects.clear(); ObjectIds.clear();
        }
        for (auto& Event : ResolvedEvents)
            Notify("Debugger.breakpointResolved", std::move(Event));
        auto PausedFields = SourceFields(Frame);
        PausedFields.emplace("reason", Json(Reason));
        PausedFields.emplace("stopId", Json(StopId));
        Notify("Debugger.paused", std::move(PausedFields));

        while (true) {
            PendingCommand Command;
            {
                std::unique_lock Lock(Mutex);
                Wake.wait(Lock, [&] { return !Paused || !Enabled || !Pending.empty(); });
                if (!Paused || !Enabled) {
                    Objects.clear();
                    ObjectIds.clear();
                    return;
                }
                Command = std::move(Pending.front());
                Pending.pop_front();
            }
            ProcessPaused(Context, std::move(Command));
            std::lock_guard Lock(Mutex);
            if (!Paused) {
                Objects.clear();
                ObjectIds.clear();
                return;
            }
        }
    }

    void Finished(bool Faulted) {
        bool SendEvent;
        std::deque<PendingCommand> Cancelled;
        {
            std::lock_guard Lock(Mutex);
            SendEvent = Enabled;
            Paused = false; PauseRequested = false; CurrentStep = Step::None;
            Cancelled.swap(Pending); Objects.clear(); ObjectIds.clear();
        }
        Wake.notify_all();
        for (const auto& Command : Cancelled)
            Error(Command.Id, "ExecutionFinished", "execution finished before this request was handled");
        if (SendEvent) Notify("Debugger.executionFinished", {{"faulted", Json(Faulted)}});
    }

    void Close() noexcept {
        {
            std::lock_guard Lock(Mutex);
            Closed = true;
            Enabled = false;
            Paused = false;
            PauseRequested = false;
            CurrentStep = Step::None;
            Pending.clear();
        }
        Wake.notify_all();
    }

    std::shared_ptr<DebugChannel> Channel;
    std::size_t MaxMessageBytes;
    std::mutex Mutex;
    std::condition_variable Wake;
    bool Enabled = false;
    bool Closed = false;
    bool Paused = false;
    bool PauseRequested = false;
    std::uint64_t StopId = 0;
    std::uint64_t NextBreakpointId = 1;
    std::vector<Breakpoint> Breakpoints;
    std::deque<PendingCommand> Pending;
    Step CurrentStep = Step::None;
    LocationKey StepStart;
    std::size_t StepDepth = 0;
    std::unordered_map<Object*, std::uint64_t> ObjectIds;
    std::vector<Value> Objects;
};

DebugTarget::DebugTarget(std::shared_ptr<DebugChannel> Channel, std::size_t MaxMessageBytes)
    : State(std::make_unique<Impl>(std::move(Channel), MaxMessageBytes)) {}
DebugTarget::~DebugTarget() { State->Close(); }

void DebugTarget::DispatchProtocolMessage(std::string_view Message) noexcept {
    State->Dispatch(Message);
}

void DebugTarget::Close() noexcept {
    State->Close();
}

void DebugTarget::OnSafePoint(const VmDebugContext& Context) {
    try { State->SafePoint(Context); }
    catch (...) {
        std::lock_guard Lock(State->Mutex);
        State->Enabled = false;
        State->Paused = false;
        State->Wake.notify_all();
    }
}

void DebugTarget::OnExecutionFinished(bool Faulted) {
    try { State->Finished(Faulted); }
    catch (...) {}
}

} // namespace Feather
