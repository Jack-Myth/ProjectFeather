#include "Json.hpp"

#include <Feather/Runtime.hpp>

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace Feather::Protocol {
namespace {

void AppendUtf8(std::string& Output, std::uint32_t Point) {
    if (Point <= 0x7f) Output.push_back(static_cast<char>(Point));
    else if (Point <= 0x7ff) {
        Output.push_back(static_cast<char>(0xc0 | (Point >> 6)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    } else if (Point <= 0xffff) {
        Output.push_back(static_cast<char>(0xe0 | (Point >> 12)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 6) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    } else {
        Output.push_back(static_cast<char>(0xf0 | (Point >> 18)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 12) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | ((Point >> 6) & 0x3f)));
        Output.push_back(static_cast<char>(0x80 | (Point & 0x3f)));
    }
}

class Parser final {
public:
    explicit Parser(std::string_view Input) : Input(Input) {}
    Json Parse() {
        auto Result = ParseValue(0);
        Space();
        if (Position != Input.size()) Fail();
        return Result;
    }

private:
    [[noreturn]] void Fail() const { throw std::invalid_argument("invalid JSON protocol message"); }
    void Space() {
        while (Position < Input.size() &&
               (Input[Position] == ' ' || Input[Position] == '\t' ||
                Input[Position] == '\r' || Input[Position] == '\n')) ++Position;
    }
    bool Take(char Expected) {
        Space();
        if (Position == Input.size() || Input[Position] != Expected) return false;
        ++Position;
        return true;
    }
    Json ParseValue(unsigned Depth) {
        if (Depth >= 64) Fail();
        Space();
        if (Position == Input.size()) Fail();
        switch (Input[Position]) {
        case '{': return ParseObject(Depth + 1);
        case '[': return ParseArray(Depth + 1);
        case '"': return Json(ParseString());
        case 't': Literal("true"); return Json(true);
        case 'f': Literal("false"); return Json(false);
        case 'n': Literal("null"); return Json();
        default: return ParseNumber();
        }
    }
    void Literal(std::string_view Text) {
        if (Input.substr(Position, Text.size()) != Text) Fail();
        Position += Text.size();
    }
    Json ParseObject(unsigned Depth) {
        ++Position;
        Json::Object Result;
        if (Take('}')) return Result;
        while (true) {
            Space();
            if (Position == Input.size() || Input[Position] != '"') Fail();
            auto Name = ParseString();
            if (!Take(':')) Fail();
            if (!Result.emplace(std::move(Name), ParseValue(Depth)).second) Fail();
            if (Take('}')) return Result;
            if (!Take(',')) Fail();
        }
    }
    Json ParseArray(unsigned Depth) {
        ++Position;
        Json::Array Result;
        if (Take(']')) return Result;
        while (true) {
            Result.push_back(ParseValue(Depth));
            if (Take(']')) return Result;
            if (!Take(',')) Fail();
        }
    }
    std::uint32_t Hex4() {
        if (Position + 4 > Input.size()) Fail();
        std::uint32_t Result = 0;
        for (unsigned I = 0; I < 4; ++I) {
            char C = Input[Position++];
            unsigned Digit = C >= '0' && C <= '9' ? C - '0' :
                C >= 'a' && C <= 'f' ? C - 'a' + 10 :
                C >= 'A' && C <= 'F' ? C - 'A' + 10 : 16;
            if (Digit == 16) Fail();
            Result = Result * 16 + Digit;
        }
        return Result;
    }
    std::string ParseString() {
        if (Input[Position++] != '"') Fail();
        std::string Result;
        while (Position < Input.size()) {
            unsigned char C = static_cast<unsigned char>(Input[Position++]);
            if (C == '"') {
                (void)Value::String(Result);
                return Result;
            }
            if (C < 0x20) Fail();
            if (C != '\\') { Result.push_back(static_cast<char>(C)); continue; }
            if (Position == Input.size()) Fail();
            switch (Input[Position++]) {
            case '"': Result.push_back('"'); break;
            case '\\': Result.push_back('\\'); break;
            case '/': Result.push_back('/'); break;
            case 'b': Result.push_back('\b'); break;
            case 'f': Result.push_back('\f'); break;
            case 'n': Result.push_back('\n'); break;
            case 'r': Result.push_back('\r'); break;
            case 't': Result.push_back('\t'); break;
            case 'u': {
                auto Point = Hex4();
                if (Point >= 0xd800 && Point <= 0xdbff) {
                    if (Position + 2 > Input.size() || Input[Position] != '\\' ||
                        Input[Position + 1] != 'u') Fail();
                    Position += 2;
                    auto Low = Hex4();
                    if (Low < 0xdc00 || Low > 0xdfff) Fail();
                    Point = 0x10000 + ((Point - 0xd800) << 10) + (Low - 0xdc00);
                } else if (Point >= 0xdc00 && Point <= 0xdfff) Fail();
                AppendUtf8(Result, Point);
                break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    Json ParseNumber() {
        auto Start = Position;
        if (Input[Position] == '-') ++Position;
        if (Position == Input.size()) Fail();
        if (Input[Position] == '0') ++Position;
        else {
            if (Input[Position] < '1' || Input[Position] > '9') Fail();
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
        }
        if (Position < Input.size() && Input[Position] == '.') {
            ++Position;
            auto Fraction = Position;
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
            if (Fraction == Position) Fail();
        }
        if (Position < Input.size() && (Input[Position] == 'e' || Input[Position] == 'E')) {
            ++Position;
            if (Position < Input.size() && (Input[Position] == '+' || Input[Position] == '-')) ++Position;
            auto Exponent = Position;
            while (Position < Input.size() && Input[Position] >= '0' && Input[Position] <= '9') ++Position;
            if (Exponent == Position) Fail();
        }
        double Result = 0;
        auto Text = Input.substr(Start, Position - Start);
        auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Result);
        if (Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size() || !std::isfinite(Result)) Fail();
        return Json(Result);
    }
    std::string_view Input;
    std::size_t Position = 0;
};

void WriteJson(std::string& Output, const Json& Input);

void WriteString(std::string& Output, std::string_view Input) {
    constexpr char Hex[] = "0123456789abcdef";
    Output.push_back('"');
    for (unsigned char C : Input) {
        switch (C) {
        case '"': Output += "\\\""; break;
        case '\\': Output += "\\\\"; break;
        case '\b': Output += "\\b"; break;
        case '\f': Output += "\\f"; break;
        case '\n': Output += "\\n"; break;
        case '\r': Output += "\\r"; break;
        case '\t': Output += "\\t"; break;
        default:
            if (C < 0x20) {
                Output += "\\u00";
                Output.push_back(Hex[C >> 4]);
                Output.push_back(Hex[C & 15]);
            } else Output.push_back(static_cast<char>(C));
        }
    }
    Output.push_back('"');
}

void WriteJson(std::string& Output, const Json& Input) {
    const auto& Data = Input.Get();
    if (std::holds_alternative<std::nullptr_t>(Data)) { Output += "null"; return; }
    if (auto Value = std::get_if<bool>(&Data)) { Output += *Value ? "true" : "false"; return; }
    if (auto Value = std::get_if<double>(&Data)) {
        char Buffer[64];
        auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), *Value,
                                    std::chars_format::general);
        if (Result.ec != std::errc{}) throw std::runtime_error("cannot encode JSON number");
        Output.append(Buffer, Result.ptr);
        return;
    }
    if (auto Value = std::get_if<std::string>(&Data)) { WriteString(Output, *Value); return; }
    if (auto Value = std::get_if<Json::Object>(&Data)) {
        Output.push_back('{');
        bool First = true;
        for (const auto& [Name, Item] : *Value) {
            if (!First) Output.push_back(',');
            First = false;
            WriteString(Output, Name); Output.push_back(':'); WriteJson(Output, Item);
        }
        Output.push_back('}');
        return;
    }
    const auto& Value = std::get<Json::Array>(Data);
    Output.push_back('[');
    for (std::size_t I = 0; I < Value.size(); ++I) {
        if (I) Output.push_back(',');
        WriteJson(Output, Value[I]);
    }
    Output.push_back(']');
}

} // namespace

Json ParseJson(std::string_view Input) { return Parser(Input).Parse(); }

std::string EncodeJson(const Json& Input) {
    std::string Result;
    WriteJson(Result, Input);
    return Result;
}

const Json* Find(const Json::Object& Object, std::string_view Name) {
    auto Found = Object.find(std::string(Name));
    return Found == Object.end() ? nullptr : &Found->second;
}

std::uint64_t Integer(const Json& Input, std::string_view Name) {
    if (!Input.IsNumber() || Input.Number() < 0 || std::floor(Input.Number()) != Input.Number() ||
        Input.Number() > 9'007'199'254'740'991.0)
        throw std::invalid_argument(std::string(Name) + " must be a nonnegative integer");
    return static_cast<std::uint64_t>(Input.Number());
}

} // namespace Feather::Protocol
