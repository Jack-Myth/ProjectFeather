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
                                   std::string Name, std::uint32_t InputSnapshotVersion)
    : Machine(InputMachine), Id{Module, std::move(Name)},
      SnapshotVersion(InputSnapshotVersion) {
    if (Id.Name.empty()) throw std::invalid_argument("empty native type name");
    (void)Value::String(Id.Name);
    if (SnapshotVersion == 0) throw std::invalid_argument("native snapshot version is zero");
}

std::vector<std::uint8_t> NativeObjectType::Serialize(const NativeObject& Input) const {
    if (Input.GetNativeObjectType() != this)
        throw std::invalid_argument("NativeObject belongs to another native type");
    return {};
}

void NativeObjectType::RegisterObject(const std::shared_ptr<NativeObject>& Input) {
    if (!Input || Input->GetNativeObjectType() != this)
        throw std::invalid_argument("NativeObject belongs to another native type");
    Machine.RegisterNativeObject(Input);
}

std::shared_ptr<NativeObject> NativeSingletonType::Deserialize(
    std::uint32_t StoredVersion, std::span<const std::uint8_t> Payload) {
    if (StoredVersion != GetSnapshotVersion() || !Payload.empty())
        throw std::invalid_argument("invalid native singleton payload");
    auto Result = Canonical.lock();
    if (!Result) throw std::logic_error("native singleton is unavailable");
    return Result;
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
    Owner->ValidateOwnedValue(Input);
    Members.insert_or_assign(std::move(*Key), Input);
    return Input;
}
Value ErrorObject::GetMember(const Value& Key) {
    if (Key.GetType() == ValueType::String && Key.AsString() == "message")
        return Value::String(Message);
    return Error("member does not exist");
}
} // namespace Feather
