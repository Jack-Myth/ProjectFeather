#include <Feather/Checkpoint.hpp>
#include <Feather/Compiler.hpp>

#include <iostream>
#include <stdexcept>

using namespace Feather;

namespace {
void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}
}

int main() {
    try {
        auto Program = Compile(R"(
            def restoreFromChild(id) { return checkpoint.Restore(id); }

            def repeat() {
                var count = 0;
                var value = 40 + checkpoint.Capture();
                count = count + 1;
                if (count < 3) { return restoreFromChild(value - 40); }
                return value + count;
            }

            def unload() {
                var id = checkpoint.Capture();
                checkpoint.Unload(id);
                return checkpoint.Restore(id);
            }

            def escape() { return checkpoint.Capture(); }
            def restoreReturnedFrame() {
                var id = escape();
                return checkpoint.Restore(id);
            }
            def restoreExpired(id) { return checkpoint.Restore(id); }
        )");
        Vm Machine(Program.Program);
        CheckpointLibrary Checkpoints(Machine);
        Machine.SetGlobal("checkpoint", Checkpoints.GetModule());
        Check(!Program.Initialize(Machine).IsError(), "checkpoint initializer");

        auto Repeated = Machine.Run(Program.Functions.at("repeat"));
        Check(Repeated.GetType() == ValueType::Number && Repeated.AsNumber() == 44,
              "checkpoint restore preserves locals, operand stack, and supports reuse");

        Check(Machine.Run(Program.Functions.at("unload")).IsError(),
              "unloaded checkpoint must be rejected");

        Check(Machine.Run(Program.Functions.at("restoreReturnedFrame")).IsError(),
              "checkpoint whose captured frame returned must be rejected");

        auto Escaped = Machine.Run(Program.Functions.at("escape"));
        Check(Escaped.GetType() == ValueType::Number,
              "checkpoint capture returns a numeric ID");
        Check(Machine.Run(Program.Functions.at("restoreExpired"), {Escaped}).IsError(),
              "checkpoint must expire when the outer run ends");

        std::cout << "checkpoint tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << Failure.what() << '\n';
        return 1;
    }
}
