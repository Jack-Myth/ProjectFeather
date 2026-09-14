#include <Feather/Runtime.hpp>

#include <bit>
#include <cmath>
#include <functional>

namespace Feather {
namespace {
Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}
}

Value NativeObject::Call(const std::vector<Value>&) { return Error("object is not callable"); }
Value NativeObject::GetMember(const Value&) { return Error("member does not exist"); }
Value NativeObject::SetMember(const Value&, const Value&) { return Error("member is not writable"); }
void NativeObject::SetGcVisibleMember(std::string Name, Value Input) {
    GcVisibleMembers.insert_or_assign(std::move(Name), std::move(Input));
}
void NativeObject::RemoveGcVisibleMember(const std::string& Name) {
    GcVisibleMembers.erase(Name);
}

ScriptObject::ScriptObject(Vm* InputOwner, ScriptObject* InputMeta)
    : Owner(InputOwner), MetaObject(InputMeta) {}

bool ScriptObject::Key::operator==(const Key& Other) const {
    if (Type != Other.Type) return false;
    return Type == ValueType::String ? Text == Other.Text : Number == Other.Number;
}
std::size_t ScriptObject::KeyHash::operator()(const Key& Input) const {
    if (Input.Type == ValueType::String)
        return std::hash<std::string>{}(Input.Text) ^ 0x517cc1b727220a95ULL;
    double Normalized = Input.Number == 0 ? 0 : Input.Number;
    return std::hash<std::uint64_t>{}(std::bit_cast<std::uint64_t>(Normalized));
}
std::optional<ScriptObject::Key> ScriptObject::ConvertKey(const Value& Input) {
    if (Input.GetType() == ValueType::String)
        return Key{ValueType::String, Input.AsString()};
    if (Input.GetType() == ValueType::Number && !std::isnan(Input.AsNumber()))
        return Key{ValueType::Number, {}, Input.AsNumber() == 0 ? 0 : Input.AsNumber()};
    return std::nullopt;
}
std::optional<Value> ScriptObject::GetRaw(const Value& Input) const {
    auto Key = ConvertKey(Input);
    if (!Key) return std::nullopt;
    auto Found = Members.find(*Key);
    if (Found == Members.end()) return std::nullopt;
    return Found->second;
}
Value ScriptObject::SetRaw(const Value& InputKey, const Value& Input) {
    auto Key = ConvertKey(InputKey);
    if (!Key) return Error("invalid ScriptObject key");
    if (Input.IsScriptObject() && !Owner->OwnsScript(Input.AsScriptObject()))
        throw std::invalid_argument("foreign ScriptObject cannot be stored");
    Members.insert_or_assign(std::move(*Key), Input);
    return Input;
}
Value ErrorObject::GetMember(const Value& Key) {
    if (Key.GetType() == ValueType::String && Key.AsString() == "message")
        return Value::String(Message);
    return Error("member does not exist");
}
} // namespace Feather
