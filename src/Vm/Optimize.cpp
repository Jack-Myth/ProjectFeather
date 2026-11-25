#include "Optimize.hpp"
#include "../Bytecode/InstructionWidth.hpp"

#include <algorithm>
#include <bit>

namespace Feather {
namespace {
using Code = std::vector<std::uint8_t>;
constexpr std::size_t LoadSize = 1 + sizeof(std::uint32_t);

Op At(const Code& Bytes, std::size_t Pc) { return static_cast<Op>(Bytes[Pc]); }

std::uint32_t Operand(const Code& Bytes, std::size_t Pc) {
    return std::uint32_t(Bytes[Pc+1]) | (std::uint32_t(Bytes[Pc+2]) << 8) |
           (std::uint32_t(Bytes[Pc+3]) << 16) | (std::uint32_t(Bytes[Pc+4]) << 24);
}
std::size_t JumpTarget(const Code& Bytes, std::size_t Pc) {
    return static_cast<std::size_t>(static_cast<std::int64_t>(Pc + JumpSize) +
                                    std::bit_cast<std::int32_t>(Operand(Bytes, Pc)));
}
bool IsArithmetic(Op Instruction) {
    return Instruction == Op::MulConst || Instruction == Op::AddConst || Instruction == Op::SubConst;
}

// Starting after [receiver, key], follow the RHS to the store. Bound the
// lookahead so nested expressions cannot make load-time optimization quadratic.
std::optional<std::size_t> MemberStore(const Code& Bytes, std::size_t Pc) {
    int Values = 0;
    for (unsigned I = 0; I < 32 && Pc < Bytes.size(); ++I) {
        const auto OpCode = At(Bytes, Pc);
        switch (OpCode) {
        case Op::GetLocal: case Op::GetGlobal: case Op::Const: case Op::Null:
        case Op::True: case Op::False: case Op::NewObject: ++Values; break;
        case Op::GetField: case Op::AddConst: case Op::SubConst: case Op::MulConst:
        case Op::Negate: case Op::ToByte: case Op::SetLocal: case Op::SetGlobal:
            if (Values < 1) return {};
            break;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::GetMember:
        case Op::Equal: case Op::Less:
            if (Values < 2) return {};
            --Values;
            break;
        case Op::SetMember:
            if (Values == 1) return Pc;
            if (Values < 3) return {};
            Values -= 2;
            break;
        case Op::Call: {
            const int Args = Bytes[Pc + 1] | (Bytes[Pc + 2] << 8);
            if (Values <= Args) return {};
            Values -= Args;
            break;
        }
        default: return {};
        }
        Pc += 1 + OperandWidth(OpCode);
    }
    return {};
}

std::optional<std::size_t> LocalStoreEnd(const Code& Bytes, std::size_t Pc) {
    if (Pc >= Bytes.size()) return {};
    if (At(Bytes, Pc) == Op::StoreLocal) return Pc + LoadSize;
    if (Bytes.size() - Pc > LoadSize && At(Bytes, Pc) == Op::SetLocal &&
        At(Bytes, Pc + LoadSize) == Op::Pop) return Pc + LoadSize + 1;
    return {};
}

bool MatchBlock(const Code& Bytes, std::size_t Start, const Module& Program, SlotBlock& Block) {
    auto Pc = Start + LoadSize;
    if (Pc == Bytes.size()) return false;
    Block.Source = Operand(Bytes, Start);
    auto Next = At(Bytes, Pc);
    if (Next == Op::GetLocal || Next == Op::Const) {
        Block.Operand = Operand(Bytes, Pc);
        Pc += LoadSize;
        if (Pc == Bytes.size()) return false;
        if (Next == Op::Const && Program.Constants[Block.Operand].Type != Constant::Kind::Number)
            return false;
        const auto Binary = At(Bytes, Pc);
        if (Binary == Op::Add || Binary == Op::Sub || Binary == Op::Mul || Binary == Op::Div) {
            ++Pc;
            const auto StoreEnd = LocalStoreEnd(Bytes, Pc);
            if (!StoreEnd) return false;
            Block.Operation = Next == Op::GetLocal ? SlotBlock::Kind::BinaryLocal : SlotBlock::Kind::BinaryConstant;
            Block.BinaryOp = Binary;
            Block.Destination = Operand(Bytes, Pc);
            Block.EndPc = *StoreEnd;
            return true;
        }
        if (Binary != BranchLess) return false;
        Block.Operation = Next == Op::GetLocal ? SlotBlock::Kind::CompareLocal : SlotBlock::Kind::CompareConstant;
        Block.EndPc = Pc + 1 + 2 * JumpSize;
        Block.FalsePc = JumpTarget(Bytes, Pc + 1 + JumpSize);
        return true;
    }
    if (!IsArithmetic(Next)) return false;
    while (Pc < Bytes.size() && IsArithmetic(At(Bytes, Pc))) {
        ++Block.StepCount;
        Pc += LoadSize;
    }
    const auto StoreEnd = LocalStoreEnd(Bytes, Pc);
    if (!StoreEnd) return false;
    Block.Operation = SlotBlock::Kind::Arithmetic;
    Block.Operand = Operand(Bytes, Pc);
    Block.EndPc = *StoreEnd;
    return true;
}
} // namespace

SlotPlan OptimizeFunction(FunctionPrototype& Body, const Module& Program) {
    auto& Bytes = Body.Code;
    std::vector<std::size_t> Targets;
    for (std::size_t Pc = 0; Pc < Bytes.size();) {
        const auto Instruction = At(Bytes, Pc);
        if (Instruction == Op::Jump || Instruction == Op::JumpIf)
            Targets.push_back(JumpTarget(Bytes, Pc));
        if ((Instruction == Op::Less || Instruction == Op::Equal) && Bytes.size()-Pc > 2*JumpSize &&
            At(Bytes, Pc+1) == Op::JumpIf && At(Bytes, Pc+1+JumpSize) == Op::Jump &&
            Operand(Bytes, Pc+1) == JumpSize) {
            Bytes[Pc] = static_cast<std::uint8_t>(Instruction == Op::Less ? BranchLess : BranchEqual);
        }
        Pc += 1 + OperandWidth(Instruction);
    }
    std::sort(Targets.begin(), Targets.end());
    SlotPlan Plan;
    std::size_t LoadStart = 0;
    for (std::size_t Pc = 0; Pc < Bytes.size();) {
        const auto Instruction = static_cast<Op>(Bytes[Pc] & 0x7f);
        if (Instruction == Op::GetLocal && Bytes.size() - Pc > LoadSize) {
            const auto Next = Pc + LoadSize;
            const auto Entry = std::upper_bound(Targets.begin(), Targets.end(), Pc);
            if (At(Bytes, Next) == Op::GetField && (Entry == Targets.end() || *Entry > Next)) {
                Bytes[Pc] = static_cast<std::uint8_t>(ReadLocalField);
                LoadStart = Pc = Next + LoadSize;
                continue;
            }
            const auto KeyOp = At(Bytes, Next);
            if (KeyOp == Op::GetLocal || KeyOp == Op::Const) {
                const bool Field = KeyOp == Op::Const &&
                    Program.Constants[Operand(Bytes, Next)].Type == Constant::Kind::String;
                const bool Index = KeyOp == Op::GetLocal ||
                    Program.Constants[Operand(Bytes, Next)].Type == Constant::Kind::Number ||
                    Program.Constants[Operand(Bytes, Next)].Type == Constant::Kind::Byte;
                auto AfterKey = Next + LoadSize;
                if (Index && AfterKey < Bytes.size() &&
                    (At(Bytes, AfterKey) == Op::SubConst || At(Bytes, AfterKey) == Op::AddConst))
                    AfterKey += LoadSize;
                if (Index && AfterKey < Bytes.size() && At(Bytes, AfterKey) == Op::GetMember &&
                    (Entry == Targets.end() || *Entry > AfterKey)) {
                    Bytes[Pc] = static_cast<std::uint8_t>(ReadLocalIndex);
                    LoadStart = Pc = AfterKey + 1;
                    continue;
                }
                auto Store = (Field || Index) ? MemberStore(Bytes, AfterKey) : std::nullopt;
                if (Store && (Entry == Targets.end() || *Entry > *Store)) {
                    Bytes[Pc] = static_cast<std::uint8_t>(Field ? PrepareLocalField : PrepareLocalIndex);
                    Bytes[*Store] = static_cast<std::uint8_t>(StoreLocalMember);
                    LoadStart = Pc = AfterKey;
                    continue;
                }
            }
        }
        if (Instruction == Op::GetGlobal) LoadStart = Pc;
        if (Instruction == Op::Call) {
            const auto Resume = Pc + 1 + sizeof(std::uint16_t);
            const auto Count = std::size_t(Bytes[Pc + 1]) | (std::size_t(Bytes[Pc + 2]) << 8);
            // A trailing run of local loads can include operands of an enclosing expression.
            // The arity identifies the callee within that run. A global may only be the callee.
            if ((Pc - LoadStart) / LoadSize >= Count + 1 && Plan.Calls.size() <= UINT32_MAX) {
                const auto Start = Pc - (Count + 1) * LoadSize;
                const auto Interior = std::upper_bound(Targets.begin(), Targets.end(), Start);
                if (Interior == Targets.end() || *Interior > Pc) {
                    SlotCall Call{At(Bytes, Start) == Op::GetGlobal, Operand(Bytes, Start), {}, Resume};
                    Call.Arguments.reserve(Count);
                    for (auto Arg = Start + LoadSize; Arg < Pc; Arg += LoadSize)
                        Call.Arguments.push_back(Operand(Bytes, Arg));
                    const auto Id = static_cast<std::uint32_t>(Plan.Calls.size());
                    Plan.Calls.push_back(std::move(Call));
                    Bytes[Start] = static_cast<std::uint8_t>(CallFromSlots);
                    for (unsigned I = 0; I < sizeof(Id); ++I)
                        Bytes[Start + 1 + I] = static_cast<std::uint8_t>(Id >> (8 * I));
                }
            }
            if (Bytes.size() - Resume > LoadSize && At(Bytes, Resume) == Op::SetLocal &&
                At(Bytes, Resume + LoadSize) == Op::Pop)
                Bytes[Resume] = static_cast<std::uint8_t>(ReturnDestination);
        }
        SlotBlock Block{};
        if (Instruction == Op::GetLocal && MatchBlock(Bytes, Pc, Program, Block)) {
            auto Entry = std::upper_bound(Targets.begin(), Targets.end(), Pc);
            if ((Entry == Targets.end() || *Entry >= Block.EndPc) && Plan.Blocks.size() <= UINT32_MAX) {
                Block.FirstStep = Plan.Steps.size();
                for (std::size_t I = 0; I < Block.StepCount; ++I) {
                    auto StepPc = Pc + LoadSize * (I + 1);
                    Plan.Steps.push_back({At(Bytes, StepPc), Operand(Bytes, StepPc)});
                }
                // Preserve original PCs and interior instructions for debugger and slow-path execution.
                auto Id = static_cast<std::uint32_t>(Plan.Blocks.size());
                Bytes[Pc] = static_cast<std::uint8_t>(OptimizedLocal);
                for (unsigned I = 0; I < sizeof(Id); ++I)
                    Bytes[Pc+1+I] = static_cast<std::uint8_t>(Id >> (8*I));
                Plan.Blocks.push_back(Block);
                Pc = Block.EndPc;
                LoadStart = Pc;
                continue;
            }
        }
        Pc += 1 + OperandWidth(Instruction);
        if (Instruction != Op::GetLocal && Instruction != Op::GetGlobal) LoadStart = Pc;
    }
    return Plan;
}

} // namespace Feather
