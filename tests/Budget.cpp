#include <Feather/Compiler.hpp>
#include <cmath>

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Feather;

namespace {

void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}

template <typename Action>
void BudgetExceeded(Action&& Run, const char* Message) {
    try { Run(); }
    catch (const std::runtime_error& Failure) {
        if (std::string_view(Failure.what()) == "VM instruction budget exceeded") return;
        throw;
    }
    throw std::runtime_error(Message);
}

class NestedCall final : public NativeObject {
public:
    NestedCall(Vm& Machine, std::uint32_t Entry) : Machine(Machine), Entry(Entry) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (!Arguments.empty()) return Value::FromObject(std::make_shared<ErrorObject>("no arguments expected"));
        return Machine.Run(Entry);
    }
private:
    Vm& Machine;
    std::uint32_t Entry;
};

void CheckCompareBranchBudget() {
    auto Branch = Compile("def choose(a,b) { if (a < b) { return 1; } return 2; }");
    const auto Entry = Branch.Functions.at("choose");
    const auto Original = SerializeProgram(Branch);
    const std::vector<Value> Taken{Value::Byte(0), Value::Number(1)};
    const std::vector<Value> NotTaken{Value::Number(1), Value::Number(0)};
    // The mixed numeric path dispatches two loads, one fused branch, Const and Return.
    Vm Four(Branch.Program, 100, 4), Five(Branch.Program, 100, 5);
    BudgetExceeded([&] { Four.Run(Entry, Taken); }, "mixed branch fit below its dispatch count");
    Check(Five.Run(Entry, Taken).AsNumber() == 1, "mixed branch exceeded its dispatch count");
    Vm Three(Branch.Program, 100, 3);
    Check(Three.Run(Entry, NotTaken).AsNumber() == 2, "numeric comparison block exceeded its budget");
    Check(SerializeProgram(Branch) == Original, "branch preparation modified the source program");
}

void CheckNumericAssignment() {
    auto Program = Compile("def adjust(x) { x = x * 2 + 1; return x; }");
    const auto Entry = Program.Functions.at("adjust");
    const auto Image = SerializeProgram(Program);
    const std::vector<Value> Args{Value::Number(7)};
    // One arithmetic block, GetLocal and Return.
    for (unsigned Limit = 1; Limit < 3; ++Limit) {
        Vm Limited(Program.Program, 100, Limit);
        BudgetExceeded([&] { Limited.Run(Entry, Args); }, "folded assignment lost an instruction charge");
    }
    Vm Exact(Program.Program, 100, 3), Fallback(Program.Program, 100, 7);
    Check(Exact.Run(Entry, Args).AsNumber() == 15, "folded assignment failed at exact budget");
    Check(Fallback.Run(Entry, {Value::Byte(7)}).GetType() == ValueType::Number, "byte promotion failed");
    Check(Fallback.Run(Entry, {Value::String("x")}).IsError(), "assignment lost its arithmetic error");
    Check(SerializeProgram(Program) == Image, "optimization changed the source bytecode");
}

void CheckEntryInsideAssignment() {
    auto Program = std::make_shared<Module>();
    auto Two = Program->AddNumber(2);
    auto Function = std::make_shared<FunctionPrototype>();
    Function->ParameterCount = Function->LocalCount = 3;
    Function->Defaults.resize(3);
    Builder Code;
    Code.EmitU32(Op::GetLocal, 2);
    auto First = Code.EmitJump(Op::JumpIf);
    Code.EmitU32(Op::GetLocal, 1);
    auto Interior = Code.EmitJump(Op::Jump);
    Code.PatchJump(First, Code.Offset());
    Code.EmitU32(Op::GetLocal, 0);
    Code.PatchJump(Interior, Code.Offset());
    Code.EmitU32(Op::MulConst, Two);
    Code.EmitU32(Op::SetLocal, 0);
    Code.Emit(Op::Pop);
    Code.EmitU32(Op::GetLocal, 0);
    Code.Emit(Op::Return);
    Function->Code = std::move(Code).Finish();
    auto Entry = Program->AddFunction(Function);
    Vm Machine(Program);
    for (bool UseFirst : {true, false}) {
        auto Result = Machine.Run(Entry, {Value::Number(3), Value::Number(5), Value::Bool(UseFirst)});
        Check(Result.AsNumber() == (UseFirst ? 6 : 10), "branch into assignment changed its operand");
    }
}

void CheckBinaryAssignment() {
    auto Program = Compile("def divide(x,y) { y = x / y; return y; }");
    const auto Entry = Program.Functions.at("divide");
    const std::vector<Value> Args{Value::Number(7), Value::Number(2)};
    Vm Short(Program.Program, 100, 2), Exact(Program.Program, 100, 3), Fallback(Program.Program, 100, 7);
    BudgetExceeded([&] { (void)Short.Run(Entry, Args); }, "binary assignment skipped a budget charge");
    Check(Exact.Run(Entry, Args).AsNumber() == 3.5, "binary assignment overwrote its right operand");
    Check(Fallback.Run(Entry, {Value::Number(7), Value::Byte(2)}).AsNumber() == 3.5,
          "binary assignment lost byte promotion");
    const auto Infinity = Exact.Run(Entry, {Value::Number(1), Value::Number(-0.0)}).AsNumber();
    Check(std::isinf(Infinity) && std::signbit(Infinity), "division by negative zero lost its sign");
    Check(std::isnan(Exact.Run(Entry, {Value::Number(0), Value::Number(0)}).AsNumber()),
          "zero divided by zero must be NaN");
}

void CheckSlotCall() {
    auto Program = Compile(R"(
        def difference(a,b) { return a-b; }
        def run(f,a,b) { a=f(b,a); return a; }
    )");
    // Obtain the function value without spending this deliberately tiny budget on module initialization.
    Builder LoadTarget;
    LoadTarget.EmitU32(Op::Const, Program.Functions.at("difference"));
    LoadTarget.Emit(Op::Return);
    auto Getter = std::make_shared<FunctionPrototype>();
    Getter->Code = std::move(LoadTarget).Finish();
    const auto GetTarget = Program.Program->AddFunction(Getter);
    auto CheckBudget = [&](std::size_t Limit, bool ExpectSuccess) {
        Vm Machine(Program.Program, 100, Limit);
        auto Target = Machine.Run(GetTarget);
        auto Run = [&] {
            return Machine.Run(Program.Functions.at("run"),
                               {Target, Value::Number(2), Value::Number(7)});
        };
        if (ExpectSuccess) Check(Run().AsNumber() == 5, "call-site argument order or return destination");
        else BudgetExceeded(Run, "slot call skipped an instruction charge");
    };
    // One slot call, four callee instructions, then GetLocal and Return.
    CheckBudget(6, false);
    CheckBudget(7, true);
}

void CheckLocalFields() {
    auto Program = Compile(R"(
        def bump(p) { p.x = p.x + 1; return p.x; }
        def rebind(p, q) { return p.x = (p = q).x; }
    )");
    auto Key = Value::String("x");
    auto Bump = Program.Functions.at("bump");
    // Prepare the assignment, read x, add, store/discard, read x, return.
    for (std::size_t Limit : {5u, 6u}) {
        Vm Machine(Program.Program, 100, Limit);
        auto* Object = Machine.CreateScriptObject();
        Object->SetRaw(Key, Value::Number(10));
        auto Run = [&] { return Machine.Run(Bump, {Value::FromScript(Object)}); };
        if (Limit == 5) BudgetExceeded(Run, "field update skipped a dispatch charge");
        else Check(Run().AsNumber() == 11, "field update exceeded its fused budget");
    }
    Vm Machine(Program.Program);
    auto* Left = Machine.CreateScriptObject();
    auto* Right = Machine.CreateScriptObject();
    Left->SetRaw(Key, Value::Number(1));
    Right->SetRaw(Key, Value::Number(42));
    auto Result = Machine.Run(Program.Functions.at("rebind"),
                              {Value::FromScript(Left), Value::FromScript(Right)});
    Check(Result.AsNumber() == 42 && Left->GetRaw(Key)->AsNumber() == 42,
          "assignment must retain its original receiver while evaluating the RHS");

    struct Steps final : VmDebugController {
        int Count = 0;
        void OnSafePoint(const VmDebugContext&) override { ++Count; }
        void OnError(const VmDebugContext&, const Value&, DebugErrorOrigin) override {}
        void OnExecutionFinished(bool) override {}
    };
    auto Debug = std::make_shared<Steps>();
    Machine.SetDebugController(Debug);
    Check(Machine.Run(Bump, {Value::FromScript(Left)}).AsNumber() == 43 && Debug->Count == 10,
          "debugger must observe every original instruction in a local field update");
}

} // namespace

int main() {
    try {
        CheckCompareBranchBudget();
        CheckNumericAssignment();
        CheckEntryInsideAssignment();
        CheckBinaryAssignment();
        CheckSlotCall();
        CheckLocalFields();
        auto Loop = Compile("def spin() { while (true) { } } def answer() { return 42; }");
        Vm Limited(Loop.Program, std::numeric_limits<std::size_t>::max(), 40);
        Check(!Loop.Initialize(Limited).IsError(), "initializer should fit its budget");
        BudgetExceeded([&] { (void)Limited.Run(Loop.Functions.at("spin")); },
                       "infinite loop was not stopped");
        Check(Limited.Run(Loop.Functions.at("answer")).AsNumber() == 42,
              "next independent Run should receive a fresh budget");

        auto Nested = Compile(
            "def tiny() { return 1; } "
            "def outer() { return invoke() + invoke() + invoke(); }");
        Vm Reentrant(Nested.Program, std::numeric_limits<std::size_t>::max(), 12);
        Reentrant.RegisterNativeFunction("invoke",
            std::make_shared<NestedCall>(Reentrant, Nested.Functions.at("tiny")));
        Check(!Nested.Initialize(Reentrant).IsError(), "nested test initializer failed");
        BudgetExceeded([&] { (void)Reentrant.Run(Nested.Functions.at("outer")); },
                       "nested Run reset the outer budget");
        Check(Reentrant.Run(Nested.Functions.at("tiny")).AsNumber() == 1,
              "fresh execution after nested failure was rejected");

        bool ZeroRejected = false;
        try { Vm Invalid(Loop.Program, 100, 0); }
        catch (const std::invalid_argument&) { ZeroRejected = true; }
        Check(ZeroRejected, "zero instruction budget should be invalid");
        std::cout << "Budget tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "Budget tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
