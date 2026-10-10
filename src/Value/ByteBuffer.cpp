#include <Feather/ByteBuffer.hpp>
#include <Feather/StdIo.hpp>

#include <cmath>
#include <utility>

namespace Feather {
namespace BufferDetail {
Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}
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
            if (!A.empty()) return Error("ByteBuffer.ToString expects no arguments");
            try { return Value::String(*Bytes); }
            catch (const std::invalid_argument&) { return Error("buffer is not valid UTF-8"); }
        }
        std::uint64_t Index = 0;
        if (A.size() != 1 || !Integer(A[0], Index, Bytes->size()) || Index == Bytes->size())
            return Error("ByteBuffer.Get expects an in-range byte index");
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
        : NativeObjectType(Machine, StdIoModuleGuid, "ByteBuffer.Method") {}
    std::shared_ptr<BufferMethod> Create(std::shared_ptr<const std::string> Bytes,
                                         BufferOperation Operation) {
        return CreateObject<BufferMethod>(std::move(Bytes), Operation);
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        auto* Method = dynamic_cast<const BufferMethod*>(&Input);
        if (!Method || Method->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid ByteBuffer method object");
        std::vector<std::uint8_t> Result;
        Result.reserve(Method->Data().size() + 1);
        Result.push_back(static_cast<std::uint8_t>(Method->GetOperation()));
        Result.insert(Result.end(), Method->Data().begin(), Method->Data().end());
        return Result;
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || Payload.empty() || Payload[0] > 1)
            throw std::invalid_argument("invalid ByteBuffer method payload");
        auto Bytes = std::make_shared<const std::string>(
            reinterpret_cast<const char*>(Payload.data() + 1), Payload.size() - 1);
        return Create(std::move(Bytes), static_cast<BufferOperation>(Payload[0]));
    }
};

class ByteBufferType final : public NativeObjectType {
public:
    ByteBufferType(Vm& Machine, BufferMethodType& MethodType)
        : NativeObjectType(Machine, StdIoModuleGuid, "ByteBuffer"), MethodType(MethodType) {}
    std::shared_ptr<ByteBuffer> Create(std::string Bytes) {
        return CreateObject<ByteBuffer>(std::move(Bytes), &MethodType);
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        auto* Buffer = dynamic_cast<const ByteBuffer*>(&Input);
        if (!Buffer || Buffer->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid ByteBuffer object");
        return {Buffer->Data().begin(), Buffer->Data().end()};
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1) throw std::invalid_argument("unsupported ByteBuffer version");
        std::string Bytes;
        if (!Payload.empty())
            Bytes.assign(reinterpret_cast<const char*>(Payload.data()), Payload.size());
        return Create(std::move(Bytes));
    }
private:
    BufferMethodType& MethodType;
};

Value ByteBuffer::GetMember(const Value& Key) {
    if (Key.GetType() != ValueType::String) return Error("member name must be a string");
    if (Key.AsString() == "Length") return Value::Number(static_cast<double>(Bytes->size()));
    if (Key.AsString() == "ToString")
        return Value::FromObject(MethodType->Create(Bytes, BufferOperation::ToString));
    if (Key.AsString() == "Get")
        return Value::FromObject(MethodType->Create(Bytes, BufferOperation::Get));
    return Error("unknown ByteBuffer member");
}

} // namespace BufferDetail

ByteBufferLibrary::ByteBufferLibrary(Vm& Machine) {
    auto* Methods = Machine.FindNativeObjectType({StdIoModuleGuid, "ByteBuffer.Method"});
    if (!Methods) Methods = &Machine.CreateNativeObjectType<BufferDetail::BufferMethodType>();
    auto* TypedMethods = dynamic_cast<BufferDetail::BufferMethodType*>(Methods);
    if (!TypedMethods) throw std::invalid_argument("ByteBuffer method type collision");
    Type = Machine.FindNativeObjectType({StdIoModuleGuid, "ByteBuffer"});
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