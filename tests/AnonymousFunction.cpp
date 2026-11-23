#include <Feather/Compiler.hpp>
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
    Check(!program.Initialize(machine).IsError(), "initialize");
    Check(machine.GetGlobal("total").AsNumber() == 0, "literal executed while created");
    auto result = machine.Run(program.Functions.at("main"));
    Check(!result.IsError() && result.AsNumber() == 42, "execution/context/global scope/repeated evaluation");
    Check(machine.GetGlobal("total").AsNumber() == 2, "global assignments not preserved");
}
}
int main() {
    try {
        auto program = Compile(R"(
            var total = 0;
            var code = @(context) { total = total + 1; var local = 20; context.value = context.value + 1; return local + context.value; };
            def main() {
                var state = object(); state.value = 20;
                code(state);
                return code(state);
            }
        )");
        Exercise(program);
        Exercise(DeserializeProgram(SerializeProgram(program)));
        auto quick = Compile("var code = @{\n>deferred\n};");
        Check(quick.UsesQuickOperators, "anonymous function shortcut missing host requirement");
        Vm quickVm(quick.Program);
        Check(!quick.Initialize(quickVm).IsError(), "shortcut ran during initialization");
        auto scope = Compile("var x = 42; def main(){ var x = 7; return @{ return x; }(); }");
        Vm scopeVm(scope.Program);
        scope.Initialize(scopeVm);
        Check(scopeVm.Run(scope.Functions.at("main")).AsNumber() == 42, "anonymous function captured caller local");
        auto nested = Compile(R"(
            def main() {
                var outer = @() { return @(a, b = 1) { return a + b; }; };
                var inner = outer();
                return inner(41);
            }
        )");
        Vm nestedVm(nested.Program);
        nested.Initialize(nestedVm);
        Check(nestedVm.Run(nested.Functions.at("main")).AsNumber() == 42, "nested/returned functions");
        auto root = Compile("def main() { return null; }");
        auto module = Compile(R"(
            var value = 40;
            export var code = @{ value = value + 1; return value; };
        )");
        Vm moduleVm(root.Program);
        module.LoadInto(moduleVm,"file:test");
        module.InitializeModule(moduleVm,"file:test");
        moduleVm.SetGlobal("value", Value::Number(900));
        Check(moduleVm.Call(moduleVm.GetModuleGlobal("file:test", "code")).AsNumber() == 41, "module globals");
        Check(moduleVm.GetModuleGlobal("file:test","value").AsNumber() == 41, "module writeback");
        auto gc = Compile(R"(
            def main() {
                var kept = array();
                var value = object(); value.answer = 42;
                kept.Add(value);
                value = null;
                var other = array();
                other.Add(42);
                var remove = other.Remove;
                other = null;
                var code = @{ var i = 0; while(i < 200) { var temp = object(); i = i + 1; } };
                code();
                if (remove(42) != true) { return byte(-1); }
                return kept[0].answer;
            }
        )");
        Vm gcVm(gc.Program, 20);
        gc.Initialize(gcVm);
        Check(gcVm.Run(gc.Functions.at("main")).AsNumber() == 42, "caller roots lost during GC");
        auto argsGc = Compile(R"(
            def make() {
                var value = object(); value.answer = 42;
                return value;
            }
            def check(a) {
                a = null;
                var i = 0;
                while (i < 200) { var temp = object(); i = i + 1; }
                if (vm.GetArgument(0).answer != 42) { return false; }
                var contextValue = @{
                    var i = 0;
                    while (i < 200) { var temp = object(); i = i + 1; }
                    if (vm.GetArgumentCount() != 1) { return false; }
                    return vm.GetArgument(0);
                }(42);
                if (contextValue != 42) { return false; }
                if (vm.GetArgument(0).answer != 42) { return false; }
                return vm.GetArgument(1).answer;
            }
            def descend(depth) {
                if (depth == 0) { return check(make(), make()); }
                return 1 + descend(depth - 1);
            }
            def main() {
                if (descend(48) != 90) { return false; }
                return descend(0);
            }
        )");
        Vm argsGcVm(argsGc.Program, 20);
        argsGcVm.SetGlobal("vm", argsGcVm.GetVmModule());
        argsGc.Initialize(argsGcVm);
        Check(argsGcVm.Run(argsGc.Functions.at("main")).AsNumber() == 42,
              "original arguments lost during GC or nested call");
        auto budget = Compile("def main(){ return @{ while(true) {} }(); }");
        Vm budgetVm(budget.Program,100,1000);
        budget.Initialize(budgetVm);
        Reject([&]{ budgetVm.Run(budget.Functions.at("main")); });
        Reject([]{ Compile("var code = @(x, x) {};"); });
        Reject([]{ Compile("var code = @(x = 1, y) {};"); });
        Reject([]{ Compile("var code = @{ var x = ; };"); });
        Reject([]{ Compile("while(true) { var code = @{ break; }; }"); });
        Reject([]{ Compile("var code = @{ def f() {} };"); });
        std::cout << "Anonymous function tests passed\n";
    } catch(const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
