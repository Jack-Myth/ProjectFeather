#include <Feather/NativeModule.hpp>
#include <cmath>
#include <Feather/ByteBuffer.hpp>



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
bool IsWide(const Value& Encoding) {
    if (Encoding.GetType() == ValueType::String) {
        if (Encoding.AsString() == "UTF-16LE") return true;
        if (Encoding.AsString() == "UTF-8") return false;
    }
    throw std::invalid_argument("unknown text encoding");
}
// Feather strings are already validated UTF-8.
std::string Utf16Bytes(const std::string& Text) {
    std::string Out;
    auto Unit = [&](unsigned U) { Out.push_back(char(U)); Out.push_back(char(U >> 8)); };
    for (size_t I = 0; I < Text.size();) {
        unsigned C = static_cast<unsigned char>(Text[I++]);
        if (C >= 128) {
            unsigned Count = C < 224 ? 1 : C < 240 ? 2 : 3;
            C &= 0x7f >> (Count + 1);
            while (Count--) C = (C << 6) | (Text[I++] & 63);
        }
        if (C < 65536) Unit(C);
        else { C -= 65536; Unit(0xd800 + (C >> 10)); Unit(0xdc00 + (C & 1023)); }
    }
    return Out;
}
std::string FromUtf16(std::string_view Bytes) {
    if (Bytes.size() % 2) throw std::invalid_argument("odd UTF-16LE byte count");
    auto Unit = [&](size_t I) -> unsigned {
        return static_cast<unsigned char>(Bytes[I]) | (static_cast<unsigned char>(Bytes[I+1]) << 8);
    };
    std::string Out;
    for (size_t I = 0; I < Bytes.size(); I += 2) {
        unsigned C = Unit(I);
        if (C >= 0xd800 && C <= 0xdfff) {
            if (C >= 0xdc00 || I+3 >= Bytes.size() || Unit(I+2) < 0xdc00 || Unit(I+2) > 0xdfff)
                throw std::invalid_argument("invalid UTF-16 surrogate pair");
            C = 0x10000 + ((C-0xd800) << 10) + Unit(I+2)-0xdc00;
            I += 2;
        }
        if (C < 128) { Out.push_back(char(C)); continue; }
        unsigned Count = C < 2048 ? 1 : C < 65536 ? 2 : 3;
        Out.push_back(char((0xff << (7-Count)) | (C >> (6*Count))));
        while (Count) Out.push_back(char(0x80 | ((C >> (6 * --Count)) & 63)));
    }
    return Out;
}
Value Create(const NativeModuleContext& context) {
    NativeModuleBuilder Module(context.Machine, Guid, "text");
    ByteBufferLibrary Buffers(context.Machine);
    Module.AddValue("UTF8", Value::String("UTF-8"));
    Module.AddValue("UTF16LE", Value::String("UTF-16LE"));
    for (bool Z : {false, true}) {
        Module.AddFunction(Z ? "EncodeZ" : "Encode", [Buffers, Z](const auto& A) {
            if (A.size() != 2 || A[0].GetType() != ValueType::String)
                return Value::Error("Encode expects a string and encoding");
            bool Wide = IsWide(A[1]);
            auto Bytes = A[0].AsString();
            if (Z && Bytes.find('\0') != std::string::npos) return Value::Error("embedded NUL in EncodeZ");
            if (Wide) Bytes = Utf16Bytes(Bytes);
            if (Z) Bytes.append(Wide ? 2 : 1, '\0');
            return Buffers.Create(std::move(Bytes));
        });
        Module.AddFunction(Z ? "DecodeZ" : "Decode", [Z](const auto& A) {
            if (A.size() != 2) return Value::Error("Decode expects a ByteBuffer and encoding");
            auto* Data = ByteBufferData(A[0]);
            if (!Data) return Value::Error("Decode expects a ByteBuffer");
            bool Wide = IsWide(A[1]);
            std::string_view Bytes = *Data;
            if (Z) {
                size_t I = 0, Width = Wide ? 2 : 1;
                while (I + Width <= Bytes.size() && (Bytes[I] || (Wide && Bytes[I+1]))) I += Width;
                if (I + Width > Bytes.size()) return Value::Error("missing string terminator");
                Bytes = Bytes.substr(0, I);
            }
            return Value::String(Wide ? FromUtf16(Bytes) : std::string(Bytes));
        });
    }
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
