#include <Feather/Dap.hpp>

#include "../Protocol/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Feather {
namespace {

using Protocol::EncodeJson;
using Protocol::Find;
using Protocol::Integer;
using Protocol::Json;
using Protocol::ParseJson;

const Json::Object& ObjectField(const Json::Object& Object, std::string_view Name,
                                bool Optional = false) {
    auto Value = Find(Object, Name);
    if (!Value) {
        if (Optional) { static const Json::Object Empty; return Empty; }
        throw std::invalid_argument(std::string(Name) + " is required");
    }
    if (!Value->IsObject()) throw std::invalid_argument(std::string(Name) + " must be an object");
    return Value->Members();
}

std::string StringField(const Json::Object& Object, std::string_view Name) {
    auto Value = Find(Object, Name);
    if (!Value || !Value->IsString())
        throw std::invalid_argument(std::string(Name) + " must be a string");
    return Value->String();
}

std::uint64_t IntegerField(const Json::Object& Object, std::string_view Name) {
    auto Value = Find(Object, Name);
    if (!Value) throw std::invalid_argument(std::string(Name) + " is required");
    return Integer(*Value, Name);
}

Json::Object SourceObject(std::string Path) {
    if (Path.empty()) return {};
    return {{"name", Json(Path.substr(Path.find_last_of("/\\") + 1))},
            {"path", Json(std::move(Path))}};
}

} // namespace

struct DapAdapter::Impl final {
    enum class PendingKind {
        Simple, Attach, Stack, Variables, Properties, Evaluate, SetVariable,
        SetBreakpoint, RemoveBreakpoint
    };
    struct Pending {
        PendingKind Kind = PendingKind::Simple;
        std::uint64_t DapRequest = 0;
        std::string DapCommand;
        std::uint64_t Batch = 0;
        std::size_t Item = 0;
        std::uint64_t FrameId = 0;
    };
    struct BreakpointBatch {
        std::uint64_t Request = 0;
        std::string Source;
        std::size_t Remaining = 0;
        std::vector<Json::Object> Results;
        std::vector<std::uint64_t> NewIds;
        bool Failed = false;
        std::string Failure;
    };
    struct Reference {
        enum class Kind { Scope, Object } Type = Kind::Scope;
        std::uint64_t FrameOrObject = 0;
        std::string Scope;
        std::uint64_t FrameId = 0;
    };

    Impl(std::shared_ptr<DapChannel> Input, DapAdapterOptions InputOptions)
        : Channel(std::move(Input)), Options(std::move(InputOptions)) {
        if (!Channel) throw std::invalid_argument("null DAP channel");
        if (Options.MaxMessageBytes == 0) throw std::invalid_argument("DAP message limit must be positive");
    }

    Json::Object Envelope(std::string Type) {
        return {{"seq", Json(NextDapSequence++)}, {"type", Json(std::move(Type))}};
    }

    std::string Response(std::uint64_t Request, std::string Command, bool Success,
                         Json::Object Body = {}, std::string Message = {}) {
        auto Result = Envelope("response");
        Result.emplace("request_seq", Json(Request));
        Result.emplace("success", Json(Success));
        Result.emplace("command", Json(std::move(Command)));
        if (!Body.empty()) Result.emplace("body", Json(std::move(Body)));
        if (!Message.empty()) Result.emplace("message", Json(std::move(Message)));
        return EncodeJson(Json(std::move(Result)));
    }

    std::string Event(std::string Name, Json::Object Body = {}) {
        auto Result = Envelope("event");
        Result.emplace("event", Json(std::move(Name)));
        if (!Body.empty()) Result.emplace("body", Json(std::move(Body)));
        return EncodeJson(Json(std::move(Result)));
    }

    std::pair<std::uint64_t, std::string> TargetRequest(std::string Method,
                                                        Json::Object Params = {}) {
        auto Id = NextTargetId++;
        Json::Object Request{{"id", Json(Id)}, {"method", Json(std::move(Method))}};
        if (!Params.empty()) Request.emplace("params", Json(std::move(Params)));
        return {Id, EncodeJson(Json(std::move(Request)))};
    }

    std::string ModuleId(std::string_view Path) const {
        return !Options.PrimarySourcePath.empty() && Path == Options.PrimarySourcePath
            ? std::string{} : std::string(Path);
    }

    std::string SourcePath(std::string_view Module) const {
        return Module.empty() ? Options.PrimarySourcePath : std::string(Module);
    }

    std::uint64_t AddReference(Reference Value) {
        auto Id = NextReference++;
        References.emplace(Id, std::move(Value));
        return Id;
    }

    void ResetReferences() {
        References.clear();
        NextReference = 1;
    }

    static std::string TargetFailure(const Json::Object& Message) {
        auto Error = Find(Message, "error");
        if (!Error || !Error->IsObject()) return "Feather debug request failed";
        auto Text = Find(Error->Members(), "message");
        return Text && Text->IsString() ? Text->String() : "Feather debug request failed";
    }

    Json::Object DapValue(const Json::Object& Input, std::uint64_t FrameId = 0) {
        Json::Object Result;
        auto Description = Find(Input, "description");
        Result.emplace("value", Json(Description && Description->IsString()
            ? Description->String() : std::string("<value>")));
        auto Type = Find(Input, "type");
        if (Type && Type->IsString()) Result.emplace("type", Json(Type->String()));
        std::uint64_t ReferenceId = 0;
        if (auto ObjectId = Find(Input, "objectId"))
            ReferenceId = AddReference({Reference::Kind::Object,
                Integer(*ObjectId, "objectId"), {}, FrameId});
        Result.emplace("variablesReference", Json(ReferenceId));
        return Result;
    }

    void CompleteBatch(BreakpointBatch& Batch, std::vector<std::string>& DapOutput) {
        if (Batch.Remaining != 0) return;
        if (Batch.Failed) {
            DapOutput.push_back(Response(Batch.Request, "setBreakpoints", false, {}, Batch.Failure));
        } else {
            Json::Array Items;
            for (auto& Item : Batch.Results) Items.emplace_back(std::move(Item));
            DapOutput.push_back(Response(Batch.Request, "setBreakpoints", true,
                                         {{"breakpoints", Json(std::move(Items))}}));
            SourceBreakpoints[Batch.Source] = std::move(Batch.NewIds);
        }
    }

    void Dap(std::string_view Message, std::vector<std::string>& DapOutput,
             std::vector<std::string>& TargetOutput) {
        if (Message.size() > Options.MaxMessageBytes)
            throw std::invalid_argument("DAP message exceeds its size limit");
        auto Parsed = ParseJson(Message);
        if (!Parsed.IsObject()) throw std::invalid_argument("DAP message must be an object");
        const auto& Request = Parsed.Members();
        if (StringField(Request, "type") != "request")
            throw std::invalid_argument("only DAP requests are accepted from the client");
        auto Sequence = IntegerField(Request, "seq");
        auto Command = StringField(Request, "command");
        const auto& Arguments = ObjectField(Request, "arguments", true);

        if (Closed) { DapOutput.push_back(Response(Sequence, Command, false, {}, "adapter is closed")); return; }
        if (Command == "initialize") {
            Json::Array ExceptionFilters;
            ExceptionFilters.emplace_back(Json::Object{{"filter", Json("error")},
                {"label", Json("Error results")}, {"default", Json(false)}});
            DapOutput.push_back(Response(Sequence, Command, true, {
                {"supportsConfigurationDoneRequest", Json(true)},
                {"exceptionBreakpointFilters", Json(std::move(ExceptionFilters))},
                {"supportsTerminateRequest", Json(false)},
                {"supportsEvaluateForHovers", Json(true)},
                {"supportsConditionalBreakpoints", Json(true)},
                {"supportsSetVariable", Json(true)}}));
            return;
        }
        if (Command == "attach" || Command == "launch") {
            auto [Id, Out] = TargetRequest("Debugger.enable");
            PendingRequests.emplace(Id, Pending{PendingKind::Attach, Sequence, Command});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        if (Command == "configurationDone") {
            DapOutput.push_back(Response(Sequence, Command, true));
            return;
        }
        if (Command == "disconnect") {
            auto [Id, Out] = TargetRequest("Debugger.disable");
            PendingRequests.emplace(Id, Pending{PendingKind::Simple, Sequence, Command});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        if (Command == "threads") {
            Json::Array Threads;
            Threads.emplace_back(Json::Object{{"id", Json(std::uint64_t{1})},
                                              {"name", Json("Feather VM")}});
            DapOutput.push_back(Response(Sequence, Command, true,
                                         {{"threads", Json(std::move(Threads))}}));
            return;
        }
        if (Command == "stackTrace") {
            auto [Id, Out] = TargetRequest("Debugger.getStackTrace");
            PendingRequests.emplace(Id, Pending{PendingKind::Stack, Sequence, Command});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        if (Command == "scopes") {
            auto Frame = IntegerField(Arguments, "frameId");
            Json::Array Scopes;
            for (auto Name : {"Locals", "Stack", "Globals"}) {
                std::string Scope = Name;
                std::transform(Scope.begin(), Scope.end(), Scope.begin(),
                               [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
                auto Ref = AddReference({Reference::Kind::Scope, Frame, std::move(Scope), Frame});
                Scopes.emplace_back(Json::Object{{"name", Json(Name)},
                    {"variablesReference", Json(Ref)}, {"expensive", Json(Name == std::string_view("Globals"))}});
            }
            DapOutput.push_back(Response(Sequence, Command, true, {{"scopes", Json(std::move(Scopes))}}));
            return;
        }
        if (Command == "variables") {
            auto Ref = IntegerField(Arguments, "variablesReference");
            auto Found = References.find(Ref);
            if (Found == References.end()) {
                DapOutput.push_back(Response(Sequence, Command, false, {}, "stale variablesReference"));
                return;
            }
            std::pair<std::uint64_t, std::string> Out;
            PendingKind Kind;
            if (Found->second.Type == Reference::Kind::Scope) {
                Kind = PendingKind::Variables;
                Out = TargetRequest("Debugger.getVariables", {{"frameId", Json(Found->second.FrameOrObject)},
                                                               {"scope", Json(Found->second.Scope)}});
            } else {
                Kind = PendingKind::Properties;
                Out = TargetRequest("Debugger.getProperties", {{"objectId", Json(Found->second.FrameOrObject)}});
            }
            PendingRequests.emplace(Out.first, Pending{Kind, Sequence, Command, 0, 0,
                Found->second.FrameId});
            TargetOutput.push_back(std::move(Out.second));
            return;
        }
        if (Command == "evaluate") {
            auto Frame = IntegerField(Arguments, "frameId");
            auto Expression = StringField(Arguments, "expression");
            auto [Id, Out] = TargetRequest("Debugger.evaluate",
                {{"frameId", Json(Frame)}, {"expression", Json(std::move(Expression))}});
            PendingRequests.emplace(Id, Pending{PendingKind::Evaluate, Sequence, Command, 0, 0, Frame});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        if (Command == "setVariable") {
            auto Ref = IntegerField(Arguments, "variablesReference");
            auto Found = References.find(Ref);
            if (Found == References.end()) {
                DapOutput.push_back(Response(Sequence, Command, false, {}, "stale variablesReference"));
                return;
            }
            Json::Object Params{{"name", Json(StringField(Arguments, "name"))},
                                {"expression", Json(StringField(Arguments, "value"))},
                                {"frameId", Json(Found->second.FrameId)}};
            if (Found->second.Type == Reference::Kind::Scope)
                Params.emplace("scope", Json(Found->second.Scope));
            else Params.emplace("objectId", Json(Found->second.FrameOrObject));
            auto [Id, Out] = TargetRequest("Debugger.setVariable", std::move(Params));
            PendingRequests.emplace(Id, Pending{PendingKind::SetVariable, Sequence, Command,
                                                0, 0, Found->second.FrameId});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        if (Command == "setBreakpoints") {
            const auto& Source = ObjectField(Arguments, "source");
            auto Path = StringField(Source, "path");
            auto Input = Find(Arguments, "breakpoints");
            if (!Input || !Input->IsArray()) throw std::invalid_argument("breakpoints must be an array");
            auto BatchId = NextBatch++;
            BreakpointBatch Batch;
            Batch.Request = Sequence;
            Batch.Source = Path;
            Batch.Results.resize(Input->Items().size());
            Batch.Remaining = Input->Items().size();
            if (auto Old = SourceBreakpoints.find(Path); Old != SourceBreakpoints.end())
                Batch.Remaining += Old->second.size();
            Batches.emplace(BatchId, std::move(Batch));
            if (auto Old = SourceBreakpoints.find(Path); Old != SourceBreakpoints.end()) {
                for (auto BreakpointId : Old->second) {
                    auto [Id, Out] = TargetRequest("Debugger.removeBreakpoint", {{"breakpointId", Json(BreakpointId)}});
                    PendingRequests.emplace(Id, Pending{PendingKind::RemoveBreakpoint, 0, {}, BatchId});
                    TargetOutput.push_back(std::move(Out));
                }
            }
            for (std::size_t I = 0; I < Input->Items().size(); ++I) {
                if (!Input->Items()[I].IsObject()) throw std::invalid_argument("breakpoint must be an object");
                const auto& Item = Input->Items()[I].Members();
                Json::Object Params{{"moduleId", Json(ModuleId(Path))},
                                    {"line", Json(IntegerField(Item, "line"))}};
                if (auto Column = Find(Item, "column")) Params.emplace("column", Json(Integer(*Column, "column")));
                if (auto Condition = Find(Item, "condition")) {
                    if (!Condition->IsString()) throw std::invalid_argument("breakpoint condition must be a string");
                    if (!Condition->String().empty()) Params.emplace("condition", Json(Condition->String()));
                }
                auto [Id, Out] = TargetRequest("Debugger.setBreakpoint", std::move(Params));
                PendingRequests.emplace(Id, Pending{PendingKind::SetBreakpoint, 0, {}, BatchId, I});
                TargetOutput.push_back(std::move(Out));
            }
            auto& Stored = Batches.at(BatchId);
            if (Stored.Remaining == 0) {
                CompleteBatch(Stored, DapOutput);
                Batches.erase(BatchId);
            }
            return;
        }
        if (Command == "setExceptionBreakpoints") {
            bool Enabled = false;
            if (auto Filters = Find(Arguments, "filters")) {
                if (!Filters->IsArray()) throw std::invalid_argument("filters must be an array");
                for (const auto& Filter : Filters->Items()) {
                    if (!Filter.IsString()) throw std::invalid_argument("exception filter must be a string");
                    if (Filter.String() == "error") Enabled = true;
                }
            }
            auto [Id, Out] = TargetRequest("Debugger.setPauseOnErrors", {{"enabled", Json(Enabled)}});
            PendingRequests.emplace(Id, Pending{PendingKind::Simple, Sequence, Command});
            TargetOutput.push_back(std::move(Out));
            return;
        }

        std::optional<std::string> Method;
        if (Command == "continue") Method = "Debugger.resume";
        else if (Command == "next") Method = "Debugger.stepOver";
        else if (Command == "stepIn") Method = "Debugger.stepInto";
        else if (Command == "stepOut") Method = "Debugger.stepOut";
        else if (Command == "pause") Method = "Debugger.pause";
        if (Method) {
            auto [Id, Out] = TargetRequest(*Method);
            PendingRequests.emplace(Id, Pending{PendingKind::Simple, Sequence, Command});
            TargetOutput.push_back(std::move(Out));
            return;
        }
        DapOutput.push_back(Response(Sequence, Command, false, {}, "unsupported DAP request"));
    }

    void Target(std::string_view Message, std::vector<std::string>& DapOutput) {
        if (Message.size() > Options.MaxMessageBytes) throw std::invalid_argument("target message exceeds its size limit");
        auto Parsed = ParseJson(Message);
        if (!Parsed.IsObject()) throw std::invalid_argument("target message must be an object");
        const auto& Input = Parsed.Members();
        if (auto IdValue = Find(Input, "id")) {
            auto Id = Integer(*IdValue, "id");
            auto Found = PendingRequests.find(Id);
            if (Found == PendingRequests.end()) return;
            auto PendingValue = std::move(Found->second);
            PendingRequests.erase(Found);
            auto Error = Find(Input, "error");
            auto ResultValue = Find(Input, "result");
            const Json::Object Empty;
            const auto& Result = ResultValue && ResultValue->IsObject() ? ResultValue->Members() : Empty;

            if (PendingValue.Kind == PendingKind::SetBreakpoint || PendingValue.Kind == PendingKind::RemoveBreakpoint) {
                auto BatchFound = Batches.find(PendingValue.Batch);
                if (BatchFound == Batches.end()) return;
                auto& Batch = BatchFound->second;
                if (Error) { Batch.Failed = true; Batch.Failure = TargetFailure(Input); }
                if (!Error && PendingValue.Kind == PendingKind::SetBreakpoint) {
                    auto BreakpointId = IntegerField(Result, "breakpointId");
                    Batch.NewIds.push_back(BreakpointId);
                    Batch.Results[PendingValue.Item] = {{"id", Json(BreakpointId)}, {"verified", Json(false)}};
                }
                if (Batch.Remaining) --Batch.Remaining;
                CompleteBatch(Batch, DapOutput);
                if (Batch.Remaining == 0) Batches.erase(BatchFound);
                return;
            }
            if (Error) {
                DapOutput.push_back(Response(PendingValue.DapRequest, PendingValue.DapCommand,
                                             false, {}, TargetFailure(Input)));
                return;
            }
            if (PendingValue.Kind == PendingKind::Attach) {
                DapOutput.push_back(Response(PendingValue.DapRequest, PendingValue.DapCommand, true));
                DapOutput.push_back(Event("initialized"));
            } else if (PendingValue.Kind == PendingKind::Stack) {
                Json::Array Frames;
                auto InputFrames = Find(Result, "frames");
                if (InputFrames && InputFrames->IsArray()) for (const auto& Frame : InputFrames->Items()) {
                    if (!Frame.IsObject()) continue;
                    const auto& Fields = Frame.Members();
                    Json::Object Out{{"id", Json(IntegerField(Fields, "frameId"))},
                                     {"name", Json(StringField(Fields, "functionName"))},
                                     {"line", Json(std::uint64_t{1})}, {"column", Json(std::uint64_t{1})}};
                    if (auto Line = Find(Fields, "line")) Out["line"] = Json(Integer(*Line, "line"));
                    if (auto Column = Find(Fields, "column")) Out["column"] = Json(Integer(*Column, "column"));
                    if (auto Module = Find(Fields, "moduleId"); Module && Module->IsString()) {
                        auto Path = SourcePath(Module->String());
                        if (!Path.empty()) Out.emplace("source", Json(SourceObject(std::move(Path))));
                    }
                    Frames.emplace_back(std::move(Out));
                }
                DapOutput.push_back(Response(PendingValue.DapRequest, PendingValue.DapCommand, true,
                    {{"stackFrames", Json(std::move(Frames))}}));
            } else if (PendingValue.Kind == PendingKind::Variables || PendingValue.Kind == PendingKind::Properties) {
                Json::Array Variables;
                auto Values = Find(Result, PendingValue.Kind == PendingKind::Variables ? "variables" : "properties");
                if (Values && Values->IsArray()) for (const auto& Value : Values->Items()) {
                    if (!Value.IsObject()) continue;
                    const auto& Fields = Value.Members();
                    auto Out = DapValue(Fields, PendingValue.FrameId);
                    Out.emplace("name", Json(StringField(Fields, "name")));
                    Variables.emplace_back(std::move(Out));
                }
                DapOutput.push_back(Response(PendingValue.DapRequest, PendingValue.DapCommand, true,
                    {{"variables", Json(std::move(Variables))}}));
            } else if (PendingValue.Kind == PendingKind::Evaluate ||
                       PendingValue.Kind == PendingKind::SetVariable) {
                auto Body = DapValue(Result, PendingValue.FrameId);
                if (PendingValue.Kind == PendingKind::Evaluate) {
                    auto Description = Find(Body, "value");
                    if (Description) {
                        Body.emplace("result", *Description);
                        Body.erase("value");
                    }
                }
                DapOutput.push_back(Response(PendingValue.DapRequest,
                    PendingValue.DapCommand, true, std::move(Body)));
            } else {
                Json::Object Body;
                if (PendingValue.DapCommand == "continue") Body.emplace("allThreadsContinued", Json(true));
                DapOutput.push_back(Response(PendingValue.DapRequest, PendingValue.DapCommand, true, std::move(Body)));
            }
            return;
        }

        auto MethodValue = Find(Input, "method");
        if (!MethodValue || !MethodValue->IsString()) return;
        const auto& Params = ObjectField(Input, "params", true);
        const auto& Method = MethodValue->String();
        if (Method == "Debugger.paused") {
            ResetReferences();
            auto Reason = StringField(Params, "reason");
            if (Reason == "pause") Reason = "pause";
            else if (Reason == "breakpoint") Reason = "breakpoint";
            else if (Reason == "error") Reason = "exception";
            else Reason = "step";
            Json::Object Body{{"reason", Json(std::move(Reason))},
                              {"threadId", Json(std::uint64_t{1})},
                              {"allThreadsStopped", Json(true)}};
            if (auto Error = Find(Params, "error"); Error && Error->IsObject()) {
                if (auto Description = Find(Error->Members(), "description");
                    Description && Description->IsString())
                    Body.emplace("text", Json(Description->String()));
            }
            if (auto ConditionError = Find(Params, "conditionError");
                ConditionError && ConditionError->IsString())
                Body.emplace("description", Json(ConditionError->String()));
            DapOutput.push_back(Event("stopped", std::move(Body)));
        } else if (Method == "Debugger.resumed") {
            ResetReferences();
            DapOutput.push_back(Event("continued", {{"threadId", Json(std::uint64_t{1})},
                                                    {"allThreadsContinued", Json(true)}}));
        } else if (Method == "Debugger.executionFinished") {
            ResetReferences();
            DapOutput.push_back(Event("terminated"));
        } else if (Method == "Debugger.breakpointResolved") {
            auto BreakpointId = IntegerField(Params, "breakpointId");
            Json::Object Breakpoint{{"id", Json(BreakpointId)}, {"verified", Json(true)}};
            if (auto Line = Find(Params, "line")) Breakpoint.emplace("line", Json(Integer(*Line, "line")));
            if (auto Column = Find(Params, "column")) Breakpoint.emplace("column", Json(Integer(*Column, "column")));
            DapOutput.push_back(Event("breakpoint", {{"reason", Json("changed")},
                                                     {"breakpoint", Json(std::move(Breakpoint))}}));
        } else if (Method == "Debugger.protocolError") {
            auto Text = Find(Params, "message");
            DapOutput.push_back(Event("output", {{"category", Json("stderr")},
                {"output", Json((Text && Text->IsString() ? Text->String() : "debug protocol error") + "\n")}}));
        }
    }

    std::shared_ptr<DapChannel> Channel;
    DapAdapterOptions Options;
    std::mutex Mutex;
    bool Closed = false;
    std::uint64_t NextDapSequence = 1;
    std::uint64_t NextTargetId = 1;
    std::uint64_t NextBatch = 1;
    std::uint64_t NextReference = 1;
    std::unordered_map<std::uint64_t, Pending> PendingRequests;
    std::unordered_map<std::uint64_t, BreakpointBatch> Batches;
    std::map<std::string, std::vector<std::uint64_t>> SourceBreakpoints;
    std::unordered_map<std::uint64_t, Reference> References;
};

DapAdapter::DapAdapter(std::shared_ptr<DapChannel> Channel, DapAdapterOptions Options)
    : State(std::make_unique<Impl>(std::move(Channel), std::move(Options))) {}
DapAdapter::~DapAdapter() { Close(); }

void DapAdapter::DispatchDapMessage(std::string_view Message) noexcept {
    std::vector<std::string> DapOutput, TargetOutput;
    try {
        std::lock_guard Lock(State->Mutex);
        State->Dap(Message, DapOutput, TargetOutput);
    } catch (const std::exception& Failure) {
        try {
            std::lock_guard Lock(State->Mutex);
            DapOutput.push_back(State->Event("output", {{"category", Json("stderr")},
                {"output", Json(std::string(Failure.what()) + "\n")}}));
        } catch (...) {}
    }
    for (const auto& Output : TargetOutput) State->Channel->SendTargetMessage(Output);
    for (const auto& Output : DapOutput) State->Channel->SendDapMessage(Output);
}

void DapAdapter::DispatchTargetMessage(std::string_view Message) noexcept {
    std::vector<std::string> DapOutput;
    try {
        std::lock_guard Lock(State->Mutex);
        if (!State->Closed) State->Target(Message, DapOutput);
    } catch (const std::exception& Failure) {
        try {
            std::lock_guard Lock(State->Mutex);
            DapOutput.push_back(State->Event("output", {{"category", Json("stderr")},
                {"output", Json(std::string(Failure.what()) + "\n")}}));
        } catch (...) {}
    }
    for (const auto& Output : DapOutput) State->Channel->SendDapMessage(Output);
}

void DapAdapter::Close() noexcept {
    std::lock_guard Lock(State->Mutex);
    State->Closed = true;
    State->PendingRequests.clear();
    State->Batches.clear();
    State->References.clear();
}

} // namespace Feather
