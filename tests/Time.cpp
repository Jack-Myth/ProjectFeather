#include "Time.hpp"
#include <Feather/Compiler.hpp>
#include <Feather/Import.hpp>

#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

class Host final : public TimeHost {
public:
    double Monotonic() override { return Now; }
    double UnixTime() override { return Wall; }
    void Sleep(double Seconds) override { Now += Seconds; }
    void SleepUntil(double Deadline) override { Now = std::max(Now, Deadline); }
    double Now = 10;
    double Wall = 1000;
};

Value Call(const Value& Module, const char* Name, std::vector<Value> Arguments = {}) {
    auto Native = dynamic_cast<NativeObject*>(Module.AsObject());
    auto Function = Native->GetMember(Value::String(Name));
    return dynamic_cast<NativeObject*>(Function.AsObject())->Call(Arguments);
}
} // namespace

int main() {
    try {
        auto Program = Compile("def main() { return null; }");
        Vm Machine(Program.Program);
        auto Fake = std::make_shared<Host>();
        TimeLibrary Library(Machine, Fake);
        auto Time = Library.GetModule();

        Check(Call(Time, "Monotonic").AsNumber() == 10, "monotonic time mismatch");
        Check(Call(Time, "UnixTime").AsNumber() == 1000, "Unix time mismatch");
        auto Clock = Call(Time, "CreateClock");
        Check(!Call(Time, "Sleep", {Value::Number(0.25)}).IsError(), "sleep failed");
        Check(std::abs(Call(Time, "Tick", {Clock}).AsNumber() - 0.25) < 1e-9,
              "clock tick mismatch");
        Check(!Call(Time, "SleepUntil", {Value::Number(12)}).IsError() && Fake->Now == 12,
              "sleep until mismatch");
        Check(!Call(Time, "Reset", {Clock}).IsError(), "clock reset failed");
        Fake->Now += 0.5;
        Check(std::abs(Call(Time, "Tick", {Clock}).AsNumber() - 0.5) < 1e-9,
              "clock reset baseline mismatch");
        Check(Call(Time, "Sleep", {Value::Number(-1)}).IsError(),
              "negative sleep was accepted");
        Check(Call(Time, "Tick", {Value{}}).IsError(), "invalid Clock was accepted");

        std::cout << "Time tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Time tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
