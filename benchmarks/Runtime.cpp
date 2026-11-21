#include <Feather/Compiler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace Feather;

namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

// Each workload returns the duration of its measured region. Compilation,
// module initialization and construction of the GC backlog happen outside it.
template<class Sample>
void Report(const char* Name, Sample Run) {
    Run(); // Warm up.
    std::array<double, 7> Times;
    for (auto& Time : Times) Time = Run().count();
    std::sort(Times.begin(), Times.end());
    std::cout << Name << ',' << Times[Times.size() / 2] << ',' << Times.back() << '\n';
}

void MeasureFunction(const char* Name, Vm& Machine, std::uint32_t Entry, double Expected) {
    Report(Name, [&] {
        auto Start = Clock::now();
        auto Result = Machine.Run(Entry);
        Milliseconds Elapsed = Clock::now() - Start;
        if (Result.AsNumber() != Expected) throw std::runtime_error(Name);
        return Elapsed;
    });
}

class Temporary final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
};
}

int main() {
    try {
        auto Program = Compile(R"(
            export def arithmetic() {
                var i = 0;
                while (i < 500000) { i = i + 1; }
                return i;
            }
            def members() {
                var item = object();
                item.width = 0;
                var i = 0;
                while (i < 200000) { item.width = item.width + 1; i = i + 1; }
                return item.width;
            }
            def module_loop() { return child.arithmetic(); }
        )");
        Vm Machine(Program.Program);
        Program.LoadInto(Machine, "child");
        if (Program.InitializeModule(Machine, "child").IsError() || Program.Initialize(Machine).IsError())
            throw std::runtime_error("benchmark initialization failed");
        Machine.SetGlobal("child", Machine.GetModuleNamespace("child"));

        std::cout << std::fixed << std::setprecision(3) << "workload,median_ms,max_ms\n";
        for (int Count : {16, 256, 2048}) {
            std::string Source;
            for (int I = 0; I < Count; ++I)
                Source += "def f" + std::to_string(I) + "(x){return x * 2 + 1;}\n";
            auto Image = DeserializeProgram(SerializeProgram(Compile(Source)));
            auto Name = "module-load-" + std::to_string(Count);
            Report(Name.c_str(), [&] {
                auto Start = Clock::now();
                Vm Loaded(Image.Program);
                return Milliseconds(Clock::now() - Start);
            });
        }
        MeasureFunction("local-arithmetic", Machine, Program.Functions.at("arithmetic"), 500000);
        MeasureFunction("module-arithmetic", Machine, Program.Functions.at("module_loop"), 500000);
        MeasureFunction("member-read-write", Machine, Program.Functions.at("members"), 200000);
        Machine.CollectGarbage();
        Report("gc-expired-native", [&] {
            for (int I = 0; I < 32768; ++I)
                Machine.RegisterNativeObject(std::make_shared<Temporary>());
            // A live registered object must still be traced past the expired entries.
            auto Keeper = std::make_shared<Temporary>();
            Machine.RegisterNativeObject(Keeper);
            Keeper->SetGcVisibleMember("held", Value::FromScript(Machine.CreateScriptObject()));
            Machine.CreateScriptObject();
            auto Start = Clock::now();
            auto Collected = Machine.CollectGarbage();
            Milliseconds Elapsed = Clock::now() - Start;
            if (Collected != 1) throw std::runtime_error("unexpected GC result");
            Keeper.reset();
            if (Machine.CollectGarbage() != 1) throw std::runtime_error("GC retained the released object");
            return Elapsed;
        });
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << Failure.what() << '\n';
        return 1;
    }
}
