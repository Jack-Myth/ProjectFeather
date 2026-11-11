#include <Feather/Compiler.hpp>
#include <Feather/ByteBuffer.hpp>
#include <iostream>
#include <stdexcept>
using namespace Feather;
namespace {
void Check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid source accepted");
}
void Exercise(CompiledProgram program) {
    Vm machine(program.Program);
    machine.SetGlobal("vm", machine.GetVmModule());
    Check(!program.Initialize(machine).IsError(), "initialize");
    Check(machine.GetGlobal("total").AsNumber() == 0, "literal executed while created");
    auto code = machine.GetGlobal("code");
    Check(ByteBufferData(code) && !ByteBufferData(code)->empty(), "literal is not ByteBuffer");
    auto result = machine.Run(program.Functions.at("main"));
    Check(!result.IsError() && result.AsNumber() == 42, "execution/context/global scope/repeated evaluation");
    Check(machine.GetGlobal("total").AsNumber() == 2, "global assignments not preserved");
    Check(machine.EvalBytecode(Value::Number(1)).IsError(), "wrong argument accepted");
    Check(machine.EvalBytecode(ByteBufferLibrary(machine).Create("broken")).IsError(), "invalid bytecode accepted");
    auto truncated = *ByteBufferData(code); truncated.pop_back();
    Check(machine.EvalBytecode(ByteBufferLibrary(machine).Create(truncated)).IsError(), "truncated bytecode accepted");
}
}
int main() {
    try {
        auto program = Compile(R"(
            var total = 0;
            var code = @{ total = total + 1; var local = 20; context.value = context.value + 1; return local + context.value; };
            def main() {
                var state = object(); state.value = 20;
                vm.eval_bytecode(code, state);
                return vm.eval_bytecode(code, state);
            }
        )");
        Exercise(program);
        Exercise(DeserializeProgram(SerializeProgram(program)));
        auto quoted = Compile("var code = @{\n>stored, not executed\n};");
        Check(!quoted.UsesQuickOperators, "quoted shortcut incorrectly requires initializer host");
        Vm quoteVm(quoted.Program);
        Check(!quoted.Initialize(quoteVm).IsError(), "quoted shortcut ran during initialization");
        auto scope = Compile("var x = 42; def main(){ var x = 7; return vm.eval_bytecode(@{ return x; }); }");
        Vm scopeVm(scope.Program); scopeVm.SetGlobal("vm", scopeVm.GetVmModule());
        scope.Initialize(scopeVm);
        Check(scopeVm.Run(scope.Functions.at("main")).AsNumber() == 42, "block captured caller local");
        auto nested = Compile(R"(
            def main() {
                var outer = @{ return @{ return context + 1; }; };
                var inner = vm.eval_bytecode(outer);
                return vm.eval_bytecode(inner, 41);
            }
        )");
        Vm nestedVm(nested.Program); nestedVm.SetGlobal("vm", nestedVm.GetVmModule());
        nested.Initialize(nestedVm);
        Check(nestedVm.Run(nested.Functions.at("main")).AsNumber() == 42, "nested/returned blocks");
        auto root = Compile("def main() { return null; }");
        auto module = Compile(R"(
            var value = 40;
            export var code = @{ value = value + 1; return value; };
            export def run() { return vm.eval_bytecode(code); }
        )");
        Vm moduleVm(root.Program);
        module.LoadInto(moduleVm,"file:test");
        moduleVm.SetModuleGlobal("file:test", "vm", moduleVm.GetVmModule());
        module.InitializeModule(moduleVm,"file:test");
        Check(moduleVm.RunModule("file:test",module.Functions.at("run")).AsNumber() == 41, "module globals");
        Check(moduleVm.GetModuleGlobal("file:test","value").AsNumber() == 41, "module writeback");
        auto gc = Compile(R"(
            def main() {
                var kept = array();
                var value = object(); value.answer = 42;
                kept.add(value);
                value = null;
                var other = array();
                other.add(42);
                var remove = other.remove;
                other = null;
                var code = @{ var i = 0; while(i < 200) { var temp = object(); i = i + 1; } };
                vm.eval_bytecode(code);
                if (remove(42) != true) { return byte(-1); }
                return kept[0].answer;
            }
        )");
        Vm gcVm(gc.Program, 20); gcVm.SetGlobal("vm", gcVm.GetVmModule());
        gc.Initialize(gcVm);
        Check(gcVm.Run(gc.Functions.at("main")).AsNumber() == 42, "caller roots lost during eval GC");
        auto argsGc = Compile(R"(
            def make() {
                var value = object(); value.answer = 42;
                return value;
            }
            def check(a) {
                a = null;
                var i = 0;
                while (i < 200) { var temp = object(); i = i + 1; }
                if (vm.get_func_argv(0).answer != 42) { return false; }
                var contextValue = vm.eval_bytecode(@{
                    var i = 0;
                    while (i < 200) { var temp = object(); i = i + 1; }
                    if (vm.get_func_argc() != 1) { return false; }
                    return vm.get_func_argv(0);
                }, 42);
                if (contextValue != 42) { return false; }
                if (vm.get_func_argv(0).answer != 42) { return false; }
                return vm.get_func_argv(1).answer;
            }
            def main() { return check(make(), make()); }
        )");
        Vm argsGcVm(argsGc.Program, 20);
        argsGcVm.SetGlobal("vm", argsGcVm.GetVmModule());
        argsGc.Initialize(argsGcVm);
        Check(argsGcVm.Run(argsGc.Functions.at("main")).AsNumber() == 42,
              "original arguments lost during GC or nested eval");
        auto budget = Compile("def main(){ return vm.eval_bytecode(@{ while(true) {} }); }");
        Vm budgetVm(budget.Program,100,1000); budgetVm.SetGlobal("vm", budgetVm.GetVmModule());
        budget.Initialize(budgetVm);
        Reject([&]{ budgetVm.Run(budget.Functions.at("main")); });
        Reject([]{ Compile("var code = @{ var x = ; };"); });
        Reject([]{ Compile("while(true) { var code = @{ break; }; }"); });
        Reject([]{ Compile("var code = @{ def f() {} };"); });
        std::cout << "Compiled block tests passed\n";
    } catch(const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
