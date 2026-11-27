#include <Feather/Runtime.hpp>
#include <Feather/Utf8.hpp>
#include "InstructionWidth.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace Feather {
namespace {
constexpr std::size_t MaxCode = 1'048'576;
constexpr std::size_t MaxConstants = 65'536;
constexpr std::size_t MaxStack = 65'536;

void WriteU32(std::vector<std::uint8_t>& Code, std::uint32_t Value) {
    for (unsigned I = 0; I < 4; ++I) Code.push_back(static_cast<std::uint8_t>(Value >> (I * 8)));
}
std::uint32_t ReadU32(const std::vector<std::uint8_t>& Code, std::size_t At) {
    std::uint32_t Result = 0;
    for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(Code[At + I]) << (I * 8);
    return Result;
}
struct Instruction {
    Op Code;
    std::size_t Start;
    std::size_t End;
    std::uint32_t Operand = 0;
    int Consumes = 0;
    int Produces = 0;
};
[[noreturn]] void Invalid(const char* Message) { throw std::invalid_argument(Message); }

void VerifyFunction(const Module& Program, const FunctionPrototype& Function) {
    if (Function.Code.empty() || Function.Code.size() > MaxCode) Invalid("invalid function code size");
    if (Function.ParameterCount > Function.LocalCount || Function.LocalCount > MaxStack ||
        Function.Defaults.size() != Function.ParameterCount) Invalid("invalid function locals or defaults");
    for (const auto& Default : Function.Defaults) {
        if (!Default) continue;
        if (Default->GetType() == ValueType::Object)
            Invalid("default must be a primitive constant");
    }
    std::vector<Instruction> Instructions;
    const auto& Code = Function.Code;
    // PCs are byte offsets bounded by MaxCode; a dense table avoids one
    // allocation per instruction and still distinguishes operand bytes.
    constexpr std::uint32_t NotInstruction = UINT32_MAX;
    std::vector<std::uint32_t> Indices(Code.size(), NotInstruction);
    auto IsInstruction = [&](std::size_t Pc) {
        return Pc < Indices.size() && Indices[Pc] != NotInstruction;
    };
    for (std::size_t Pc = 0; Pc < Code.size();) {
        Instruction Current{static_cast<Op>(Code[Pc]), Pc, Pc + 1};
        if (Current.Code == Op::NotEqual && Program.InstructionVersion < 6) Invalid("opcode requires instruction version 6");
        const auto Width = OperandWidth(Current.Code);
        if (Code.size() - Pc - 1 < Width) Invalid("truncated instruction");
        Current.End += Width;
        if (Width == 4) Current.Operand = ReadU32(Code, Pc + 1);
        if (Width == 2) Current.Operand = std::uint32_t(Code[Pc + 1]) | (std::uint32_t(Code[Pc + 2]) << 8);
        switch (Current.Code) {
        case Op::AddToLocal: case Op::AddConstToLocal: {
            const auto Local = Current.Operand & 0xffff;
            const auto Right = Current.Operand >> 16;
            if (Local >= Function.LocalCount) Invalid("local index out of range");
            if (Current.Code == Op::AddToLocal) {
                if (Right >= Function.LocalCount) Invalid("local index out of range");
            } else if (Right >= Program.Constants.size() || Program.Constants[Right].Type != Constant::Kind::Number)
                Invalid("addition requires a number constant");
            break;
        }
        case Op::AddConst: case Op::SubConst: case Op::MulConst: case Op::GetField: {
            auto Type = Current.Code == Op::GetField ? Constant::Kind::String : Constant::Kind::Number;
            if (Current.Operand >= Program.Constants.size() || Program.Constants[Current.Operand].Type != Type)
                Invalid("invalid instruction constant");
            Current.Consumes = 1; Current.Produces = 1; break;
        }
        case Op::Const:
            if (Current.Operand >= Program.Constants.size()) Invalid("constant index out of range");
            [[fallthrough]];
        case Op::Null: case Op::True: case Op::False: case Op::NewObject:
        case Op::GetLocal: case Op::GetGlobal: Current.Produces = 1; break;
        case Op::Pop: case Op::StoreLocal: Current.Consumes = 1; break;
        case Op::SetLocal: case Op::SetGlobal: case Op::ToByte: case Op::Negate: Current.Consumes = 1; Current.Produces = 1; break;
        case Op::GetMember: case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        case Op::NotEqual: case Op::Equal: case Op::Less: Current.Consumes = 2; Current.Produces = 1; break;
        case Op::SetMember: Current.Consumes = 3; Current.Produces = 1; break;
        case Op::JumpIf: Current.Consumes = 1; break;
        case Op::Call: Current.Consumes = static_cast<int>(Current.Operand) + 1; Current.Produces = 1; break;
        case Op::Return: Current.Consumes = 1; break;
        case Op::Jump: break;
        }
        if ((Current.Code == Op::GetLocal || Current.Code == Op::SetLocal || Current.Code == Op::StoreLocal) &&
            Current.Operand >= Function.LocalCount) Invalid("local index out of range");
        if (Current.Code == Op::GetGlobal || Current.Code == Op::SetGlobal) {
            if (Current.Operand >= Program.Constants.size() ||
                Program.Constants[Current.Operand].Type != Constant::Kind::String)
                Invalid("global name must be a string constant");
        }
        Indices[Pc] = static_cast<std::uint32_t>(Instructions.size());
        Instructions.push_back(Current);
        Pc = Current.End;
    }
    if (!Function.Locations.empty()) {
        if (Function.Locations.size() != Instructions.size())
            Invalid("debug location count does not match instructions");
        for (std::size_t I = 0; I < Instructions.size(); ++I) {
            const auto& Location = Function.Locations[I];
            if (Location.Pc != Instructions[I].Start ||
                Location.Source.Line == 0 || Location.Source.Column == 0)
                Invalid("invalid debug instruction location");
        }
    }
    if (!Function.DebugName.empty()) ValidateUtf8(Function.DebugName);
    if (Function.LocalVariables.size() > Function.LocalCount)
        Invalid("too many debug local variables");
    std::vector<bool> SeenLocalSlots(Function.LocalCount, false);
    for (const auto& Variable : Function.LocalVariables) {
        if (Variable.Name.empty()) Invalid("empty debug local variable name");
        ValidateUtf8(Variable.Name);
        if (Variable.Slot >= Function.LocalCount || SeenLocalSlots[Variable.Slot])
            Invalid("invalid or duplicate debug local variable slot");
        SeenLocalSlots[Variable.Slot] = true;
        if (Variable.StartPc > Variable.EndPc || Variable.EndPc > Code.size() ||
            !IsInstruction(Variable.StartPc) ||
            (Variable.EndPc != Code.size() && !IsInstruction(Variable.EndPc)))
            Invalid("invalid debug local variable range");
        if (Variable.Slot < Function.ParameterCount &&
            (Variable.StartPc != 0 || Variable.EndPc != Code.size()))
            Invalid("debug parameter range must cover the function");
    }
    std::vector<int> Heights(Instructions.size(), -1);
    std::vector<std::uint32_t> Pending;
    Heights[0] = 0;
    Pending.push_back(0);
    for (std::size_t Cursor = 0; Cursor < Pending.size(); ++Cursor) {
        auto Index = Pending[Cursor];
        auto& Current = Instructions[Index];
        int Height = Heights[Index];
        if ((Current.Code == Op::AddToLocal || Current.Code == Op::AddConstToLocal) && Height != 0)
            Invalid("local update requires an empty expression stack");
        if (Height < Current.Consumes) Invalid("bytecode stack underflow");
        if (Current.Code == Op::Return && Height != 1) Invalid("Return requires exactly one value");
        int NextHeight = Height - Current.Consumes + Current.Produces;
        if (NextHeight > static_cast<int>(MaxStack)) Invalid("bytecode stack limit exceeded");
        auto Enqueue = [&](std::size_t Target) {
            if (!IsInstruction(Target)) Invalid("jump or fallthrough is not an instruction");
            auto Next = Indices[Target];
            if (Heights[Next] >= 0 && Heights[Next] != NextHeight) Invalid("stack height mismatch");
            if (Heights[Next] < 0) { Heights[Next] = NextHeight; Pending.push_back(Next); }
        };
        if (Current.Code == Op::Return) continue;
        if (Current.Code == Op::Jump || Current.Code == Op::JumpIf) {
            auto Offset = std::bit_cast<std::int32_t>(Current.Operand);
            auto Target = static_cast<std::int64_t>(Current.End) + Offset;
            if (Target < 0 || Target >= static_cast<std::int64_t>(Code.size())) Invalid("jump target outside function");
            Enqueue(static_cast<std::size_t>(Target));
        }
        if (Current.Code != Op::Jump) Enqueue(Current.End);
    }
    // Reject even unreachable malformed jump targets: a module is valid as a whole.
    for (const auto& Current : Instructions) {
        if (Current.Code != Op::Jump && Current.Code != Op::JumpIf) continue;
        auto Target = static_cast<std::int64_t>(Current.End) + std::bit_cast<std::int32_t>(Current.Operand);
        if (Target < 0 || Target >= static_cast<std::int64_t>(Code.size()) ||
            !IsInstruction(static_cast<std::size_t>(Target))) Invalid("invalid jump target");
    }
}
} // namespace

Constant Constant::Number(double Input) { Constant Result{Kind::Number}; Result.Numeric = Input; return Result; }
Constant Constant::Byte(double Input) {
    Constant Result{Kind::Byte}; Result.ByteValue = Value::Byte(Input).AsByte(); return Result;
}
Constant Constant::String(std::string Input) { Constant Result{Kind::String}; Result.Text = std::move(Input); return Result; }
Constant Constant::FunctionRef(std::shared_ptr<FunctionPrototype> Input) {
    Constant Result{Kind::Function}; Result.Function = std::move(Input); return Result;
}
std::uint32_t Module::AddNumber(double Input) {
    Constants.push_back(Constant::Number(Input)); return static_cast<std::uint32_t>(Constants.size() - 1);
}
std::uint32_t Module::AddByte(double Input) {
    Constants.push_back(Constant::Byte(Input)); return static_cast<std::uint32_t>(Constants.size() - 1);
}
std::uint32_t Module::AddString(std::string Input) {
    Constants.push_back(Constant::String(std::move(Input))); return static_cast<std::uint32_t>(Constants.size() - 1);
}
std::uint32_t Module::AddFunction(std::shared_ptr<FunctionPrototype> Input) {
    Constants.push_back(Constant::FunctionRef(std::move(Input))); return static_cast<std::uint32_t>(Constants.size() - 1);
}
void Module::Validate() const {
    if (InstructionVersion != 5 && InstructionVersion != CurrentInstructionVersion)
        Invalid("unsupported instruction version");
    if (Constants.size() > MaxConstants) Invalid("too many constants");
    for (const auto& Item : Constants) {
        if (Item.Type == Constant::Kind::String) ValidateUtf8(Item.Text);
        if (Item.Type == Constant::Kind::Function) {
            if (!Item.Function) Invalid("null function prototype");
            VerifyFunction(*this, *Item.Function);
        }
    }
}
void Builder::Emit(Op Instruction) { Code.push_back(static_cast<std::uint8_t>(Instruction)); }
void Builder::EmitU16(Op Instruction, std::uint16_t Operand) {
    Emit(Instruction); Code.push_back(static_cast<std::uint8_t>(Operand));
    Code.push_back(static_cast<std::uint8_t>(Operand >> 8));
}
void Builder::EmitU32(Op Instruction, std::uint32_t Operand) { Emit(Instruction); WriteU32(Code, Operand); }
std::size_t Builder::EmitJump(Op Instruction) {
    if (Instruction != Op::Jump && Instruction != Op::JumpIf) Invalid("not a jump opcode");
    Emit(Instruction); auto Position = Code.size(); WriteU32(Code, 0); return Position;
}
void Builder::PatchJump(std::size_t OperandPosition, std::size_t Target) {
    if (OperandPosition == 0 || OperandPosition + 4 > Code.size()) Invalid("invalid jump patch position");
    auto Delta = static_cast<std::int64_t>(Target) - static_cast<std::int64_t>(OperandPosition + 4);
    if (Delta < std::numeric_limits<std::int32_t>::min() ||
        Delta > std::numeric_limits<std::int32_t>::max()) Invalid("jump offset out of range");
    auto Bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(Delta));
    for (unsigned I = 0; I < 4; ++I) Code[OperandPosition + I] = static_cast<std::uint8_t>(Bits >> (I * 8));
}
} // namespace Feather
