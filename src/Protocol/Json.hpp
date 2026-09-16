#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Feather::Protocol {

class Json final {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    Json() : Data(nullptr) {}
    Json(std::nullptr_t) : Data(nullptr) {}
    Json(bool Input) : Data(Input) {}
    Json(double Input) : Data(Input) {}
    Json(std::uint64_t Input) : Data(static_cast<double>(Input)) {}
    Json(std::string Input) : Data(std::move(Input)) {}
    Json(std::string_view Input) : Data(std::string(Input)) {}
    Json(const char* Input) : Data(std::string(Input)) {}
    Json(Object Input) : Data(std::move(Input)) {}
    Json(Array Input) : Data(std::move(Input)) {}

    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(Data); }
    bool IsBool() const { return std::holds_alternative<bool>(Data); }
    bool IsNumber() const { return std::holds_alternative<double>(Data); }
    bool IsString() const { return std::holds_alternative<std::string>(Data); }
    bool IsObject() const { return std::holds_alternative<Object>(Data); }
    bool IsArray() const { return std::holds_alternative<Array>(Data); }
    bool Bool() const { return std::get<bool>(Data); }
    double Number() const { return std::get<double>(Data); }
    const std::string& String() const { return std::get<std::string>(Data); }
    const Object& Members() const { return std::get<Object>(Data); }
    const Array& Items() const { return std::get<Array>(Data); }
    const Storage& Get() const { return Data; }

private:
    Storage Data;
};

Json ParseJson(std::string_view Input);
std::string EncodeJson(const Json& Input);
const Json* Find(const Json::Object& Object, std::string_view Name);
std::uint64_t Integer(const Json& Input, std::string_view Name);

} // namespace Feather::Protocol
