#include <Feather/NativeModule.hpp>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace {
using namespace Feather;
constexpr NativeModuleGuid Guid{{0x48,0x31,0x99,0x28,0x13,0xd2,0x49,0x19,0xa4,0x43,0x0d,0x63,0xca,0x25,0x93,0x12}};
Value Error(std::string message) { return Value::FromObject(std::make_shared<ErrorObject>(std::move(message))); }
class Function final : public NativeObject {
public:
    using Body = std::function<Value(const std::vector<Value>&)>;
    Function(NativeObjectType& type, Body body) : NativeObject(type), body(std::move(body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& args) override { return body(args); }
private: Body body;
};
class Namespace final : public NativeObject {
public:
    explicit Namespace(NativeObjectType& type) : NativeObject(type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    std::unordered_map<std::string, Value> members;
    Value GetMember(const Value& key) override {
        if (key.GetType() != ValueType::String) return Error("text member must be string");
        auto found = members.find(key.AsString());
        return found == members.end() ? Error("unknown text method") : found->second;
    }
};
std::vector<std::size_t> Offsets(const std::string& s) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) result.push_back(i);
    result.push_back(s.size());
    return result;
}
Value Create(const NativeModuleContext& context) {
    auto& type = context.Machine.CreateNativeObjectType<NativeSingletonType>(Guid, "Module");
    auto module = type.Create<Namespace>();
    auto add = [&](const char* name, Function::Body body) {
        auto& fn = context.Machine.CreateNativeObjectType<NativeSingletonType>(Guid, name);
        module->members.emplace(name, Value::FromObject(fn.Create<Function>(std::move(body))));
    };
    add("Length", [](const std::vector<Value>& a) {
        if (a.size() != 1 || a[0].GetType() != ValueType::String) return Error("text.Length expects string");
        return Value::Number(static_cast<double>(Offsets(a[0].AsString()).size() - 1));
    });
    add("Slice", [](const std::vector<Value>& a) {
        if (a.size() != 3 || a[0].GetType() != ValueType::String || !a[1].IsNumeric() || !a[2].IsNumeric())
            return Error("text.Slice expects string, start, count");
        auto offsets = Offsets(a[0].AsString());
        auto start = a[1].AsNumber(), count = a[2].AsNumber();
        auto length = offsets.size() - 1;
        if (!std::isfinite(start) || !std::isfinite(count) || start < 0 || count < 0 ||
            std::trunc(start) != start || std::trunc(count) != count || start > length || count > length - start)
            return Error("text.Slice range out of bounds");
        auto first = static_cast<std::size_t>(start), last = static_cast<std::size_t>(start + count);
        return Value::String(a[0].AsString().substr(offsets[first], offsets[last] - offsets[first]));
    });
    for (bool after : {false, true}) {
        add(after ? "After" : "Before", [after](const std::vector<Value>& a) {
            if (a.size() != 2 || a[0].GetType() != ValueType::String || a[1].GetType() != ValueType::String || a[1].AsString().empty())
                return Error("text.Before/After expects string and nonempty delimiter");
            const auto& value = a[0].AsString(); const auto& delimiter = a[1].AsString();
            auto at = value.find(delimiter);
            if (at == std::string::npos) return after ? Value::String("") : a[0];
            return Value::String(after ? value.substr(at + delimiter.size()) : value.substr(0, at));
        });
    }
    return Value::FromObject(module);
}
}
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor descriptor{Guid, "text", &Create};
    return &descriptor;
}
