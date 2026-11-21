#pragma once
#include <Feather/Runtime.hpp>

namespace Feather {
// Common read interface for fixed-size binary storage shared by native modules.
class FEATHER_API ByteBufferView : public NativeObject {
public:
    explicit ByteBufferView(NativeObjectType& Type) : NativeObject(Type) {}
    virtual const std::string& Data() const = 0;
};
// Shared immutable binary buffer used by stdio and optional Native modules.
// The VM must outlive this factory and its buffers.
class FEATHER_API ByteBufferLibrary final {
public:
    explicit ByteBufferLibrary(Vm& Machine);
    static const NativeTypeId& GetTypeId();
    Value Create(std::string Bytes) const;
private:
    NativeObjectType* Type = nullptr;
};
// The returned storage is valid while Input retains the immutable buffer.
FEATHER_API const std::string* ByteBufferData(const Value& Input);
} // namespace Feather
