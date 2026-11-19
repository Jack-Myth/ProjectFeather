#include "Time.hpp"
#include <Feather/NativeModule.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Feather {
namespace {


double Duration(const Value& Input, const char* Call) {
    if (!Input.IsNumeric() || !std::isfinite(Input.AsNumber()) ||
        Input.AsNumber() < 0)
        throw std::invalid_argument(std::string(Call) + " expects a finite non-negative number of seconds");
    return Input.AsNumber();
}

class Clock final : public NativeObject {
public:
    Clock(NativeObjectType& Type, std::shared_ptr<TimeHost> Host)
        : NativeObject(Type), Host(std::move(Host)) { Reset(); }
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    double Tick() {
        auto Now = Host->Monotonic();
        auto Result = std::max(0.0, Now - Last);
        Last = Now;
        return Result;
    }
    void Reset() { Last = Host->Monotonic(); }
private:
    std::shared_ptr<TimeHost> Host;
    double Last = 0;
};

class ClockType final : public NativeObjectType {
public:
    ClockType(Vm& Machine, std::shared_ptr<TimeHost> Host)
        : NativeObjectType(Machine, TimeModuleGuid, "Clock"), Host(std::move(Host)) {}
    std::shared_ptr<Clock> Create() { return CreateObject<Clock>(Host); }

private:
    std::shared_ptr<TimeHost> Host;
};

Clock& ClockArgument(const Value& Input, const ClockType& ExpectedType) {
    if (Input.GetType() != ValueType::Object || Input.IsScriptObject())
        throw std::invalid_argument("expected Clock");
    auto* Result = dynamic_cast<Clock*>(Input.AsObject());
    if (!Result || Result->GetNativeObjectType() != &ExpectedType)
        throw std::invalid_argument("expected Clock from this time module");
    return *Result;
}

class SystemTimeHost final : public TimeHost {
public:
    double Monotonic() override {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    double UnixTime() override {
        return std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void Sleep(double Seconds) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(Seconds));
    }
    void SleepUntil(double Deadline) override {
        auto Remaining = Deadline - Monotonic();
        if (Remaining > 0) Sleep(Remaining);
    }
};

} // namespace

std::shared_ptr<TimeHost> CreateSystemTimeHost() {
    return std::make_shared<SystemTimeHost>();
}

TimeLibrary::TimeLibrary(Vm& Machine)
    : TimeLibrary(Machine, CreateSystemTimeHost()) {}

TimeLibrary::TimeLibrary(Vm& Machine, std::shared_ptr<TimeHost> InputHost)
    : Host(std::move(InputHost)) {
    if (!Host) throw std::invalid_argument("time requires a host clock");
    auto& Clocks = Machine.CreateNativeObjectType<ClockType>(Host);
    NativeModuleBuilder Module(Machine, TimeModuleGuid, "time");

    Module.AddFunction("Monotonic", [Clock = Host](const std::vector<Value>& A) {
        if (!A.empty()) return Value::Error("Monotonic expects no arguments");
        return Value::Number(Clock->Monotonic());
    });
    Module.AddFunction("UnixTime", [Clock = Host](const std::vector<Value>& A) {
        if (!A.empty()) return Value::Error("UnixTime expects no arguments");
        return Value::Number(Clock->UnixTime());
    });
    Module.AddFunction("Sleep", [Clock = Host, &Machine](const std::vector<Value>& A) {
        if (A.size() != 1) return Value::Error("Sleep expects one duration in seconds");
        return Machine.Idle(Duration(A[0], "Sleep"), [Clock] { return Clock->Monotonic(); },
            [Clock](double Seconds) { Clock->Sleep(Seconds); });
    });
    Module.AddFunction("SleepUntil", [Clock = Host, &Machine](const std::vector<Value>& A) {
        if (A.size() != 1) return Value::Error("SleepUntil expects one monotonic deadline");
        return Machine.Idle(std::max(0.0, Duration(A[0], "SleepUntil") - Clock->Monotonic()),
            [Clock] { return Clock->Monotonic(); }, [Clock](double Seconds) { Clock->Sleep(Seconds); });
    });
    Module.AddFunction("CreateClock", [&Clocks](const std::vector<Value>& A) {
        if (!A.empty()) return Value::Error("CreateClock expects no arguments");
        return Value::FromObject(Clocks.Create());
    });
    Module.AddFunction("Tick", [&Clocks](const std::vector<Value>& A) {
        if (A.size() != 1) return Value::Error("Tick expects one Clock");
        return Value::Number(ClockArgument(A[0], Clocks).Tick());
    });
    Module.AddFunction("Reset", [&Clocks](const std::vector<Value>& A) {
        if (A.size() != 1) return Value::Error("Reset expects one Clock");
        ClockArgument(A[0], Clocks).Reset();
        return Value{};
    });
    ModuleObject = Module.Build();
}

TimeLibrary::~TimeLibrary() = default;
Value TimeLibrary::GetModule() const { return ModuleObject; }

} // namespace Feather
