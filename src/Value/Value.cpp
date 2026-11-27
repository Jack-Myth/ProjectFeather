#include <Feather/Runtime.hpp>
#include <Feather/Utf8.hpp>

#include <new>
#include <cmath>
#include <utility>

namespace Feather {

Value Value::Error(std::string Message) {
    return FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}

Value::Value() = default;
Value::Value(ValueType NewType) : Type(NewType) {}
Value::Value(const Value& Other) { CopyFrom(Other); }
Value::Value(Value&& Other) noexcept { MoveFrom(Other); }
Value& Value::operator=(const Value& Other) {
    if (this != &Other) { Value Copy(Other); *this = std::move(Copy); }
    return *this;
}
Value& Value::operator=(Value&& Other) noexcept {
    if (this != &Other) { Destroy(); MoveFrom(Other); }
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
    case ValueType::Byte: Data.ByteValue = Other.Data.ByteValue; break;
    case ValueType::String: new (&Data.Text) std::shared_ptr<const std::string>(Other.Data.Text); break;
    case ValueType::Object:
        if (ScriptReference) Data.Script = Other.Data.Script;
        else new (&Data.Reference) std::shared_ptr<NativeObject>(Other.Data.Reference);
        break;
    }
}
void Value::MoveFrom(Value& Other) noexcept {
    Type = Other.Type;
    ScriptReference = Other.ScriptReference;
    switch (Type) {
    case ValueType::Null: break;
    case ValueType::Bool: Data.Boolean = Other.Data.Boolean; break;
    case ValueType::Number: Data.Numeric = Other.Data.Numeric; break;
    case ValueType::Byte: Data.ByteValue = Other.Data.ByteValue; break;
    case ValueType::String:
        new (&Data.Text) std::shared_ptr<const std::string>(std::move(Other.Data.Text));
        Other.Data.Text.~shared_ptr();
        break;
    case ValueType::Object:
        if (ScriptReference) Data.Script = Other.Data.Script;
        else {
            new (&Data.Reference) std::shared_ptr<NativeObject>(std::move(Other.Data.Reference));
            Other.Data.Reference.~shared_ptr();
        }
        break;
    }
    Other.Type = ValueType::Null;
    Other.ScriptReference = false;
}
Value Value::Bool(bool Input) { Value Result(ValueType::Bool); Result.Data.Boolean = Input; return Result; }
Value Value::Number(double Input) { Value Result(ValueType::Number); Result.Data.Numeric = Input; return Result; }
Value Value::Byte(double Input) {
    if (!std::isfinite(Input) || Input < 0 || Input > 255 || std::trunc(Input) != Input)
        throw std::invalid_argument("byte requires an integer in [0, 255]");
    Value Result(ValueType::Byte);
    Result.Data.ByteValue = static_cast<std::uint8_t>(Input);
    return Result;
}
std::uint8_t Value::AsByte() const {
    if (Type != ValueType::Byte) throw std::logic_error("value is not byte");
    return Data.ByteValue;
}
Value Value::String(std::string Input) {
    ValidateUtf8(Input);
    auto Text = std::make_shared<const std::string>(std::move(Input));
    Value Result(ValueType::String);
    new (&Result.Data.Text) std::shared_ptr<const std::string>(std::move(Text));
    return Result;
}
Value Value::FromObject(std::shared_ptr<NativeObject> Input) {
    if (!Input) throw std::invalid_argument("null object reference");
    if (Input->GetObjectType() == ObjectType::Script)
        throw std::invalid_argument("ScriptObject must come from a VM heap");
    Value Result(ValueType::Object);
    new (&Result.Data.Reference) std::shared_ptr<NativeObject>(std::move(Input));
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
    if (Type == ValueType::Byte) return Data.ByteValue;
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
const std::shared_ptr<NativeObject>& Value::AsNativeObject() const {
    if (Type != ValueType::Object || ScriptReference)
        throw std::logic_error("value is not NativeObject");
    return Data.Reference;
}
bool Value::IsTruthy() const {
    switch (Type) {
    case ValueType::Null: return false;
    case ValueType::Bool: return Data.Boolean;
    case ValueType::Number: return Data.Numeric != 0;
    case ValueType::Byte: return Data.ByteValue != 0;
    case ValueType::String: return !Data.Text->empty();
    case ValueType::Object: return true;
    }
    return false;
}
} // namespace Feather
