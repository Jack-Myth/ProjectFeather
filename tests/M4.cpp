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
