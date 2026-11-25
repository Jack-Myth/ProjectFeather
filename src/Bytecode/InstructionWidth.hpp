#pragma once

#include <Feather/Runtime.hpp>
#include <stdexcept>

namespace Feather {
inline std::size_t OperandWidth(Op Instruction) {
    switch (Instruction) {
    case Op::Const: case Op::GetLocal: case Op::GetGlobal:
    case Op::AddToLocal: case Op::AddConstToLocal:
    case Op::AddConst: case Op::SubConst: case Op::MulConst: case Op::GetField:
    case Op::SetLocal: case Op::StoreLocal: case Op::SetGlobal: case Op::Jump: case Op::JumpIf: return 4;
    case Op::Call: return 2;
    case Op::Null: case Op::True: case Op::False: case Op::Pop:
    case Op::NewObject: case Op::GetMember: case Op::SetMember:
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
    case Op::ToByte: case Op::Negate: case Op::Equal: case Op::Less: case Op::Return: return 0;
    }
    throw std::invalid_argument("unknown opcode");
}
} // namespace Feather
