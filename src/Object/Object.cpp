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

std::size_t NativeTypeIdHash::operator()(const NativeTypeId& Input) const noexcept {
    std::size_t Result = 0xcbf29ce484222325ULL;
    for (auto Byte : Input.Module.Bytes)
        Result = (Result ^ Byte) * 0x100000001b3ULL;
    for (unsigned char Byte : Input.Name)
        Result = (Result ^ Byte) * 0x100000001b3ULL;
    return Result;
}

NativeObjectType::NativeObjectType(Vm& InputMachine, NativeModuleGuid Module,
                                   std::string Name)
    : Machine(InputMachine), Id{Module, std::move(Name)} {
    if (Id.Name.empty()) throw std::invalid_argument("empty native type name");
    (void)Value::String(Id.Name);
}

void NativeObjectType::RegisterObject(const std::shared_ptr<NativeObject>& Input) {
    if (!Input || Input->GetNativeObjectType() != this)
        throw std::invalid_argument("NativeObject belongs to another native type");
    Machine.RegisterNativeObject(Input);
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
    if (Input.IsNumeric() && !std::isnan(Input.AsNumber()))
        return Key{ValueType::Number, {}, Input.AsNumber() == 0 ? 0 : Input.AsNumber()};
    return std::nullopt;
}
std::optional<Value> ScriptObject::GetRaw(const Value& Input) const {
    if (Elements) {
        if (Input.GetType() == ValueType::String) {
            if (Input.AsString() == "Length")
                return Value::Number(static_cast<double>(GetArrayLength()));
            return GetArrayMethod(Input.AsString());
        }
        const auto Index = GetArrayIndex(Input);
        if (!Index) return std::nullopt;
        return (*Elements)[*Index];
    }
    auto Key = ConvertKey(Input);
    if (!Key) return std::nullopt;
    auto Found = Members.find(*Key);
    if (Found == Members.end()) return std::nullopt;
    return Found->second;
}
Value ScriptObject::SetRaw(const Value& InputKey, const Value& Input) {
    if (Elements) return Error("array members are read-only");
    auto Key = ConvertKey(InputKey);
    if (!Key) return Error("invalid ScriptObject key");
    Owner->ValidateOwnedValue(Input);
    Members.insert_or_assign(std::move(*Key), Input);
    return Input;
}
std::optional<std::size_t> ScriptObject::GetArrayIndex(const Value& Key) const {
    if (!Key.IsNumeric()) return std::nullopt;
    const double Index = Key.AsNumber();
    if (!std::isfinite(Index) || Index < 0 || Index >= static_cast<double>(Elements->size()) ||
        std::trunc(Index) != Index)
        return std::nullopt;
    return static_cast<std::size_t>(Index);
}

void ScriptObject::AppendArray(const Value& Input) {
    if (!Elements) throw std::logic_error("AppendArray requires an array");
    Owner->ValidateOwnedValue(Input);
    Elements->push_back(Input);
}

bool ScriptObject::RemoveArrayAt(std::size_t Index) {
    if (!Elements) throw std::logic_error("RemoveArrayAt requires an array");
    if (Index >= Elements->size()) return false;
    Elements->erase(Elements->begin() + static_cast<std::ptrdiff_t>(Index));
    return true;
}

std::size_t ScriptObject::GetArrayLength() const {
    if (!Elements) throw std::logic_error("GetArrayLength requires an array");
    return Elements->size();
}

Value ErrorObject::GetMember(const Value& Key) {
    if (Key.GetType() == ValueType::String && Key.AsString() == "message")
        return Value::String(Message);
    return Error("member does not exist");
}
} // namespace Feather
