#include <Feather/Runtime.hpp>

#include <new>
#include <utility>

namespace Feather {
namespace {
bool IsValidUtf8(std::string_view Input) {
    for (std::size_t I = 0; I < Input.size();) {
        auto Lead = static_cast<unsigned char>(Input[I]);
        if (Lead < 0x80) { ++I; continue; }
        std::size_t Width = Lead >= 0xC2 && Lead <= 0xDF ? 2 :
                            Lead >= 0xE0 && Lead <= 0xEF ? 3 :
                            Lead >= 0xF0 && Lead <= 0xF4 ? 4 : 0;
        if (Width == 0 || I + Width > Input.size()) return false;
        for (std::size_t J = 1; J < Width; ++J) {
            auto Byte = static_cast<unsigned char>(Input[I + J]);
            if (Byte < 0x80 || Byte > 0xBF) return false;
        }
        auto Second = static_cast<unsigned char>(Input[I + 1]);
        if ((Lead == 0xE0 && Second < 0xA0) ||
            (Lead == 0xED && Second > 0x9F) ||
            (Lead == 0xF0 && Second < 0x90) ||
            (Lead == 0xF4 && Second > 0x8F)) return false;
        I += Width;
    }
    return true;
}
} // namespace

Value::Value() = default;
Value::Value(ValueType NewType) : Type(NewType) {}
Value::Value(const Value& Other) { CopyFrom(Other); }
Value::Value(Value&& Other) noexcept { CopyFrom(Other); Other.Destroy(); }
Value& Value::operator=(const Value& Other) {
    if (this != &Other) { Value Copy(Other); *this = std::move(Copy); }
    return *this;
}
Value& Value::operator=(Value&& Other) noexcept {
    if (this != &Other) { Destroy(); CopyFrom(Other); Other.Destroy(); }
    return *this;
}
Value::~Value() { Destroy(); }

void Value::Destroy() {
    if (Type == ValueType::String) Data.Text.~shared_ptr();
    if (Type == ValueType::Object && !ScriptReference) Data.Reference.~shared_ptr();
    Type = ValueType::Null;
    ScriptReference = false;
}
void Value::CopyFrom(const Value& Other) {
    Type = Other.Type;
    ScriptReference = Other.ScriptReference;
    switch (Type) {
    case ValueType::Null: break;
    case ValueType::Bool: Data.Boolean = Other.Data.Boolean; break;
    case ValueType::Number: Data.Numeric = Other.Data.Numeric; break;
    case ValueType::String: new (&Data.Text) std::shared_ptr<const std::string>(Other.Data.Text); break;
    case ValueType::Object:
        if (ScriptReference) Data.Script = Other.Data.Script;
        else new (&Data.Reference) std::shared_ptr<Object>(Other.Data.Reference);
        break;
    }
}
Value Value::Bool(bool Input) { Value Result(ValueType::Bool); Result.Data.Boolean = Input; return Result; }
Value Value::Number(double Input) { Value Result(ValueType::Number); Result.Data.Numeric = Input; return Result; }
Value Value::String(std::string Input) {
    if (!IsValidUtf8(Input)) throw std::invalid_argument("invalid UTF-8 string");
    auto Text = std::make_shared<const std::string>(std::move(Input));
    Value Result(ValueType::String);
    new (&Result.Data.Text) std::shared_ptr<const std::string>(std::move(Text));
    return Result;
}
Value Value::FromObject(std::shared_ptr<Object> Input) {
    if (!Input) throw std::invalid_argument("null object reference");
    if (Input->GetObjectType() == ObjectType::Script)
        throw std::invalid_argument("ScriptObject must come from a VM heap");
    Value Result(ValueType::Object);
    new (&Result.Data.Reference) std::shared_ptr<Object>(std::move(Input));
    return Result;
}
Value Value::FromScript(ScriptObject* Input) {
    if (!Input) throw std::invalid_argument("null ScriptObject reference");
    Value Result(ValueType::Object);
    Result.ScriptReference = true;
    Result.Data.Script = Input;
    return Result;
}
bool Value::AsBool() const {
    if (Type != ValueType::Bool) throw std::logic_error("value is not bool");
    return Data.Boolean;
}
double Value::AsNumber() const {
    if (Type != ValueType::Number) throw std::logic_error("value is not number");
    return Data.Numeric;
}
const std::string& Value::AsString() const {
    if (Type != ValueType::String) throw std::logic_error("value is not string");
    return *Data.Text;
}
Object* Value::AsObject() const {
    if (Type != ValueType::Object) throw std::logic_error("value is not object");
    return ScriptReference ? static_cast<Object*>(Data.Script) : Data.Reference.get();
}
ObjectType Value::GetObjectType() const { return AsObject()->GetObjectType(); }
bool Value::IsError() const {
    return Type == ValueType::Object && GetObjectType() == ObjectType::Error;
}
ScriptObject* Value::AsScriptObject() const {
    if (Type != ValueType::Object || !ScriptReference)
        throw std::logic_error("value is not ScriptObject");
    return Data.Script;
}
const std::shared_ptr<Object>& Value::AsNativeObject() const {
    if (Type != ValueType::Object || ScriptReference)
        throw std::logic_error("value is not NativeObject");
    return Data.Reference;
}
bool Value::IsTruthy() const {
    switch (Type) {
    case ValueType::Null: return false;
    case ValueType::Bool: return Data.Boolean;
    case ValueType::Number: return Data.Numeric != 0;
    case ValueType::String: return !Data.Text->empty();
    case ValueType::Object: return true;
    }
    return false;
}
} // namespace Feather
