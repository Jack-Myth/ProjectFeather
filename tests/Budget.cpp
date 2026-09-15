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

} // namespace

int main() {
    try {
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
