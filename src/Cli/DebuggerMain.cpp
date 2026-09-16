#include "Socket.hpp"
#include "../Protocol/Json.hpp"

#include <atomic>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using Feather::Protocol::EncodeJson;
using Feather::Protocol::Find;
using Feather::Protocol::Integer;
using Feather::Protocol::Json;

constexpr std::size_t MaxDebugMessageBytes = 1024 * 1024;

void PrintHelp() {
    std::cout <<
        "commands:\n"
        "  run                         start the waiting program\n"
        "  break <line> [module]       set a source breakpoint\n"
        "  delete <breakpoint-id>      remove a breakpoint\n"
        "  errors on | off             pause on Error results\n"
        "  continue | c                resume execution\n"
        "  pause                       pause at the next safe point\n"
        "  step | s                    step into\n"
        "  next | n                    step over\n"
        "  out                         step out\n"
        "  stack                       show the call stack\n"
        "  locals <frame-id>           show locals\n"
        "  values <frame-id>           show the operand stack\n"
        "  globals <frame-id>          show module globals\n"
        "  properties <object-id>      show object properties\n"
        "  raw <json>                  send a raw Feather request\n"
        "  help                        show this help\n"
        "  quit                        disconnect\n";
}

std::optional<std::uint64_t> Number(std::string_view Text) {
    std::uint64_t Value = 0;
    auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    if (Result.ec != std::errc{} || Result.ptr != Text.data() + Text.size()) return std::nullopt;
    return Value;
}

std::string Request(std::uint64_t Id, std::string Method, Json::Object Params = {}) {
    Json::Object Value{{"id", Json(Id)}, {"method", Json(std::move(Method))}};
    if (!Params.empty()) Value.emplace("params", Json(std::move(Params)));
    return EncodeJson(Json(std::move(Value)));
}

void PrintMessage(std::string_view Message) {
    try {
        auto Parsed = Feather::Protocol::ParseJson(Message);
        if (!Parsed.IsObject()) throw std::runtime_error("message is not an object");
        const auto& Object = Parsed.Members();
        if (auto Method = Find(Object, "method"); Method && Method->IsString()) {
            const auto* Params = Find(Object, "params");
            if (Method->String() == "Debugger.paused" && Params && Params->IsObject()) {
                const auto& Fields = Params->Members();
                std::cout << "paused";
                if (auto Reason = Find(Fields, "reason"); Reason && Reason->IsString())
                    std::cout << " (" << Reason->String() << ")";
                if (auto Module = Find(Fields, "moduleId"); Module && Module->IsString() && !Module->String().empty())
                    std::cout << " at " << Module->String();
                if (auto Line = Find(Fields, "line")) std::cout << ':' << Integer(*Line, "line");
                if (auto Error = Find(Fields, "error"); Error && Error->IsObject()) {
                    if (auto Description = Find(Error->Members(), "description");
                        Description && Description->IsString())
                        std::cout << " - " << Description->String();
                }
                std::cout << '\n';
                return;
            }
            if (Method->String() == "Debugger.resumed") {
                std::cout << "resumed\n";
                return;
            }
            if (Method->String() == "Debugger.executionFinished") {
                std::cout << "execution finished";
                if (Params && Params->IsObject()) {
                    if (auto Faulted = Find(Params->Members(), "faulted"); Faulted && Faulted->IsBool() && Faulted->Bool())
                        std::cout << " (faulted)";
                }
                std::cout << '\n';
                return;
            }
            if (Method->String() == "Debugger.breakpointResolved") {
                std::cout << "breakpoint resolved: " << Message << '\n';
                return;
            }
            std::cout << "event: " << Message << '\n';
            return;
        }
        if (auto Id = Find(Object, "id")) {
            std::cout << "response " << Integer(*Id, "id") << ": ";
            if (auto Error = Find(Object, "error")) std::cout << "error " << EncodeJson(*Error);
            else if (auto Result = Find(Object, "result")) std::cout << EncodeJson(*Result);
            else std::cout << Message;
            std::cout << '\n';
            return;
        }
        std::cout << "message: " << Message << '\n';
    } catch (const std::exception& Failure) {
        std::cout << "invalid target message (" << Failure.what() << "): " << Message << '\n';
    }
}

int Run(std::string_view EndpointText) {
    auto Connection = std::make_shared<Feather::Cli::TcpConnection>(
        Feather::Cli::TcpConnection::Connect(Feather::Cli::ParseTcpEndpoint(EndpointText)));
    std::atomic_bool Connected = true;
    std::mutex OutputMutex;
    std::jthread Receiver([&] {
        try {
            while (auto Message = Connection->ReceiveMessage(MaxDebugMessageBytes)) {
                std::lock_guard Lock(OutputMutex);
                std::cout << "\n";
                PrintMessage(*Message);
                std::cout << "debug> " << std::flush;
            }
        } catch (const std::exception& Failure) {
            if (Connected.exchange(false)) {
                std::lock_guard Lock(OutputMutex);
                std::cout << "\ndebug connection failed: " << Failure.what() << '\n';
            }
        }
        Connected = false;
    });

    std::uint64_t NextId = 1;
    auto Send = [&](std::string Method, Json::Object Params = {}) {
        Connection->SendMessage(Request(NextId++, std::move(Method), std::move(Params)));
    };
    Send("Debugger.enable");
    PrintHelp();

    std::string Line;
    while (Connected) {
        {
            std::lock_guard Lock(OutputMutex);
            std::cout << "debug> " << std::flush;
        }
        if (!std::getline(std::cin, Line)) break;
        std::istringstream Input(Line);
        std::string Command;
        Input >> Command;
        if (Command.empty()) continue;
        try {
            if (Command == "quit" || Command == "q") break;
            if (Command == "help" || Command == "h") { PrintHelp(); continue; }
            if (Command == "run") { Send("Runtime.runIfWaitingForDebugger"); continue; }
            if (Command == "continue" || Command == "c") { Send("Debugger.resume"); continue; }
            if (Command == "pause") { Send("Debugger.pause"); continue; }
            if (Command == "step" || Command == "s") { Send("Debugger.stepInto"); continue; }
            if (Command == "next" || Command == "n") { Send("Debugger.stepOver"); continue; }
            if (Command == "out") { Send("Debugger.stepOut"); continue; }
            if (Command == "stack") { Send("Debugger.getStackTrace"); continue; }
            if (Command == "errors") {
                std::string Setting;
                Input >> Setting;
                if (Setting != "on" && Setting != "off")
                    throw std::invalid_argument("errors requires 'on' or 'off'");
                Send("Debugger.setPauseOnErrors", {{"enabled", Json(Setting == "on")}});
                continue;
            }
            if (Command == "break") {
                std::string LineText;
                Input >> LineText;
                auto LineNumber = Number(LineText);
                if (!LineNumber || *LineNumber == 0) throw std::invalid_argument("line must be a positive integer");
                std::string Module;
                std::getline(Input >> std::ws, Module);
                Send("Debugger.setBreakpoint", {{"moduleId", Json(std::move(Module))},
                                                   {"line", Json(*LineNumber)}});
                continue;
            }
            if (Command == "delete" || Command == "properties") {
                std::string IdText;
                Input >> IdText;
                auto Id = Number(IdText);
                if (!Id) throw std::invalid_argument("an integer id is required");
                if (Command == "delete")
                    Send("Debugger.removeBreakpoint", {{"breakpointId", Json(*Id)}});
                else Send("Debugger.getProperties", {{"objectId", Json(*Id)}});
                continue;
            }
            if (Command == "locals" || Command == "values" || Command == "globals") {
                std::string FrameText;
                Input >> FrameText;
                auto Frame = Number(FrameText);
                if (!Frame) throw std::invalid_argument("a frame id is required");
                auto Scope = Command == "values" ? "stack" : Command;
                Send("Debugger.getVariables", {{"frameId", Json(*Frame)}, {"scope", Json(Scope)}});
                continue;
            }
            if (Command == "raw") {
                std::string JsonText;
                std::getline(Input >> std::ws, JsonText);
                if (JsonText.empty()) throw std::invalid_argument("raw requires one JSON object");
                Connection->SendMessage(JsonText);
                continue;
            }
            std::cout << "unknown command; type 'help'\n";
        } catch (const std::exception& Failure) {
            std::cout << "command error: " << Failure.what() << '\n';
        }
    }

    if (Connected) {
        try { Send("Debugger.disable"); } catch (...) {}
    }
    Connected = false;
    Connection->Close();
    Receiver.join();
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    if (ArgCount != 3 || std::wstring_view(Arguments[1]) != L"--connect") {
        std::cerr << "usage: feather-debugger --connect <host:port>\n";
        return 2;
    }
    std::filesystem::path EndpointPath(Arguments[2]);
    auto Utf8 = EndpointPath.u8string();
    std::string Endpoint(Utf8.begin(), Utf8.end());
#else
int main(int ArgCount, char** Arguments) {
    if (ArgCount != 3 || std::string_view(Arguments[1]) != "--connect") {
        std::cerr << "usage: feather-debugger --connect <host:port>\n";
        return 2;
    }
    std::string Endpoint(Arguments[2]);
#endif
    try { return Run(Endpoint); }
    catch (const std::exception& Failure) {
        std::cerr << "feather-debugger: " << Failure.what() << '\n';
        return 1;
    }
}
