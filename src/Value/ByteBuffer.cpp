#include <Feather/ByteBuffer.hpp>

#include <cmath>
#include <utility>

namespace Feather {
namespace BufferDetail {
// Existing VM message archives contain this GUID and the name "ByteBuffer".
constexpr NativeModuleGuid Guid{{
    0x2d, 0x33, 0xd7, 0x69, 0x4a, 0x83, 0x46, 0xc0,
    0xb5, 0x87, 0x65, 0x72, 0x47, 0x4f, 0x1f, 0xd2}};
constexpr char MethodName[] = "ByteBuffer.Method";

bool Integer(const Value& Input, std::uint64_t& Result, std::uint64_t Limit) {
    if (!Input.IsNumeric()) return false;
    double Number = Input.AsNumber();
    if (!std::isfinite(Number) || Number < 0 || Number > static_cast<double>(Limit) ||
        std::trunc(Number) != Number) return false;
    Result = static_cast<std::uint64_t>(Number);
    return true;
}

class BufferMethodType;

class ByteBuffer final : public NativeObject {
public:
    ByteBuffer(NativeObjectType& Type, std::string Input,
               BufferMethodType* InputMethodType)
        : NativeObject(Type), Bytes(std::make_shared<const std::string>(std::move(Input))),
          MethodType(InputMethodType) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    const std::string& Data() const { return *Bytes; }
    Value GetMember(const Value& Key) override;
private:
    std::shared_ptr<const std::string> Bytes;
    BufferMethodType* MethodType = nullptr;
};

enum class BufferOperation : std::uint8_t { ToString, Get };

class BufferMethod final : public NativeObject {
public:
    BufferMethod(NativeObjectType& Type, std::shared_ptr<const std::string> Bytes,
                 BufferOperation Operation)
        : NativeObject(Type), Bytes(std::move(Bytes)), Operation(Operation) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& A) override {
        if (Operation == BufferOperation::ToString) {
            if (!A.empty()) return Value::Error("ByteBuffer.ToString expects no arguments");
            try { return Value::String(*Bytes); }
            catch (const std::invalid_argument&) { return Value::Error("buffer is not valid UTF-8"); }
        }
        std::uint64_t Index = 0;
        if (A.size() != 1 || !Integer(A[0], Index, Bytes->size()) || Index == Bytes->size())
            return Value::Error("ByteBuffer.Get expects an in-range byte index");
        return Value::Byte(static_cast<unsigned char>((*Bytes)[static_cast<std::size_t>(Index)]));
    }
    const std::string& Data() const { return *Bytes; }
    BufferOperation GetOperation() const { return Operation; }
private:
    std::shared_ptr<const std::string> Bytes;
    BufferOperation Operation;
};

class BufferMethodType final : public NativeObjectType {
public:
    explicit BufferMethodType(Vm& Machine)
        : NativeObjectType(Machine, Guid, MethodName) {}
    std::shared_ptr<BufferMethod> Create(std::shared_ptr<const std::string> Bytes,
                                         BufferOperation Operation) {
        return CreateObject<BufferMethod>(std::move(Bytes), Operation);
    }

};

class ByteBufferType final : public NativeObjectType {
public:
    ByteBufferType(Vm& Machine, BufferMethodType& MethodType)
        : NativeObjectType(Machine, Guid, ByteBufferLibrary::GetTypeId().Name), MethodType(MethodType) {}
    std::shared_ptr<ByteBuffer> Create(std::string Bytes) {
        return CreateObject<ByteBuffer>(std::move(Bytes), &MethodType);
    }

private:
    BufferMethodType& MethodType;
};

Value ByteBuffer::GetMember(const Value& Key) {
    if (Key.GetType() != ValueType::String) return Value::Error("member name must be a string");
    if (Key.AsString() == "Length") return Value::Number(static_cast<double>(Bytes->size()));
    if (Key.AsString() == "ToString")
        return Value::FromObject(MethodType->Create(Bytes, BufferOperation::ToString));
    if (Key.AsString() == "Get")
        return Value::FromObject(MethodType->Create(Bytes, BufferOperation::Get));
    return Value::Error("unknown ByteBuffer member");
}

} // namespace BufferDetail

const NativeTypeId& ByteBufferLibrary::GetTypeId() {
    static const NativeTypeId Id{BufferDetail::Guid, "ByteBuffer"};
    return Id;
}

ByteBufferLibrary::ByteBufferLibrary(Vm& Machine) {
    auto* Methods = Machine.FindNativeObjectType({BufferDetail::Guid, BufferDetail::MethodName});
    if (!Methods) Methods = &Machine.CreateNativeObjectType<BufferDetail::BufferMethodType>();
    auto* TypedMethods = dynamic_cast<BufferDetail::BufferMethodType*>(Methods);
    if (!TypedMethods) throw std::invalid_argument("ByteBuffer method type collision");
    Type = Machine.FindNativeObjectType(GetTypeId());
    if (!Type) Type = &Machine.CreateNativeObjectType<BufferDetail::ByteBufferType>(*TypedMethods);
    if (!dynamic_cast<BufferDetail::ByteBufferType*>(Type))
        throw std::invalid_argument("ByteBuffer type collision");
}
Value ByteBufferLibrary::Create(std::string Bytes) const {
    return Value::FromObject(
        static_cast<BufferDetail::ByteBufferType*>(Type)->Create(std::move(Bytes)));
}
const std::string* ByteBufferData(const Value& Input) {
    if (Input.GetType() != ValueType::Object || Input.IsScriptObject()) return nullptr;
    auto* Buffer = dynamic_cast<BufferDetail::ByteBuffer*>(Input.AsObject());
    return Buffer ? &Buffer->Data() : nullptr;
}
} // namespace Feather
