#include <Feather/Debug.hpp>

#include "../Protocol/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Feather {
namespace {

using Protocol::Find;
using Protocol::Integer;
using Protocol::Json;

std::string Encode(const Json& Input) { return Protocol::EncodeJson(Input); }

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

    Impl(std::shared_ptr<DebugChannel> Input, DebugTargetOptions Options)
        : Channel(std::move(Input)), MaxMessageBytes(Options.MaxMessageBytes),
          WaitingForDebugger(Options.WaitForDebugger),
          ReportEachExecution(Options.ReportEachExecution) {
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
        auto FunctionName = Frame.FunctionName.empty() ?
            "<function #" + std::to_string(Frame.FunctionId) + ">" : std::string(Frame.FunctionName);
        Json::Object Result{{"frameId", Json(static_cast<std::uint64_t>(Frame.FrameId))},
                            {"functionId", Json(static_cast<std::uint64_t>(Frame.FunctionId))},
                            {"functionName", Json(std::move(FunctionName))},
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
        if (ScopeValue->String() == "locals") {
            if (Frame.LocalVariables.empty()) {
                Variables.reserve(Frame.Locals.size());
                for (std::size_t I = 0; I < Frame.Locals.size(); ++I) {
                    auto Summary = ValueSummary(Frame.Locals[I]);
                    Summary.emplace("name", Json("$" + std::to_string(I)));
                    Variables.emplace_back(std::move(Summary));
                }
            } else {
                std::unordered_set<std::string> SeenNames;
                for (auto It = Frame.LocalVariables.rbegin(); It != Frame.LocalVariables.rend(); ++It) {
                    const auto& Variable = *It;
                    if (Frame.Pc < Variable.StartPc || Frame.Pc >= Variable.EndPc) continue;
                    if (!SeenNames.insert(Variable.Name).second) continue;
                    auto Summary = ValueSummary(Frame.Locals[Variable.Slot]);
                    Summary.emplace("name", Json(Variable.Name));
                    Variables.emplace_back(std::move(Summary));
                }
                std::reverse(Variables.begin(), Variables.end());
            }
        } else if (ScopeValue->String() == "stack") {
            Variables.reserve(Frame.Stack.size());
            for (std::size_t I = 0; I < Frame.Stack.size(); ++I) {
                auto Summary = ValueSummary(Frame.Stack[I]);
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
            auto Root = Protocol::ParseJson(Message);
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
                    Enabled = false; PauseRequested = false; PauseOnErrors = false;
                    CurrentStep = Step::None;
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
            if (Method == "Runtime.runIfWaitingForDebugger") {
                {
                    std::lock_guard Lock(Mutex);
                    WaitingForDebugger = false;
                }
                Result(*RequestId);
                Wake.notify_all();
                return;
            }
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
            if (Method == "Debugger.setPauseOnErrors") {
                auto EnabledValue = Find(Params, "enabled");
                if (!EnabledValue || !EnabledValue->IsBool())
                    throw std::invalid_argument("enabled must be a boolean");
                {
                    std::lock_guard Lock(Mutex);
                    PauseOnErrors = EnabledValue->Bool();
                }
                Result(*RequestId);
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

    void WaitWhilePaused(const VmDebugContext& Context) {
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
            if (Frame.Source && Frame.Breakable) {
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

        WaitWhilePaused(Context);
    }

    void ErrorPoint(const VmDebugContext& Context, const Value& ErrorValue,
                    DebugErrorOrigin Origin) {
        auto Frame = Context.GetFrame(0);
        Json::Object ErrorSummary;
        std::uint64_t CurrentStop;
        {
            std::lock_guard Lock(Mutex);
            if (!Enabled || !PauseOnErrors) return;
            Paused = true;
            CurrentStop = ++StopId;
            Objects.clear(); ObjectIds.clear();
            ErrorSummary = ValueSummary(ErrorValue);
        }
        auto Fields = SourceFields(Frame);
        Fields.emplace("error", Json(std::move(ErrorSummary)));
        Fields.emplace("origin", Json(Origin == DebugErrorOrigin::Operation
            ? "operation" : "functionReturn"));
        Fields.emplace("reason", Json("error"));
        Fields.emplace("stopId", Json(CurrentStop));
        Notify("Debugger.paused", std::move(Fields));
        WaitWhilePaused(Context);
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
            WaitingForDebugger = false;
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
    bool PauseOnErrors = false;
    bool WaitingForDebugger = false;
    bool ReportEachExecution = true;
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

DebugTarget::DebugTarget(std::shared_ptr<DebugChannel> Channel, DebugTargetOptions Options)
    : State(std::make_unique<Impl>(std::move(Channel), Options)) {}
DebugTarget::~DebugTarget() { State->Close(); }

void DebugTarget::DispatchProtocolMessage(std::string_view Message) noexcept {
    State->Dispatch(Message);
}

bool DebugTarget::WaitForExecutionPermission() noexcept {
    std::unique_lock Lock(State->Mutex);
    State->Wake.wait(Lock, [&] { return !State->WaitingForDebugger || State->Closed; });
    return !State->Closed;
}

void DebugTarget::NotifyExecutionFinished(bool Faulted) noexcept {
    try { State->Finished(Faulted); }
    catch (...) {}
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

void DebugTarget::OnError(const VmDebugContext& Context, const Value& Error,
                          DebugErrorOrigin Origin) {
    try { State->ErrorPoint(Context, Error, Origin); }
    catch (...) {
        std::lock_guard Lock(State->Mutex);
        State->Enabled = false;
        State->Paused = false;
        State->Wake.notify_all();
    }
}

void DebugTarget::OnExecutionFinished(bool Faulted) {
    try { if (State->ReportEachExecution) State->Finished(Faulted); }
    catch (...) {}
}

} // namespace Feather
