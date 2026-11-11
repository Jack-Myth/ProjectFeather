#include <Feather/Compiler.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace Feather;

namespace {
void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}
void Reject(std::string_view Source, const char* Message) {
    try { (void)Compile(Source); }
    catch (const std::runtime_error& Error) {
        Check(std::string(Error.what()).find("source byte ") != std::string::npos,
              "source error lacks position");
        return;
    }
    throw std::runtime_error(Message);
}
class DoubleNative final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        return Value::Number(Arguments.at(0).AsNumber() * 2);
    }
};
class UpdateArrayNative final : public NativeObject {
public:
    explicit UpdateArrayNative(Vm& Machine) : Machine(Machine) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.empty()) {
            auto* Array = Machine.CreateArray();
            Array->AppendArray(Value::Number(10));
            Array->AppendArray(Value::Number(20));
            return Value::FromScript(Array);
        }
        Check(Arguments.size() == 1 && Arguments.front().IsScriptObject(), "native array argument");
        auto* Array = Arguments.front().AsScriptObject();
        Check(Array->IsArray() && Array->GetArrayLength() == 2, "native array type and length");
        Check(Array->GetArrayElements()[0].AsNumber() == 20 &&
              Array->GetArrayElements()[1].AsNumber() == 30, "native array element order");
        Check(!Array->RemoveArrayAt(2) && Array->GetArrayLength() == 2,
              "native invalid removal leaves array intact");
        Check(Array->RemoveArrayAt(0), "native RemoveArrayAt");
        Array->AppendArray(Value::Number(40));
        return Arguments.front();
    }
private:
    Vm& Machine;
};
}

int main() {
    try {
        auto Compiled = Compile(R"(
            // Every function is available when top-level statements run.
            def sum(n, base = 3) {
                var i = 0;
                while (i < n) { base = base + i; i = i + 1; }
                return base;
            }
            def twice(x) { return x + x; }
            def compute(x) {
                var result = twice(sum(x));
                { var result = 99; }
                return result;
            }
            def make(x) {
                var item = object();
                item.answer = x;
                item[2] = x + 1;
                return item;
            }
            def choose(x) { if (x < 2) { return 11; } else { return 22; } }
            def implicit() { var unused; }
            var answer = compute(4);
        )");
        Vm Machine(Compiled.Program);
        Check(Compiled.Initialize(Machine).GetType() == ValueType::Null, "initializer result");
        Check(Machine.GetGlobal("answer").AsNumber() == 18, "initializer and calls");
        Check(Machine.Run(Compiled.Functions.at("sum"), {Value::Number(3), Value::Number(10)}).AsNumber() == 13,
              "explicit argument overrides default");
        Check(Machine.Run(Compiled.Functions.at("choose"), {Value::Number(1)}).AsNumber() == 11,
              "if true branch");
        Check(Machine.Run(Compiled.Functions.at("choose"), {Value::Number(3)}).AsNumber() == 22,
              "if false branch");
        Check(Machine.Run(Compiled.Functions.at("implicit")).GetType() == ValueType::Null,
              "implicit return");

        auto Arguments = Compile(R"(
            def probe(a = 10) {
                var argc = vm.get_func_argc;
                var argv = vm.get_func_argv;
                if (argc() == 0) { return a; }
                if (argc() == 1) { return argv(0); }
                a = 99;
                if (argv(0) != 20) { return false; }
                if (probe() != 10) { return false; }
                if (argc() != 2) { return false; }
                if (get_type(argv(2)) != "Error") { return false; }
                if (get_type(argv(0.5)) != "Error") { return false; }
                if (get_type(argv(-1)) != "Error") { return false; }
                if (get_type(argv("0")) != "Error") { return false; }
                if (get_type(argv()) != "Error") { return false; }
                if (get_type(argc(0)) != "Error") { return false; }
                return argv(byte(1));
            }
            def nested() {
                if (probe(null) != null) { return false; }
                return probe(20, 30);
            }
        )");
        Vm ArgsVm(Arguments.Program);
        const auto VmModule = ArgsVm.GetVmModule();
        ArgsVm.SetGlobal("vm", VmModule);
        Arguments.Initialize(ArgsVm);
        Check(ArgsVm.Run(Arguments.Functions.at("nested")).AsNumber() == 30,
              "script argument count, values and frame isolation");
        Check(ArgsVm.Run(Arguments.Functions.at("probe"),
                         {Value::Number(20), Value::Number(30)}).AsNumber() == 30,
              "host entry argument count and values");
        const auto Argc = static_cast<NativeObject*>(VmModule.AsObject())->GetMember(
            Value::String("get_func_argc"));
        Check(ArgsVm.Call(Argc, {}).IsError(), "argument lookup without a script frame");

        auto ArrayProgram = Compile(R"(
            def exercise() {
                var native = update_array();
                if (native.Length != 2) { return false; }
                if (native.RemoveAt(0) != true) { return false; }
                if (native.Add(30) != true) { return false; }
                if (get_type(native.RemoveAt(2)) != "Error") { return false; }
                if (get_type(native.Length = 1) != "Error") { return false; }
                if (get_type(native[2] = 99) != "Error") { return false; }
                var script = array();
                script.Add(20);
                script.Add(30);
                if (update_array(script) != script) { return false; }
                if (script[0] != 30) { return false; }
                if (script[1] != 40) { return false; }
                if (update_array(native) != native) { return false; }
                if (native[0] != 30) { return false; }
                if (native[1] != 40) { return false; }
                return native.Length == 2;
            }
        )");
        Vm ArrayVm(ArrayProgram.Program);
        ArrayVm.RegisterNativeFunction("update_array", std::make_shared<UpdateArrayNative>(ArrayVm));
        ArrayProgram.Initialize(ArrayVm);
        Check(ArrayVm.Run(ArrayProgram.Functions.at("exercise")).AsBool(), "native array round trip");

        auto InvokeProgram = Compile(R"(
            var invoke = vm.invoke_function;
            def inspect(a = 10) {
                if (vm.get_func_argc() == 0) { return a; }
                if (invoke(vm.get_func_argc, array()) != 3) { return false; }
                if (a != null) { return false; }
                if (vm.get_func_argv(1) != 7) { return false; }
                return vm.get_func_argv(2);
            }
            def forward(n) {
                if (n == 0) { return 42; }
                var args = array();
                args.Add(n - 1);
                return invoke(forward, args);
            }
            def exercise() {
                if (invoke(inspect, array()) != 10) { return false; }
                var args = array();
                args.Add(null);
                args.Add(7);
                args.Add(42);
                if (invoke(inspect, args) != 42) { return false; }
                var nativeArgs = array();
                nativeArgs.Add(21);
                if (invoke(double, nativeArgs) != 42) { return false; }
                if (get_type(invoke(1, args)) != "Error") { return false; }
                if (get_type(invoke(inspect, object())) != "Error") { return false; }
                if (get_type(invoke(inspect)) != "Error") { return false; }
                var cycle = array();
                cycle.Add(invoke);
                cycle.Add(cycle);
                if (get_type(invoke(invoke, cycle)) != "Error") { return false; }
                return forward(80) == 42;
            }
        )");
        Vm InvokeVm(InvokeProgram.Program);
        InvokeVm.SetGlobal("vm", InvokeVm.GetVmModule());
        InvokeVm.RegisterNativeFunction("double", std::make_shared<DoubleNative>());
        InvokeProgram.Initialize(InvokeVm);
        Check(InvokeVm.Run(InvokeProgram.Functions.at("exercise")).AsBool(),
              "invoke_function forwards arguments with normal call frames");
        auto* NativeArgs = InvokeVm.CreateArray();
        NativeArgs->AppendArray(Value::Number(21));
        Check(InvokeVm.Call(InvokeVm.GetGlobal("invoke"),
                            {InvokeVm.GetGlobal("double"), Value::FromScript(NativeArgs)}).AsNumber() == 42,
              "invoke_function called from the host");

        auto Item = Machine.Run(Compiled.Functions.at("make"), {Value::Number(41)}).AsScriptObject();
        Check(Item->GetRaw(Value::String("answer"))->AsNumber() == 41, "dot member assignment");
        Check(Item->GetRaw(Value::Number(2))->AsNumber() == 42, "index member assignment");

        auto Recursion = Compile("def fact(n) { if (n < 2) { return 1; } return n * fact(n - 1); }");
        Vm Recursive(Recursion.Program);
        Recursion.Initialize(Recursive);
        Check(Recursive.Run(Recursion.Functions.at("fact"), {Value::Number(5)}).AsNumber() == 120,
              "recursive call");

        auto HostCall = Compile("def apply(x) { return double(x); }");
        Vm Hosted(HostCall.Program);
        Hosted.RegisterNativeFunction("double", std::make_shared<DoubleNative>());
        HostCall.Initialize(Hosted);
        Check(Hosted.Run(HostCall.Functions.at("apply"), {Value::Number(7)}).AsNumber() == 14,
              "compiled script calls host native function");

        auto Assign = Compile("def value() { var x = 1; return x = 2 + 3 * 4; }");
        Vm AssignmentVm(Assign.Program);
        Check(AssignmentVm.Run(Assign.Functions.at("value")).AsNumber() == 14,
              "assignment value and precedence");
        auto NotEqual = Compile(
            "def values() { return 1 != 2; } "
            "def same() { return \"x\" != \"x\"; } "
            "def mixed() { return 1 != \"1\"; } "
            "def objects() { var value = object(); return value != value; }");
        Vm NotEqualVm(NotEqual.Program);
        Check(NotEqualVm.Run(NotEqual.Functions.at("values")).AsBool(),
              "different numbers were equal");
        Check(!NotEqualVm.Run(NotEqual.Functions.at("same")).AsBool(),
              "equal strings were different");
        Check(NotEqualVm.Run(NotEqual.Functions.at("mixed")).AsBool(),
              "different value types were equal");
        Check(!NotEqualVm.Run(NotEqual.Functions.at("objects")).AsBool(),
              "one object was different from itself");
        auto LoopControl = Compile(R"(
            def nested() {
                var outer = 0;
                var total = 0;
                while (outer < 3) {
                    outer = outer + 1;
                    var inner = 0;
                    while (inner < 4) {
                        inner = inner + 1;
                        if (inner == 2) { continue; }
                        if (inner == 4) { break; }
                        total = total + outer * 10 + inner;
                    }
                }
                return total;
            }
            def stop() {
                var value = 0;
                while (true) { value = value + 1; break; value = 99; }
                return value;
            }
            var top = 0;
            while (top < 3) {
                top = top + 1;
                if (top < 2) { continue; }
                break;
            }
        )");
        Vm LoopControlVm(LoopControl.Program);
        Check(LoopControl.Initialize(LoopControlVm).GetType() == ValueType::Null &&
              LoopControlVm.GetGlobal("top").AsNumber() == 2,
              "top-level break/continue failed");
        Check(LoopControlVm.Run(LoopControl.Functions.at("nested")).AsNumber() == 132,
              "nested break/continue targeted the wrong loop");
        Check(LoopControlVm.Run(LoopControl.Functions.at("stop")).AsNumber() == 1,
              "break did not skip the rest of the loop body");
        auto Exported = Compile("export var answer = 41; export def add(x) { return answer + x; } var hidden = 9;");
        Check(Exported.Exports.size() == 2 && Exported.Exports[0] == "answer" &&
              Exported.Exports[1] == "add", "export declarations were not recorded");
        Vm ExportVm(Exported.Program);
        Exported.Initialize(ExportVm);
        Check(ExportVm.Run(Exported.Functions.at("add"), {Value::Number(1)}).AsNumber() == 42,
              "export modifier changed function behavior");
        try { Compiled.Initialize(AssignmentVm); throw std::runtime_error("foreign module accepted"); }
        catch (const std::invalid_argument&) {}

        Reject("fn old() {}", "old function keyword accepted");
        Reject("let old = 1;", "old variable keyword accepted");
        Reject("def a(x, x) {}", "duplicate parameter accepted");
        Reject("def a(x = 1, y) {}", "invalid default order accepted");
        Reject("def a() { var x; var x; }", "duplicate local accepted");
        Reject("def a() { export var x = 1; }", "block export accepted");
        Reject("export return 1;", "nondeclaration export accepted");
        Reject("def a() { (1 + 2) = 3; }", "invalid assignment accepted");
        Reject("def a() { return \"bad\\q\"; }", "invalid escape accepted");
        Reject("def a() { return 1e9999; }", "number overflow accepted");
        Reject("def a() { return 1 }", "missing semicolon accepted");
        Reject("def a() { return !true; }", "standalone bang accepted");
        Reject("def a() { break; }", "break outside loop accepted");
        Reject("def a() { continue; }", "continue outside loop accepted");
        Reject("break;", "top-level break outside loop accepted");
        Reject("continue;", "top-level continue outside loop accepted");
        Reject("def a() { return \"\xFF\"; }", "invalid UTF-8 string accepted");
        Reject("// \xFF\nfn a() {}", "invalid UTF-8 comment accepted");
        Reject("def a() { return " + std::string(300, '-') + "1; }", "deep unary expression accepted");
        std::cout << "M4 compiler tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "M4 compiler tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
