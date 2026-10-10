#include "Network.hpp"
#include "../modules/time/Time.hpp"
#include <Feather/ByteBuffer.hpp>
#include <Feather/Compiler.hpp>
#include <Feather/StdIo.hpp>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace Feather;
namespace {
void Check(bool Good, const char* Message) { if (!Good) throw std::runtime_error(Message); }
Value Member(const Value& Object, const char* Name) {
    if (Object.IsScriptObject()) return Object.AsScriptObject()->GetRaw(Value::String(Name)).value_or(Value{});
    return dynamic_cast<NativeObject*>(Object.AsObject())->GetMember(Value::String(Name));
}
Value Call(const Value& Module, const char* Name, std::vector<Value> Args = {}) {
    return dynamic_cast<NativeObject*>(Member(Module, Name).AsObject())->Call(Args);
}
Value Good(const Value& Module, const char* Name, std::vector<Value> Args = {}) {
    auto Result = Call(Module, Name, std::move(Args));
    if (Result.IsError()) throw std::runtime_error(std::string(Name) + ": " +
        dynamic_cast<ErrorObject*>(Result.AsObject())->GetMessage());
    return Result;
}
Value N(double X) { return Value::Number(X); }
Value S(const char* X) { return Value::String(X); }
void Ready(const Value& Net, const Value& Socket, bool Read) {
    auto Result = Good(Net, "Poll", {Socket, Value::Bool(Read), Value::Bool(!Read), N(1000)});
    Check(Member(Result, Read ? "Readable" : "Writable").AsBool(), "loopback readiness timed out");
}
struct Pair { Value Listener, Client, Server; };
Pair TcpPair(const Value& Net, const char* Family = "ipv4", const char* Host = "127.0.0.1") {
    Pair P;
    P.Listener = Good(Net, "CreateTcp", {S(Family)});
    Good(Net, "Bind", {P.Listener, S(Host), N(0)});
    auto Port = Member(Good(Net, "LocalAddress", {P.Listener}), "Port");
    Good(Net, "Listen", {P.Listener});
    Check(Good(Net, "Accept", {P.Listener}).GetType() == ValueType::Null, "empty accept did not would-block");
    P.Client = Good(Net, "CreateTcp", {S(Family)});
    auto Connected = Good(Net, "Connect", {P.Client, S(Host), Port});
    if (!Connected.AsBool()) { Ready(Net, P.Client, false); Check(Good(Net, "FinishConnect", {P.Client}).AsBool(), "connect incomplete"); }
    Ready(Net, P.Listener, true);
    P.Server = Good(Net, "Accept", {P.Listener});
    return P;
}
void ClosePair(const Value& Net, Pair& P) {
    for (const auto& Socket : {P.Client, P.Server, P.Listener}) Good(Net, "Close", {Socket});
}
std::string ReceiveBytes(const Value& Net, const Value& Socket, std::size_t Count) {
    std::string Result;
    while (Result.size() < Count) {
        Ready(Net, Socket, true);
        auto Part = Good(Net, "Receive", {Socket, N(static_cast<double>(Count - Result.size()))});
        auto* Bytes = ByteBufferData(Part);
        Check(Bytes != nullptr && !Bytes->empty(), "unexpected TCP EOF");
        Result += *Bytes;
    }
    return Result;
}
class Thrower final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>&) override { throw std::runtime_error("receiver exploded"); }
};
}
int main() {
    try {
        auto Program = Compile(R"(
            var count = 0; var last = null; var depth = 0; var maxDepth = 0;
            def echo(e) {
                count = count + 1; last = e;
                if (e.Kind == "data") { return network.Send(e.Socket, e.Data); }
            }
            def once(e) {
                count = count + 1; last = e;
                network.OnReceive(e.Socket, null);
            }
            def closing(e) { count = count + 1; last = e; network.Close(e.Socket); }
            def replacing(e) {
                count = count + 1; last = e; network.OnReceive(e.Socket, once);
                depth = depth + 1;
                if (maxDepth < depth) { maxDepth = depth; }
                time.Sleep(0.005);
                depth = depth - 1;
            }
            def callable(self, e) { count = count + self.amount; last = e; }
            def bad(e) { return missingReceiverResult; }
            def infinite(e) { while (true) {} }
            def tick() { return time.Sleep(0.02); }
            def zero() { return time.Sleep(0); }
        )");
        Vm Machine(Program.Program);
        NetworkLibrary Library(Machine);
        auto Net = Library.GetModule();
        TimeLibrary Time(Machine);
        Machine.SetGlobal("network", Net);
        Machine.SetGlobal("time", Time.GetModule());
        Program.Initialize(Machine);
        auto Tick = [&] {
            auto Result = Machine.Run(Program.Functions.at("tick"));
            if (Result.IsError()) throw std::runtime_error("sleep callback returned Error");
        };
        auto Binary = Good(Net, "Buffer", {Value::Byte(0), Value::Byte(255), S("ab")});
        Check(*ByteBufferData(Binary) == std::string("\0\xff" "ab", 4), "binary buffer mismatch");
        Check(Good(Net, "Resolve", {S("127.0.0.1")}).AsString() == "127.0.0.1", "numeric resolve");
        Check(Call(Net, "CreateTcp", {S("other")}).IsError(), "invalid family accepted");
        auto Pair = TcpPair(Net);
        Check(Good(Net, "Receive", {Pair.Server}).GetType() == ValueType::Null, "empty receive did not would-block");
        auto Start = std::chrono::steady_clock::now();
        auto Quiet = Good(Net, "Poll", {Pair.Server, Value::Bool(true), Value::Bool(false), N(15)});
        Check(Member(Quiet, "TimedOut").AsBool() &&
              std::chrono::steady_clock::now() - Start >= std::chrono::milliseconds(10), "poll timeout");
        Check(Call(Net, "Poll", {Pair.Server, Value::Bool(true), Value::Bool(false), N(-1)}).IsError(), "negative timeout");
        Check(Call(Net, "Receive", {Pair.Server, N(0)}).IsError(), "zero receive masquerades as EOF");
        Check(Good(Net, "Send", {Pair.Client, Binary, N(1), N(2)}).AsNumber() == 2, "offset send");
        Check(ReceiveBytes(Net, Pair.Server, 2) == std::string("\xff" "a", 2), "partial receive bytes");
        Good(Net, "OnReceive", {Pair.Server, Machine.GetGlobal("echo")});
        Check(Call(Net, "Receive", {Pair.Server}).IsError(), "callback and low-level receive raced");
        Good(Net, "Send", {Pair.Client, Binary});
        Tick();
        Check(Machine.GetGlobal("count").AsNumber() > 0 && Member(Machine.GetGlobal("last"), "Kind").AsString() == "data", "sleep did not dispatch TCP data");
        Check(ReceiveBytes(Net, Pair.Client, 4) == *ByteBufferData(Binary), "TCP callback binary echo");
        Good(Net, "ShutdownWrite", {Pair.Client});
        Tick();
        Check(Member(Machine.GetGlobal("last"), "Kind").AsString() == "eof", "TCP EOF event missing");
        Check(Good(Net, "Send", {Pair.Server, S("q")}).AsNumber() == 1, "half-close forbids reverse send");
        Check(ReceiveBytes(Net, Pair.Client, 1) == "q", "half-close reverse receive");
        ClosePair(Net, Pair);
        Good(Net, "Close", {Pair.Client});
        Check(Call(Net, "Receive", {Pair.Client}).IsError(), "closed socket accepted");

        auto Pressure = TcpPair(Net);
        auto Large = Good(Net, "Buffer", {Value::String(std::string(1024 * 1024, 'x'))});
        bool Backpressure = false;
        for (int I = 0; I < 64; ++I) {
            auto Sent = Good(Net, "Send", {Pressure.Client, Large});
            if (Sent.GetType() == ValueType::Null || Sent.AsNumber() < 1024 * 1024) {
                Backpressure = true; break;
            }
        }
        Check(Backpressure, "TCP short send/would-block not observed");
        ClosePair(Net, Pressure);

        auto U = Good(Net, "CreateUdp");
        auto V = Good(Net, "CreateUdp");
        Good(Net, "Bind", {U, S("127.0.0.1"), N(0)});
        Good(Net, "Bind", {V, S("127.0.0.1"), N(0)});
        auto UP = Member(Good(Net, "LocalAddress", {U}), "Port");
        auto VP = Member(Good(Net, "LocalAddress", {V}), "Port");
        Check(Call(Net, "SendTo", {U, Binary, S("127.0.0.1"), N(65536)}).IsError(), "port overflow accepted");
        Check(Call(Net, "SendTo", {U, Binary, S("not-an-ip"), VP}).IsError(), "hidden DNS allowed");
        Good(Net, "SendTo", {U, S(""), S("127.0.0.1"), VP});
        Ready(Net, V, true);
        auto Empty = Good(Net, "ReceiveFrom", {V, N(0)});
        Check(ByteBufferData(Member(Empty, "Data"))->empty(), "empty UDP datagram lost");
        Good(Net, "SendTo", {U, Binary, S("127.0.0.1"), VP});
        Ready(Net, V, true);
        auto Packet = Good(Net, "ReceiveFrom", {V});
        Check(*ByteBufferData(Member(Packet, "Data")) == *ByteBufferData(Binary) &&
              Member(Packet, "Port").AsNumber() == UP.AsNumber() &&
              Member(Packet, "Address").AsString() == "127.0.0.1", "UDP data/source mismatch");
        Good(Net, "SendTo", {V, Member(Packet, "Data"), Member(Packet, "Address"), Member(Packet, "Port")});
        Ready(Net, U, true);
        Check(*ByteBufferData(Member(Good(Net, "ReceiveFrom", {U}), "Data")) == *ByteBufferData(Binary), "UDP echo");
        Good(Net, "SendTo", {U, S("too long"), S("127.0.0.1"), VP});
        Ready(Net, V, true);
        Check(Call(Net, "ReceiveFrom", {V, N(2)}).IsError(), "truncated datagram returned as complete");
        Check(Good(Net, "ReceiveFrom", {V}).GetType() == ValueType::Null, "truncated packet not consumed");

        Good(Net, "OnReceive", {V, Machine.GetGlobal("replacing")});
        for (int I = 0; I < 2; ++I) Good(Net, "SendTo", {U, Binary, S("127.0.0.1"), VP});
        auto BeforeCount = Machine.GetGlobal("count").AsNumber();
        Ready(Net, V, true);
        Machine.Run(Program.Functions.at("zero"));
        Check(Machine.GetGlobal("count").AsNumber() == BeforeCount + 1, "Sleep(0) dispatched multiple events or nested sleep recursed");
        Tick();
        Check(Machine.GetGlobal("count").AsNumber() == BeforeCount + 2 &&
              Machine.GetGlobal("maxDepth").AsNumber() == 1, "replacement/unregistration failed");
        Check(Good(Net, "ReceiveFrom", {V}).GetType() == ValueType::Null, "unregistration did not release low-level receive");

        auto Callable = Machine.CreateScriptObject();
        auto Meta = Machine.CreateMetaObject();
        Meta->SetRaw(S("__call"), Machine.GetGlobal("callable"));
        Machine.SetMetaObject(Callable, Meta); Callable->SetRaw(S("amount"), N(7));
        Good(Net, "OnReceive", {V, Value::FromScript(Callable)});
        Machine.CollectGarbage(); // Only the receiver's GC-visible field retains this callable graph.
        BeforeCount = Machine.GetGlobal("count").AsNumber();
        Good(Net, "SendTo", {U, Value::Byte(255), S("127.0.0.1"), VP});
        Tick();
        Check(Machine.GetGlobal("count").AsNumber() == BeforeCount + 7, "callback GC root/metacall lost");
        Good(Net, "OnReceive", {V, Machine.GetGlobal("bad")});
        Good(Net, "SendTo", {U, S("bad"), S("127.0.0.1"), VP});
        Check(Machine.Run(Program.Functions.at("tick")).IsError(), "callback Error swallowed by Sleep");
        Good(Net, "OnReceive", {V, Value::FromObject(std::make_shared<Thrower>())});
        Good(Net, "SendTo", {U, S("fault"), S("127.0.0.1"), VP});
        bool Fault = false;
        try { Tick(); } catch (const IdleCallbackFault& E) { Fault = std::string(E.what()) == "receiver exploded"; }
        Check(Fault, "callback native exception swallowed");
        Good(Net, "OnReceive", {V, Machine.GetGlobal("closing")});
        Good(Net, "SendTo", {U, S("close"), S("127.0.0.1"), VP});
        Tick();
        Check(Call(Net, "ReceiveFrom", {V}).IsError(), "close inside callback failed");
        Good(Net, "Close", {U});

        auto RejectSocket = Good(Net, "CreateTcp");
        Good(Net, "Bind", {RejectSocket, S("127.0.0.1"), N(0)});
        auto RefusedPort = Member(Good(Net, "LocalAddress", {RejectSocket}), "Port");
        auto Refused = Good(Net, "CreateTcp");
        auto Connecting = Call(Net, "Connect", {Refused, S("127.0.0.1"), RefusedPort});
        bool RefusalObserved = Connecting.IsError();
        for (int I = 0; I < 5 && !RefusalObserved; ++I) {
            Good(Net, "Poll", {Refused, Value::Bool(false), Value::Bool(true), N(1000)});
            auto Finished = Call(Net, "FinishConnect", {Refused});
            RefusalObserved = Finished.IsError();
            if (!RefusalObserved) Check(!Finished.AsBool(), "refused connect became success");
        }
        Check(RefusalObserved, "loopback refusal was not reported within five seconds");
        Good(Net, "Close", {RejectSocket}); Good(Net, "Close", {Refused});

        Vm BudgetMachine(Program.Program, 1000, 300);
        NetworkLibrary BudgetNetwork(BudgetMachine);
        TimeLibrary BudgetTime(BudgetMachine);
        auto BudgetNet = BudgetNetwork.GetModule();
        BudgetMachine.SetGlobal("network", BudgetNet);
        BudgetMachine.SetGlobal("time", BudgetTime.GetModule());
        Program.Initialize(BudgetMachine);
        auto BudgetReceiver = Good(BudgetNet, "CreateUdp");
        auto BudgetSender = Good(BudgetNet, "CreateUdp");
        Good(BudgetNet, "Bind", {BudgetReceiver, S("127.0.0.1"), N(0)});
        auto BudgetPort = Member(Good(BudgetNet, "LocalAddress", {BudgetReceiver}), "Port");
        Good(BudgetNet, "OnReceive", {BudgetReceiver, BudgetMachine.GetGlobal("infinite")});
        Good(BudgetNet, "SendTo", {BudgetSender, S("budget"), S("127.0.0.1"), BudgetPort});
        Ready(BudgetNet, BudgetReceiver, true);
        bool BudgetFailed = false;
        try { BudgetMachine.Run(Program.Functions.at("tick")); }
        catch (const std::exception& E) {
            BudgetFailed = std::string(E.what()).find("instruction budget") != std::string::npos;
        }
        Check(BudgetFailed, "receiver bypassed outer instruction budget");
        Good(BudgetNet, "Close", {BudgetReceiver}); Good(BudgetNet, "Close", {BudgetSender});

        Vm Limited(std::make_shared<Module>());
        NetworkLibrary Small(Limited, {1, 32});
        auto SmallNet = Small.GetModule();
        auto Only = Good(SmallNet, "CreateUdp");
        Check(Call(SmallNet, "CreateUdp").IsError(), "socket cap ignored");
        Good(SmallNet, "Close", {Only});
        Only = Good(SmallNet, "CreateUdp");
        Check(Call(SmallNet, "Buffer", {Value::String(std::string(33, 'x'))}).IsError(), "buffer cap ignored");
        Good(SmallNet, "Close", {Only});

        // Both stdio construction orders must share the exact same ByteBuffer types.
        std::istringstream Input; std::ostringstream Output;
        StdIoLibrary Io(Machine, Input, Output);
        auto EchoBuffer = Good(Net, "Buffer", {Binary});
        Check(Call(Member(Io.GetModule(), "Console"), "PrintLine", {Good(EchoBuffer, "Get", {N(1)})}).GetType() == ValueType::Null,
              "shared ByteBuffer/stdout interop");
        Check(Output.str() == "255\n", "binary byte print changed");

        // IPv6 support is tested on ::1 only; a missing local IPv6 stack is reported explicitly.
        auto IPv6Probe = Call(Net, "CreateTcp", {S("ipv6")});
        auto IPv6Bind = IPv6Probe.IsError() ? IPv6Probe : Call(Net, "Bind", {IPv6Probe, S("::1"), N(0)});
        if (IPv6Bind.IsError()) {
            if (!IPv6Probe.IsError()) Good(Net, "Close", {IPv6Probe});
            std::cout << "IPv6 loopback unavailable; skipped\n";
        } else {
            Good(Net, "Close", {IPv6Probe});
            auto Six = TcpPair(Net, "ipv6", "::1");
            Good(Net, "Send", {Six.Client, Binary});
            Check(ReceiveBytes(Net, Six.Server, 4) == *ByteBufferData(Binary), "IPv6 binary TCP");
            ClosePair(Net, Six);
        }
        std::cout << "Network loopback tests passed\n";
        return 0;
    } catch (const std::exception& E) { std::cerr << E.what() << '\n'; return 1; }
}
