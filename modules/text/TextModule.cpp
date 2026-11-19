#include <Feather/NativeModule.hpp>
#include <cmath>

namespace {
using namespace Feather;
constexpr NativeModuleGuid Guid{{0x48,0x31,0x99,0x28,0x13,0xd2,0x49,0x19,0xa4,0x43,0x0d,0x63,0xca,0x25,0x93,0x12}};
std::vector<std::size_t> Offsets(const std::string& s) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) result.push_back(i);
    result.push_back(s.size());
    return result;
}
Value Create(const NativeModuleContext& context) {
    NativeModuleBuilder Module(context.Machine, Guid, "text");
    Module.AddFunction("Length", [](const std::vector<Value>& a) {
        if (a.size() != 1 || a[0].GetType() != ValueType::String) return Value::Error("text.Length expects string");
        return Value::Number(static_cast<double>(Offsets(a[0].AsString()).size() - 1));
    });
    Module.AddFunction("Slice", [](const std::vector<Value>& a) {
        if (a.size() != 3 || a[0].GetType() != ValueType::String || !a[1].IsNumeric() || !a[2].IsNumeric())
            return Value::Error("text.Slice expects string, start, count");
        auto offsets = Offsets(a[0].AsString());
        auto start = a[1].AsNumber(), count = a[2].AsNumber();
        auto length = offsets.size() - 1;
        if (!std::isfinite(start) || !std::isfinite(count) || start < 0 || count < 0 ||
            std::trunc(start) != start || std::trunc(count) != count || start > length || count > length - start)
            return Value::Error("text.Slice range out of bounds");
        auto first = static_cast<std::size_t>(start), last = static_cast<std::size_t>(start + count);
        return Value::String(a[0].AsString().substr(offsets[first], offsets[last] - offsets[first]));
    });
    for (bool after : {false, true}) {
        Module.AddFunction(after ? "After" : "Before", [after](const std::vector<Value>& a) {
            if (a.size() != 2 || a[0].GetType() != ValueType::String || a[1].GetType() != ValueType::String || a[1].AsString().empty())
                return Value::Error("text.Before/After expects string and nonempty delimiter");
            const auto& value = a[0].AsString(); const auto& delimiter = a[1].AsString();
            auto at = value.find(delimiter);
            if (at == std::string::npos) return after ? Value::String("") : a[0];
            return Value::String(after ? value.substr(at + delimiter.size()) : value.substr(0, at));
        });
    }
    return Module.Build();
}
}
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor descriptor{Guid, "text", &Create};
    return &descriptor;
}
