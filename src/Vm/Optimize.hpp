#pragma once

#include <Feather/Runtime.hpp>

namespace Feather {

// These tags exist only in the VM's private copy of validated bytecode.
constexpr auto BranchLess = static_cast<Op>(0x80 | static_cast<unsigned>(Op::Less));
constexpr auto BranchEqual = static_cast<Op>(0x80 | static_cast<unsigned>(Op::Equal));
constexpr auto OptimizedLocal = static_cast<Op>(0x80 | static_cast<unsigned>(Op::GetLocal));
constexpr std::size_t JumpSize = 1 + sizeof(std::uint32_t);

struct NumericStep {
    Op Operation;
    std::uint32_t Constant;
};

struct SlotBlock {
    enum class Kind { CompareLocal, CompareConstant, Arithmetic };
    Kind Operation;
    std::uint32_t Source;
    // Right operand for comparisons or destination local for arithmetic.
    std::uint32_t Operand;
    std::size_t EndPc;
    std::size_t FalsePc;
    std::size_t FirstStep;
    std::size_t StepCount;
};

struct SlotPlan {
    std::vector<SlotBlock> Blocks;
    std::vector<NumericStep> Steps;
};

SlotPlan OptimizeFunction(FunctionPrototype& Body, const Module& Program);

} // namespace Feather
