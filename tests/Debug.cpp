#include <Feather/Compiler.hpp>
#include <Feather/Debug.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace Feather;
using namespace std::chrono_literals;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

std::size_t Count(std::string_view Text, std::string_view Fragment) {
    std::size_t Result = 0;
    for (std::size_t At = 0; (At = Text.find(Fragment, At)) != std::string_view::npos;
         At += Fragment.size()) ++Result;
    return Result;
}

class RecordingChannel final : public DebugChannel {
public:
    void SendProtocolMessage(std::string_view Message) noexcept override {
        try {
            {
                std::lock_guard Lock(Mutex);
                Messages.emplace_back(Message);
            }
            Changed.notify_all();
        } catch (...) {}
    }

    std::string WaitFor(std::string_view Fragment, std::size_t Start = 0) {
        std::unique_lock Lock(Mutex);
        bool Found = Changed.wait_for(Lock, 5s, [&] {
            for (std::size_t I = Start; I < Messages.size(); ++I)
                if (Messages[I].find(Fragment) != std::string::npos) return true;
            return false;
        });
        if (!Found) throw std::runtime_error("timed out waiting for debug message");
        for (std::size_t I = Start; I < Messages.size(); ++I)
            if (Messages[I].find(Fragment) != std::string::npos) return Messages[I];
        throw std::logic_error("debug wait predicate lost its message");
    }

    std::size_t Count() const {
        std::lock_guard Lock(Mutex);
        return Messages.size();
    }
private:
    mutable std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<std::string> Messages;
};

class ThrowingController final : public VmDebugController {
public:
    void OnSafePoint(const VmDebugContext&) override { throw std::runtime_error("debug failure"); }
    void OnError(const VmDebugContext&, const Value&, DebugErrorOrigin) override {}
    void OnExecutionFinished(bool) override {}
};

struct TargetCloser {
    std::shared_ptr<DebugTarget> Target;
    ~TargetCloser() { if (Target) Target->Close(); }
};

std::uint32_t AddFunction(const std::shared_ptr<Module>& Program, Builder Code,
                          std::vector<InstructionLocation> Locations) {
    auto Body = std::make_shared<FunctionPrototype>();
    Body->Code = std::move(Code).Finish();
    Body->Locations = std::move(Locations);
    return Program->AddFunction(std::move(Body));
}

void ProtocolRoundTrip() {
    constexpr std::string_view SourceText =
        "def main() {\n"
        "  var value = object();\n"
        "  value.answer = 40;\n"
        "  var result = value.answer + 2;\n"
        "  return result;\n"
        "}\n";
    auto SourceProgram = Compile(SourceText);
    auto Symbols = SerializeSymbols(SourceProgram);
    auto Program = DeserializeProgram(SerializeProgram(SourceProgram));
    AttachSymbols(Program, Symbols);
    Vm Machine(Program.Program);
    Check(!Program.Initialize(Machine).IsError(), "debug fixture initializer failed");

    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":1,"method":"Debugger.enable"})");
    Check(Channel->WaitFor(R"("id":1)").find(R"("protocolVersion":"1.0")") != std::string::npos,
          "enable response omitted protocol version");
    Target->DispatchProtocolMessage(
        R"({"id":2,"method":"Debugger.setBreakpoint","params":{"moduleId":"","line":4}})");
    Check(Channel->WaitFor(R"("id":2)").find(R"("breakpointId":1)") != std::string::npos,
          "breakpoint response omitted its ID");

    auto Result = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    auto Paused = Channel->WaitFor(R"("method":"Debugger.paused")");
    Check(Paused.find(R"("line":4)") != std::string::npos &&
          Paused.find(R"("reason":"breakpoint")") != std::string::npos,
          "breakpoint stopped at the wrong location");
    Check(Channel->WaitFor(R"("method":"Debugger.breakpointResolved")")
              .find(R"("breakpointId":1)") != std::string::npos,
          "breakpoint was not resolved");

    Target->DispatchProtocolMessage(R"({"id":3,"method":"Debugger.getStackTrace"})");
    auto Stack = Channel->WaitFor(R"("id":3)");
    Check(Stack.find(R"("frames":[)") != std::string::npos &&
          Stack.find(R"("functionName":"main")") != std::string::npos,
          "stack trace response is incomplete");

    Target->DispatchProtocolMessage(
        R"({"id":4,"method":"Debugger.getVariables","params":{"frameId":0,"scope":"locals"}})");
    auto Variables = Channel->WaitFor(R"("id":4)");
    Check(Variables.find(R"("name":"value")") != std::string::npos &&
          Variables.find(R"("name":"result")") == std::string::npos &&
          Variables.find(R"("objectId":1)") != std::string::npos,
          "local object was not exposed through a stop-scoped handle");

    Target->DispatchProtocolMessage(
        R"({"id":5,"method":"Debugger.getProperties","params":{"objectId":1}})");
    auto Properties = Channel->WaitFor(R"("id":5)");
    Check(Properties.find(R"("description":"answer")") != std::string::npos &&
          Properties.find(R"("value":40)") != std::string::npos,
          "ScriptObject properties were not inspected on the VM thread");

    auto BeforeStep = Channel->Count();
    Target->DispatchProtocolMessage(R"({"id":6,"method":"Debugger.stepOver"})");
    Check(Channel->WaitFor(R"("id":6)", BeforeStep).find(R"("result":{})") != std::string::npos,
          "step response missing");
    auto Stepped = Channel->WaitFor(R"("method":"Debugger.paused")", BeforeStep);
    Check(Stepped.find(R"("reason":"step")") != std::string::npos &&
          Stepped.find(R"("stopId":2)") != std::string::npos,
          "step-over did not create a new stop");

    Target->DispatchProtocolMessage(R"({"id":7,"method":"Debugger.resume"})");
    Check(Channel->WaitFor(R"("id":7)").find(R"("result":{})") != std::string::npos,
          "resume response missing");
    Check(Result.wait_for(5s) == std::future_status::ready, "VM did not resume");
    Check(Result.get().AsNumber() == 42, "debugging changed the script result");
    Check(Channel->WaitFor(R"("method":"Debugger.executionFinished")")
              .find(R"("faulted":false)") != std::string::npos,
          "normal execution completion was not reported");

    Target->DispatchProtocolMessage("not json");
    Check(Channel->WaitFor(R"("method":"Debugger.protocolError")")
              .find("InvalidRequest") != std::string::npos,
          "malformed input did not produce a protocol error");
    Machine.SetDebugController({});
}

void ControllerIsolation() {
    auto Program = Compile("def main() { return 7; }");
    Vm Machine(Program.Program);
    Program.Initialize(Machine);
    Machine.SetDebugController(std::make_shared<ThrowingController>());
    Check(Machine.Run(Program.Functions.at("main")).AsNumber() == 7,
          "controller failure changed VM execution");
    Check(!Machine.GetDebugController(), "throwing controller was not detached");
}

void SteppingAcrossCalls() {
    auto Program = std::make_shared<Module>();
    auto Two = Program->AddNumber(2);
    Builder Helper;
    Helper.EmitU32(Op::Const, Two);
    Helper.Emit(Op::Return);
    auto HelperId = AddFunction(Program, std::move(Helper),
        {{0, {0, 2, 1, {}}}, {5, {1, 2, 2, {}}}});
    Builder Caller;
    Caller.EmitU32(Op::Const, HelperId);
    Caller.EmitU16(Op::Call, 0);
    Caller.Emit(Op::Return);
    auto CallerId = AddFunction(Program, std::move(Caller),
        {{0, {2, 5, 1, {}}}, {5, {3, 5, 2, {}}}, {8, {4, 5, 3, {}}}});
    Vm Machine(Program);
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":20,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":20)");
    Target->DispatchProtocolMessage(
        R"({"id":21,"method":"Debugger.setBreakpoint","params":{"moduleId":"","line":5,"column":2}})");
    Channel->WaitFor(R"("id":21)");
    auto Result = std::async(std::launch::async, [&] { return Machine.Run(CallerId); });
    auto First = Channel->WaitFor(R"("method":"Debugger.paused")");
    Check(First.find(R"("pc":5)") != std::string::npos, "call breakpoint resolved to wrong PC");

    auto Start = Channel->Count();
    Target->DispatchProtocolMessage(R"({"id":22,"method":"Debugger.stepInto"})");
    auto Entered = Channel->WaitFor(R"("method":"Debugger.paused")", Start);
    Check(Entered.find(R"("functionId":1)") != std::string::npos &&
          Entered.find(R"("line":2)") != std::string::npos,
          "step-into did not enter the callee");
    Target->DispatchProtocolMessage(R"({"id":23,"method":"Debugger.getStackTrace"})");
    auto Stack = Channel->WaitFor(R"("id":23)");
    Check(Stack.find(R"("frameId":1)") != std::string::npos,
          "callee pause did not expose its caller");

    Start = Channel->Count();
    Target->DispatchProtocolMessage(R"({"id":24,"method":"Debugger.stepOut"})");
    auto Returned = Channel->WaitFor(R"("method":"Debugger.paused")", Start);
    Check(Returned.find(R"("functionId":2)") != std::string::npos &&
          Returned.find(R"("pc":8)") != std::string::npos,
          "step-out did not return to the caller");
    Target->DispatchProtocolMessage(R"({"id":25,"method":"Debugger.resume"})");
    Check(Result.wait_for(5s) == std::future_status::ready && Result.get().AsNumber() == 2,
          "call stepping changed execution");
}

void ExternalPauseAndDisable() {
    auto Program = Compile("def main() { return 9; }");
    Vm Machine(Program.Program);
    Program.Initialize(Machine);
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":30,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":30)");
    Target->DispatchProtocolMessage(R"({"id":31,"method":"Debugger.pause"})");
    Channel->WaitFor(R"("id":31)");
    auto Result = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    auto Paused = Channel->WaitFor(R"("method":"Debugger.paused")");
    Check(Paused.find(R"("reason":"pause")") != std::string::npos,
          "external pause request was not honored at a safe point");
    Target->DispatchProtocolMessage(R"({"id":32,"method":"Debugger.disable"})");
    Check(Channel->WaitFor(R"("id":32)").find(R"("result":{})") != std::string::npos,
          "disable response missing");
    Check(Result.wait_for(5s) == std::future_status::ready && Result.get().AsNumber() == 9,
          "disable did not release the paused VM");

    Target->DispatchProtocolMessage(R"({"id":33,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":33)");
    Target->DispatchProtocolMessage(R"({"id":34,"method":"Debugger.pause"})");
    Channel->WaitFor(R"("id":34)");
    auto Start = Channel->Count();
    auto Second = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    Channel->WaitFor(R"("method":"Debugger.paused")", Start);
    Target->Close();
    Check(Second.wait_for(5s) == std::future_status::ready && Second.get().AsNumber() == 9,
          "closing a disconnected target did not release the paused VM");
    Target->DispatchProtocolMessage(R"({"id":35,"method":"Debugger.enable"})");
    Check(Channel->WaitFor(R"("id":35)").find(R"("code":"Closed")") != std::string::npos,
          "closed target was enabled again");
}

void ShadowedLocals() {
    auto Source = Compile(
        "def main(x) {\n"
        "  {\n"
        "    var x = 2;\n"
        "    x = x + 1;\n"
        "  }\n"
        "  return x;\n"
        "}\n");
    Vm Machine(Source.Program);
    Source.Initialize(Machine);
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":40,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":40)");
    Target->DispatchProtocolMessage(
        R"({"id":41,"method":"Debugger.setBreakpoint","params":{"moduleId":"","line":4}})");
    Channel->WaitFor(R"("id":41)");
    auto Result = std::async(std::launch::async, [&] {
        return Machine.Run(Source.Functions.at("main"), {Value::Number(1)});
    });
    Channel->WaitFor(R"("method":"Debugger.paused")");
    Target->DispatchProtocolMessage(
        R"({"id":42,"method":"Debugger.getVariables","params":{"frameId":0,"scope":"locals"}})");
    auto Variables = Channel->WaitFor(R"("id":42)");
    Check(Count(Variables, R"("name":"x")") == 1 &&
          Variables.find(R"("value":2)") != std::string::npos,
          "shadowed outer local was not hidden");
    Target->DispatchProtocolMessage(R"({"id":43,"method":"Debugger.resume"})");
    Check(Result.wait_for(5s) == std::future_status::ready && Result.get().AsNumber() == 1,
          "shadowed-local inspection changed execution");
}

void StartupGate() {
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel, DebugTargetOptions{
        .WaitForDebugger = true,
    });
    TargetCloser Close{Target};
    auto Waiting = std::async(std::launch::async, [&] {
        return Target->WaitForExecutionPermission();
    });
    Check(Waiting.wait_for(50ms) == std::future_status::timeout,
          "debug startup gate did not wait");
    Target->DispatchProtocolMessage(R"({"id":50,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":50)");
    Target->DispatchProtocolMessage(
        R"({"id":51,"method":"Runtime.runIfWaitingForDebugger"})");
    Check(Channel->WaitFor(R"("id":51)").find(R"("result":{})") != std::string::npos,
          "startup release response missing");
    Check(Waiting.wait_for(5s) == std::future_status::ready && Waiting.get(),
          "startup release did not wake its host");

    auto ClosedTarget = std::make_shared<DebugTarget>(Channel, DebugTargetOptions{
        .WaitForDebugger = true,
    });
    auto ClosedWait = std::async(std::launch::async, [&] {
        return ClosedTarget->WaitForExecutionPermission();
    });
    ClosedTarget->Close();
    Check(ClosedWait.wait_for(5s) == std::future_status::ready && !ClosedWait.get(),
          "closing the target did not cancel startup wait");
}

void SyntheticInitializerDoesNotCaptureLineBreakpoint() {
    auto SourceProgram = Compile("def main() { return 7; }\n");
    auto Symbols = SerializeSymbols(SourceProgram);
    auto Program = DeserializeProgram(SerializeProgram(SourceProgram));
    AttachSymbols(Program, Symbols);
    Vm Machine(Program.Program);
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":60,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":60)");
    Target->DispatchProtocolMessage(
        R"({"id":61,"method":"Debugger.setBreakpoint","params":{"moduleId":"","line":1}})");
    Channel->WaitFor(R"("id":61)");

    auto Initializer = std::async(std::launch::async, [&] { return Program.Initialize(Machine); });
    Check(Initializer.wait_for(5s) == std::future_status::ready && !Initializer.get().IsError(),
          "function binding captured a source breakpoint");

    auto Result = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    auto Paused = Channel->WaitFor(R"("method":"Debugger.paused")");
    Check(Paused.find(R"("functionName":"main")") != std::string::npos,
          "same-line breakpoint did not resolve in the function body");
    Target->DispatchProtocolMessage(R"({"id":62,"method":"Debugger.resume"})");
    Check(Result.wait_for(5s) == std::future_status::ready && Result.get().AsNumber() == 7,
          "same-line breakpoint changed execution");
}

void ErrorBreakpoints() {
    auto Program = Compile(
        "def pass(value) { return value; }\n"
        "def main() { return pass(1 + \"bad\"); }\n");
    Vm Machine(Program.Program);
    Program.Initialize(Machine);
    auto Channel = std::make_shared<RecordingChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel);
    TargetCloser Close{Target};
    Machine.SetDebugController(Target);
    Target->DispatchProtocolMessage(R"({"id":70,"method":"Debugger.enable"})");
    Channel->WaitFor(R"("id":70)");
    auto Unobserved = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    Check(Unobserved.wait_for(5s) == std::future_status::ready && Unobserved.get().IsError(),
          "Error breakpoints were not disabled by default");
    Target->DispatchProtocolMessage(
        R"({"id":71,"method":"Debugger.setPauseOnErrors","params":{"enabled":true}})");
    Check(Channel->WaitFor(R"("id":71)").find(R"("result":{})") != std::string::npos,
          "Error breakpoint enable response missing");

    auto Result = std::async(std::launch::async, [&] {
        return Machine.Run(Program.Functions.at("main"));
    });
    auto Operation = Channel->WaitFor(R"("method":"Debugger.paused")");
    Check(Operation.find(R"("reason":"error")") != std::string::npos &&
          Operation.find(R"("origin":"operation")") != std::string::npos &&
          Operation.find("Error: invalid operands") != std::string::npos &&
          Operation.find(R"("objectId":1)") != std::string::npos &&
          Operation.find(R"("line":2)") != std::string::npos,
          "operator Error did not create a typed stop at its source");
    Target->DispatchProtocolMessage(
        R"({"id":72,"method":"Debugger.getProperties","params":{"objectId":1}})");
    Check(Channel->WaitFor(R"("id":72)").find("invalid operands") != std::string::npos,
          "paused Error could not be inspected through its object ID");

    auto Start = Channel->Count();
    Target->DispatchProtocolMessage(R"({"id":73,"method":"Debugger.resume"})");
    auto Returned = Channel->WaitFor(R"("method":"Debugger.paused")", Start);
    Check(Returned.find(R"("origin":"functionReturn")") != std::string::npos &&
          Returned.find(R"("functionName":"pass")") != std::string::npos,
          "function Error return did not create a typed stop");

    Target->DispatchProtocolMessage(
        R"({"id":74,"method":"Debugger.setPauseOnErrors","params":{"enabled":false}})");
    Channel->WaitFor(R"("id":74)");
    Target->DispatchProtocolMessage(R"({"id":75,"method":"Debugger.resume"})");
    Check(Result.wait_for(5s) == std::future_status::ready && Result.get().IsError(),
          "Error breakpoints changed the function result");
}
}

int main() {
    try {
        ProtocolRoundTrip();
        SteppingAcrossCalls();
        ExternalPauseAndDisable();
        ShadowedLocals();
        StartupGate();
        SyntheticInitializerDoesNotCaptureLineBreakpoint();
        ErrorBreakpoints();
        ControllerIsolation();
        std::cout << "Debug protocol tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Debug protocol tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
