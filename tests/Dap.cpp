#include <Feather/Dap.hpp>

#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace Feather;

namespace {

void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}

class RecordingChannel final : public DapChannel {
public:
    void SendDapMessage(std::string_view Message) noexcept override {
        std::lock_guard Lock(Mutex);
        Dap.emplace_back(Message);
    }
    void SendTargetMessage(std::string_view Message) noexcept override {
        std::lock_guard Lock(Mutex);
        Target.emplace_back(Message);
    }
    bool DapContains(std::string_view Text) const {
        std::lock_guard Lock(Mutex);
        for (const auto& Message : Dap) if (Message.find(Text) != std::string::npos) return true;
        return false;
    }
    bool TargetContains(std::string_view Text) const {
        std::lock_guard Lock(Mutex);
        for (const auto& Message : Target) if (Message.find(Text) != std::string::npos) return true;
        return false;
    }

private:
    mutable std::mutex Mutex;
    std::vector<std::string> Dap;
    std::vector<std::string> Target;
};

void Run() {
    auto Channel = std::make_shared<RecordingChannel>();
    DapAdapter Adapter(Channel, {.PrimarySourcePath = "main.fe"});

    Adapter.DispatchDapMessage(R"({"seq":1,"type":"request","command":"initialize"})");
    Check(Channel->DapContains(R"("supportsConfigurationDoneRequest":true)"),
          "initialize capabilities missing");

    Adapter.DispatchDapMessage(R"({"seq":2,"type":"request","command":"attach"})");
    Check(Channel->TargetContains(R"("method":"Debugger.enable")"), "attach did not enable target");
    Adapter.DispatchTargetMessage(R"({"id":1,"result":{}})");
    Check(Channel->DapContains(R"("event":"initialized")"), "initialized event missing");

    Adapter.DispatchDapMessage(
        R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"main.fe"},"breakpoints":[{"line":4}]}})");
    Check(Channel->TargetContains(R"("moduleId":"")"), "primary source was not mapped to root module");
    Adapter.DispatchTargetMessage(R"({"id":2,"result":{"breakpointId":7}})");
    Check(Channel->DapContains(R"("command":"setBreakpoints")"), "setBreakpoints response missing");

    Adapter.DispatchTargetMessage(
        R"({"method":"Debugger.breakpointResolved","params":{"breakpointId":7,"moduleId":"","line":4,"column":1}})");
    Check(Channel->DapContains(R"("event":"breakpoint")"), "breakpoint changed event missing");
    Adapter.DispatchTargetMessage(
        R"({"method":"Debugger.paused","params":{"stopId":1,"reason":"breakpoint"}})");
    Check(Channel->DapContains(R"("event":"stopped")"), "stopped event missing");

    Adapter.DispatchDapMessage(R"({"seq":4,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
    Adapter.DispatchTargetMessage(
        R"({"id":3,"result":{"frames":[{"frameId":0,"functionId":1,"functionName":"main","pc":2,"moduleId":"","line":4,"column":2}]}})");
    Check(Channel->DapContains(R"("stackFrames")"), "stack trace was not translated");
    Check(Channel->DapContains(R"("path":"main.fe")"), "primary source path was not restored");

    Adapter.DispatchDapMessage(R"({"seq":5,"type":"request","command":"scopes","arguments":{"frameId":0}})");
    Check(Channel->DapContains(R"("name":"Locals")"), "locals scope missing");
    Adapter.DispatchDapMessage(R"({"seq":6,"type":"request","command":"variables","arguments":{"variablesReference":1}})");
    Adapter.DispatchTargetMessage(
        R"({"id":4,"result":{"variables":[{"name":"value","type":"object","description":"Object","objectId":9}]}})");
    Check(Channel->DapContains(R"("name":"value")"), "variable was not translated");
    Check(Channel->DapContains(R"("variablesReference":4)"), "object reference missing");

    Adapter.DispatchDapMessage(R"({"seq":7,"type":"request","command":"variables","arguments":{"variablesReference":4}})");
    Adapter.DispatchTargetMessage(
        R"({"id":5,"result":{"properties":[{"name":"answer","type":"number","description":"42","value":42}]}})");
    Check(Channel->DapContains(R"("name":"answer")"), "object property was not translated");

    Adapter.DispatchDapMessage(R"({"seq":8,"type":"request","command":"continue","arguments":{"threadId":1}})");
    Adapter.DispatchTargetMessage(R"({"id":6,"result":{}})");
    Adapter.DispatchTargetMessage(R"({"method":"Debugger.resumed","params":{"stopId":1}})");
    Check(Channel->DapContains(R"("event":"continued")"), "continued event missing");

    Adapter.DispatchTargetMessage(R"({"method":"Debugger.executionFinished","params":{"faulted":false}})");
    Check(Channel->DapContains(R"("event":"terminated")"), "terminated event missing");

    Adapter.DispatchDapMessage(
        R"({"seq":9,"type":"request","command":"setExceptionBreakpoints","arguments":{"filters":["error"]}})");
    Check(Channel->TargetContains(R"("method":"Debugger.setPauseOnErrors")") &&
          Channel->TargetContains(R"("enabled":true)"),
          "DAP Error filter was not forwarded to the target");
    Adapter.DispatchTargetMessage(R"({"id":7,"result":{}})");
    Check(Channel->DapContains(R"("command":"setExceptionBreakpoints")"),
          "DAP Error filter response missing");
    Adapter.DispatchTargetMessage(
        R"({"method":"Debugger.paused","params":{"stopId":2,"reason":"error","origin":"operation","error":{"type":"object","description":"Error: invalid operands","objectId":1}}})");
    Check(Channel->DapContains(R"("reason":"exception")") &&
          Channel->DapContains(R"("text":"Error: invalid operands")"),
          "Error stop was not mapped to a DAP exception stop");
}

} // namespace

int main() {
    try {
        Run();
        std::cout << "DAP adapter tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "DAP adapter tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
