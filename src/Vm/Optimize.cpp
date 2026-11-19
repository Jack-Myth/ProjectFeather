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

bool MatchBlock(const Code& Bytes, std::size_t Start, const Module& Program, SlotBlock& Block) {
    auto Pc = Start + LoadSize;
    if (Pc == Bytes.size()) return false;
    Block.Source = Operand(Bytes, Start);
    auto Next = At(Bytes, Pc);
    if (Next == Op::GetLocal || Next == Op::Const) {
        Block.Operand = Operand(Bytes, Pc);
        Pc += LoadSize;
        if (Pc == Bytes.size() || At(Bytes, Pc) != BranchLess) return false;
        if (Next == Op::Const && Program.Constants[Block.Operand].Type != Constant::Kind::Number)
            return false;
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
    if (Bytes.size() - Pc <= LoadSize || At(Bytes, Pc) != Op::SetLocal ||
        At(Bytes, Pc + LoadSize) != Op::Pop) return false;
    Block.Operation = SlotBlock::Kind::Arithmetic;
    Block.Operand = Operand(Bytes, Pc);
    Block.EndPc = Pc + LoadSize + 1;
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
    for (std::size_t Pc = 0; Pc < Bytes.size();) {
        const auto Instruction = static_cast<Op>(Bytes[Pc] & 0x7f);
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
                continue;
            }
        }
        Pc += 1 + OperandWidth(Instruction);
    }
    return Plan;
}

} // namespace Feather
