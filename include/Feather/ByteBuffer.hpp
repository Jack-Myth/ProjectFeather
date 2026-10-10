#pragma once
#include <Feather/Runtime.hpp>

namespace Feather {
// Shared immutable binary buffer used by stdio and optional Native modules.
// The VM must outlive this factory and its buffers.
class FEATHER_API ByteBufferLibrary final {
public:
    explicit ByteBufferLibrary(Vm& Machine);
    Value Create(std::string Bytes) const;
private:
    NativeObjectType* Type = nullptr;
};
// The returned storage is valid while Input retains the immutable buffer.
FEATHER_API const std::string* ByteBufferData(const Value& Input);
} // namespace Feather