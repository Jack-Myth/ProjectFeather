#include <Feather/Runtime.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace Feather;
namespace {
void Check(bool Good, const char* Message) { if (!Good) throw std::runtime_error(Message); }
}
int main() {
    try {
        Vm Machine(std::make_shared<Module>());
        for (int I = 0; I < 2048; ++I) {
            auto Temporary = Machine.RegisterIdleHook([](double) { return Value::Bool(false); });
        }
        double Now = 0;
        int Waits = 0, A = 0, B = 0;
        auto Clock = [&] { return Now; };
        auto Wait = [&](double Seconds) { Check(Seconds > 0, "idle busy wait"); ++Waits; Now += Seconds; };
        auto First = Machine.RegisterIdleHook([&](double) { ++A; Now += 0.02; return Value::Bool(true); });
        auto Second = Machine.RegisterIdleHook([&](double) { ++B; return Value::Bool(false); });
        Machine.Idle(0.01, Clock, Wait);
        Check(A == 1 && B == 0 && Waits == 0, "deadline did not include hook time");
        Machine.Idle(0, Clock, Wait);
        Check(B == 1 && A == 1, "zero-time round robin starved next hook");
        First.reset(); Second.reset();
        Machine.Idle(0.1, Clock, Wait);
        Check(Waits == 1, "no-hook idle should block once");

        std::shared_ptr<IdleRegistration> Self;
        int Calls = 0;
        Self = Machine.RegisterIdleHook([&](double) { ++Calls; Self.reset(); return Value::Bool(true); });
        Machine.Idle(0.02, Clock, Wait);
        Check(Calls == 1, "self-unregistered service ran again");
        int Nested = 0;
        auto NestedHook = Machine.RegisterIdleHook([&](double) {
            ++Nested; Machine.Idle(0.02, Clock, Wait); return Value::Bool(true);
        });
        auto Before = Now;
        Machine.Idle(0.01, Clock, Wait);
        Check(Nested == 1 && Now >= Before + 0.02, "nested idle recursed or lost duration");
        NestedHook.reset();
        auto Failure = Value::FromObject(std::make_shared<ErrorObject>("callback error"));
        auto Bad = Machine.RegisterIdleHook([&](double) { return Failure; });
        Check(Machine.Idle(1, Clock, Wait).AsObject() == Failure.AsObject(), "idle Error identity lost");
        Bad.reset();
        auto Throwing = Machine.RegisterIdleHook([](double) -> Value { throw std::runtime_error("callback fault"); });
        bool Threw = false;
        try { Machine.Idle(0); } catch (const IdleCallbackFault& E) {
            Threw = std::string(E.what()) == "callback fault";
        }
        Check(Threw, "idle native fault swallowed");
        Throwing.reset();
        auto Busy = Machine.RegisterIdleHook([&](double) { ++Calls; return Value::Bool(true); });
        Calls = 0;
        Machine.Idle(1, Clock, Wait);
        Check(Calls == 256, "per-idle dispatch bound missing");
        Busy.reset();
        Calls = 0;
        auto Quiet = Machine.RegisterIdleHook([&](double) { ++Calls; return Value::Bool(false); });
        auto Start = std::chrono::steady_clock::now();
        Machine.Idle(0.04);
        auto Elapsed = std::chrono::steady_clock::now() - Start;
        Check(Elapsed >= std::chrono::milliseconds(30) && Calls < 30, "quiet idle spins or returns early");
        Vm Other(std::make_shared<Module>());
        auto Count = Calls;
        Other.Idle(0);
        Check(Calls == Count, "idle service leaked across VMs");
        std::cout << "Idle tests passed\n";
        return 0;
    } catch (const std::exception& E) { std::cerr << E.what() << '\n'; return 1; }
}
