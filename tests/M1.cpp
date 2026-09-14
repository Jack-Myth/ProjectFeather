#include <Feather/Runtime.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Feather;

namespace {
void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}
template<class Action> void Reject(Action Run, const char* Message) {
    try { Run(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(Message);
}
std::uint32_t Function(std::shared_ptr<Module> Program, Builder Code,
                       std::uint32_t Parameters = 0, std::uint32_t Locals = 0,
                       std::vector<std::optional<Value>> Defaults = {}) {
    auto Body = std::make_shared<FunctionPrototype>();
    Body->Code = std::move(Code).Finish();
    Body->ParameterCount = Parameters;
    Body->LocalCount = Locals;
    Body->Defaults = std::move(Defaults);
    if (Body->Defaults.empty()) Body->Defaults.resize(Parameters);
    return Program->AddFunction(Body);
}
void ArithmeticAndErrors() {
    auto Program = std::make_shared<Module>();
    auto Two = Program->AddNumber(2);
    auto Three = Program->AddNumber(3);
    Builder Sum;
    Sum.EmitU32(Op::Const, Two); Sum.EmitU32(Op::Const, Three);
    Sum.Emit(Op::Add); Sum.Emit(Op::Return);
    auto SumId = Function(Program, std::move(Sum));
    Builder Bad;
    Bad.Emit(Op::True); Bad.EmitU32(Op::Const, Two); Bad.Emit(Op::Add); Bad.Emit(Op::Return);
    auto BadId = Function(Program, std::move(Bad));
    Builder Divide;
    Divide.EmitU32(Op::Const, Two); Divide.EmitU32(Op::Const, Program->AddNumber(0));
    Divide.Emit(Op::Div); Divide.Emit(Op::Return);
    auto DivideId = Function(Program, std::move(Divide));
    Vm Machine(Program);
    Check(Machine.Run(SumId).AsNumber() == 5, "numeric Add");
    auto Result = Machine.Run(BadId);
    Check(Result.GetType() == ValueType::Object &&
          Result.AsObject()->GetObjectType() == ObjectType::Error, "invalid operands return Error");
    Check(std::isinf(Machine.Run(DivideId).AsNumber()), "division by zero is infinity");
    Check(Value::Number(std::numeric_limits<double>::quiet_NaN()).IsTruthy(), "NaN truthiness");
    Check(!Value::Number(-0.0).IsTruthy(), "negative zero truthiness");
    Check(!Value::String("").IsTruthy(), "empty string truthiness");
    Reject([] { Value::String(std::string(1, static_cast<char>(0xFF))); }, "invalid UTF-8 accepted");

    auto More = std::make_shared<Module>();
    auto Nan = More->AddNumber(std::numeric_limits<double>::quiet_NaN());
    auto A = More->AddString("a"), B = More->AddString("b");
    Builder NanEqual;
    NanEqual.EmitU32(Op::Const, Nan); NanEqual.EmitU32(Op::Const, Nan);
    NanEqual.Emit(Op::Equal); NanEqual.Emit(Op::Return);
    auto NanId = Function(More, std::move(NanEqual));
    Builder Concat;
    Concat.EmitU32(Op::Const, A); Concat.EmitU32(Op::Const, B);
    Concat.Emit(Op::Add); Concat.Emit(Op::Return);
    auto ConcatId = Function(More, std::move(Concat));
    Builder NotCallable;
    NotCallable.EmitU32(Op::Const, A); NotCallable.EmitU16(Op::Call, 0);
    NotCallable.Emit(Op::Return);
    auto NotCallableId = Function(More, std::move(NotCallable));
    Vm Second(More);
    Check(!Second.Run(NanId).AsBool(), "NaN equality");
    Check(Second.Run(ConcatId).AsString() == "ab", "string concatenation");
    Check(Second.Run(NotCallableId).AsObject()->GetObjectType() == ObjectType::Error,
          "noncallable produces Error");
    Builder DefaultReturn; DefaultReturn.Emit(Op::Null); DefaultReturn.Emit(Op::Return);
    auto ReturnId = Function(More, std::move(DefaultReturn));
    Vm WithReturn(More);
    Check(WithReturn.Run(ReturnId).GetType() == ValueType::Null,
          "compiler-style default return sequence");
}
void CallsAndDefaults() {
    auto Program = std::make_shared<Module>();
    auto Three = Program->AddNumber(3);
    auto Four = Program->AddNumber(4);
    auto Five = Program->AddNumber(5);
    Builder Callee;
    Callee.EmitU32(Op::GetLocal, 0); Callee.EmitU32(Op::GetLocal, 1);
    Callee.Emit(Op::Add); Callee.Emit(Op::Return);
    auto CalleeId = Function(Program, std::move(Callee), 2, 2,
                             {std::nullopt, Value::Number(7)});
    Builder Caller;
    Caller.EmitU32(Op::Const, CalleeId); Caller.EmitU32(Op::Const, Three);
    Caller.EmitU16(Op::Call, 1); Caller.Emit(Op::Return);
    auto CallerId = Function(Program, std::move(Caller));
    Builder Extras;
    Extras.EmitU32(Op::Const, CalleeId); Extras.EmitU32(Op::Const, Three);
    Extras.EmitU32(Op::Const, Four); Extras.EmitU32(Op::Const, Five);
    Extras.EmitU16(Op::Call, 3); Extras.Emit(Op::Return);
    auto ExtrasId = Function(Program, std::move(Extras));
    Builder Missing;
    Missing.EmitU32(Op::Const, CalleeId); Missing.EmitU16(Op::Call, 0); Missing.Emit(Op::Return);
    auto MissingId = Function(Program, std::move(Missing));
    Vm Machine(Program);
    Check(Machine.Run(CallerId).AsNumber() == 10, "default parameter and nested call");
    Check(Machine.Run(ExtrasId).AsNumber() == 7, "extra argument discarded");
    Check(Machine.Run(MissingId).AsObject()->GetObjectType() == ObjectType::Error,
          "missing parameter becomes null");
    Builder BoolDefault; BoolDefault.EmitU32(Op::GetLocal, 0); BoolDefault.Emit(Op::Return);
    auto BoolId = Function(Program, std::move(BoolDefault), 1, 1, {Value::Bool(true)});
    Vm WithBool(Program);
    Check(WithBool.Run(BoolId).AsBool(), "bool default value");
}
void BranchLoopAndGlobals() {
    auto Program = std::make_shared<Module>();
    auto Zero = Program->AddNumber(0);
    auto One = Program->AddNumber(1);
    auto Three = Program->AddNumber(3);
    auto Name = Program->AddString("total");
    Builder Code;
    Code.EmitU32(Op::Const, Three); Code.EmitU32(Op::SetLocal, 0); Code.Emit(Op::Pop);
    Code.EmitU32(Op::Const, Zero); Code.EmitU32(Op::SetGlobal, Name); Code.Emit(Op::Pop);
    auto Loop = Code.Offset();
    Code.EmitU32(Op::Const, Zero); Code.EmitU32(Op::GetLocal, 0); Code.Emit(Op::Less);
    auto ToBody = Code.EmitJump(Op::JumpIf);
    auto ToEnd = Code.EmitJump(Op::Jump);
    auto Body = Code.Offset(); Code.PatchJump(ToBody, Body);
    Code.EmitU32(Op::GetGlobal, Name); Code.EmitU32(Op::GetLocal, 0);
    Code.Emit(Op::Add); Code.EmitU32(Op::SetGlobal, Name); Code.Emit(Op::Pop);
    Code.EmitU32(Op::GetLocal, 0); Code.EmitU32(Op::Const, One); Code.Emit(Op::Sub);
    Code.EmitU32(Op::SetLocal, 0); Code.Emit(Op::Pop);
    auto Back = Code.EmitJump(Op::Jump); Code.PatchJump(Back, Loop);
    auto End = Code.Offset(); Code.PatchJump(ToEnd, End);
    Code.EmitU32(Op::GetGlobal, Name); Code.Emit(Op::Return);
    auto Id = Function(Program, std::move(Code), 0, 1);
    Vm Machine(Program);
    Check(Machine.Run(Id).AsNumber() == 6, "branch and loop");
    Builder Undefined; Undefined.EmitU32(Op::GetGlobal, Program->AddString("missing"));
    Undefined.Emit(Op::Return);
    auto UndefinedId = Function(Program, std::move(Undefined));
    Vm Other(Program);
    Check(Other.Run(UndefinedId).AsObject()->GetObjectType() == ObjectType::Error,
          "undefined global Error");
}
void InvalidBytecode() {
    auto Program = std::make_shared<Module>();
    Builder Underflow; Underflow.Emit(Op::Pop); Underflow.Emit(Op::Null); Underflow.Emit(Op::Return);
    Function(Program, std::move(Underflow));
    Reject([&] { Program->Validate(); }, "stack underflow accepted");
    auto Other = std::make_shared<Module>();
    Builder NoReturn; NoReturn.Emit(Op::Null);
    Function(Other, std::move(NoReturn));
    Reject([&] { Other->Validate(); }, "fallthrough accepted");
    auto Third = std::make_shared<Module>();
    Builder Extra; Extra.Emit(Op::Null); Extra.Emit(Op::Null); Extra.Emit(Op::Return);
    Function(Third, std::move(Extra));
    Reject([&] { Third->Validate(); }, "extra Return value accepted");
    auto Fourth = std::make_shared<Module>();
    Builder Misjump; auto Target = Misjump.EmitJump(Op::Jump);
    Misjump.PatchJump(Target, Target + 1); Misjump.Emit(Op::Null); Misjump.Emit(Op::Return);
    Function(Fourth, std::move(Misjump));
    Reject([&] { Fourth->Validate(); }, "jump into operand accepted");
    auto Fifth = std::make_shared<Module>();
    Builder Mismatch;
    Mismatch.Emit(Op::True);
    auto Branch = Mismatch.EmitJump(Op::JumpIf);
    Mismatch.Emit(Op::Null);
    auto Join = Mismatch.Offset(); Mismatch.PatchJump(Branch, Join);
    Mismatch.Emit(Op::Null); Mismatch.Emit(Op::Return);
    Function(Fifth, std::move(Mismatch));
    Reject([&] { Fifth->Validate(); }, "branch stack mismatch accepted");

    auto Source = std::make_shared<Module>();
    Builder Inner; Inner.Emit(Op::Null); Inner.Emit(Op::Return);
    auto InnerId = Function(Source, std::move(Inner));
    Builder Export; Export.EmitU32(Op::Const, InnerId); Export.Emit(Op::Return);
    auto ExportId = Function(Source, std::move(Export));
    Vm SourceVm(Source);
    auto ForeignFunction = SourceVm.Run(ExportId);
    auto Destination = std::make_shared<Module>();
    Builder Invoke; Invoke.EmitU32(Op::GetLocal, 0); Invoke.EmitU16(Op::Call, 0);
    Invoke.Emit(Op::Return);
    auto InvokeId = Function(Destination, std::move(Invoke), 1, 1);
    Vm DestinationVm(Destination);
    Reject([&] { DestinationVm.Run(InvokeId, {ForeignFunction}); },
           "cross-VM function accepted with wrong constant pool");
}
} // namespace

int main() {
    try {
        ArithmeticAndErrors(); CallsAndDefaults(); BranchLoopAndGlobals(); InvalidBytecode();
        std::cout << "M1 tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "M1 tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
