#include "Time.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

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
        return Found == Members.end() ? Error("unknown time member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};

double Duration(const Value& Input, const char* Call) {
    if (Input.GetType() != ValueType::Number || !std::isfinite(Input.AsNumber()) ||
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
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || !Payload.empty())
            throw std::invalid_argument("invalid time Clock payload");
        return Create();
    }
    void FinalizeRestore(NativeObject& Input) override {
        auto* Restored = dynamic_cast<Clock*>(&Input);
        if (!Restored || Restored->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid time Clock object");
        Restored->Reset();
    }
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
    auto& ModuleType = Machine.CreateNativeObjectType<NativeSingletonType>(
        TimeModuleGuid, "Module");
    auto Module = ModuleType.Create<Namespace>();
    auto Add = [&](std::string Name, Method Body) {
        auto TypeName = Name;
        auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
            TimeModuleGuid, std::move(TypeName));
        Module->Add(std::move(Name), Value::FromObject(Type.Create<Function>(std::move(Body))));
    };

    Add("Monotonic", [Clock = Host](const std::vector<Value>& A) {
        if (!A.empty()) return Error("Monotonic expects no arguments");
        return Value::Number(Clock->Monotonic());
    });
    Add("UnixTime", [Clock = Host](const std::vector<Value>& A) {
        if (!A.empty()) return Error("UnixTime expects no arguments");
        return Value::Number(Clock->UnixTime());
    });
    Add("Sleep", [Clock = Host](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Sleep expects one duration in seconds");
        Clock->Sleep(Duration(A[0], "Sleep"));
        return Value{};
    });
    Add("SleepUntil", [Clock = Host](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("SleepUntil expects one monotonic deadline");
        Clock->SleepUntil(Duration(A[0], "SleepUntil"));
        return Value{};
    });
    Add("CreateClock", [&Clocks](const std::vector<Value>& A) {
        if (!A.empty()) return Error("CreateClock expects no arguments");
        return Value::FromObject(Clocks.Create());
    });
    Add("Tick", [&Clocks](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Tick expects one Clock");
        return Value::Number(ClockArgument(A[0], Clocks).Tick());
    });
    Add("Reset", [&Clocks](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Reset expects one Clock");
        ClockArgument(A[0], Clocks).Reset();
        return Value{};
    });
    ModuleObject = std::move(Module);
}

TimeLibrary::~TimeLibrary() = default;
Value TimeLibrary::GetModule() const { return Value::FromObject(ModuleObject); }

} // namespace Feather
