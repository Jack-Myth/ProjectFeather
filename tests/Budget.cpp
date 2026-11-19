#include <Feather/Compiler.hpp>

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
    // Two loads, comparison, JumpIf, then the constant and Return. The false
    // path executes one additional Jump before loading its result.
    Vm Six(Branch.Program, 100, 6);
    Check(Six.Run(Entry, Taken).AsNumber() == 1, "taken branch charged an extra jump");
    BudgetExceeded([&] { Six.Run(Entry, NotTaken); }, "false branch lost its jump charge");
    Vm Seven(Branch.Program, 100, 7);
    Check(Seven.Run(Entry, NotTaken).AsNumber() == 2, "false branch exceeded its exact budget");
    Check(SerializeProgram(Branch) == Original, "branch preparation modified the source program");
}

void CheckNumericAssignment() {
    auto Program = Compile("def adjust(x) { x = x * 2 + 1; return x; }");
    const auto Entry = Program.Functions.at("adjust");
    const auto Image = SerializeProgram(Program);
    const std::vector<Value> Args{Value::Number(7)};
    // GetLocal, MulConst, AddConst, SetLocal, Pop, GetLocal, Return.
    for (unsigned Limit = 1; Limit < 7; ++Limit) {
        Vm Limited(Program.Program, 100, Limit);
        BudgetExceeded([&] { Limited.Run(Entry, Args); }, "folded assignment lost an instruction charge");
    }
    Vm Exact(Program.Program, 100, 7);
    Check(Exact.Run(Entry, Args).AsNumber() == 15, "folded assignment failed at exact budget");
    Check(Exact.Run(Entry, {Value::Byte(7)}).GetType() == ValueType::Number, "byte promotion failed");
    Check(Exact.Run(Entry, {Value::String("x")}).IsError(), "assignment lost its arithmetic error");
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

} // namespace

int main() {
    try {
        CheckCompareBranchBudget();
        CheckNumericAssignment();
        CheckEntryInsideAssignment();
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
