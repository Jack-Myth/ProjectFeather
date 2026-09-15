#include <Feather/Compiler.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Feather;

namespace {
void Check(bool Good, const char* Message) {
    if (!Good) throw std::runtime_error(Message);
}
using Event = std::pair<std::string, std::string>;
class CaptureCall final : public NativeObject {
public:
    CaptureCall(std::string Name, std::vector<Event>& Events)
        : Name(std::move(Name)), Events(Events) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
            throw std::runtime_error("quick operator must receive one string");
        Events.emplace_back(Name, Arguments[0].AsString());
        return {};
    }
private:
    std::string Name;
    std::vector<Event>& Events;
};
class EchoCall final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        if (Arguments.size() != 1 || Arguments[0].GetType() != ValueType::String)
            throw std::runtime_error("hostEcho expects one string");
        return Arguments[0];
    }
};
class HostData final : public NativeObject {
public:
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "value")
            return Value::Number(17);
        return Value::FromObject(std::make_shared<ErrorObject>("unknown host member"));
    }
};
}

int main() {
    try {
        auto Compiled = Compile(R"(
            // A regular comment is skipped by the lexer.
              >你好，我是XXX  // ignored
            #接受|拒绝
            $https:\//example.com
            &  保留前导空格  
            __QuickOperatorHash("explicit");
            def later() {
                >inside \//not-comment // ignored
            }
            def literal() { return "//"; }
            def callHost() { return hostEcho("called"); }
            def readHost() { return hostObject.value; }
        )");
        Vm Machine(Compiled.Program);
        std::vector<Event> Events;
        for (const auto* Name : {"__QuickOperatorRightAngleBucket", "__QuickOperatorHash",
                                 "__QuickOperatorDollar", "__QuickOperatorAmpersand"})
            Machine.RegisterNativeFunction(Name,
                std::make_shared<CaptureCall>(Name, Events));
        Machine.RegisterNativeFunction("hostEcho", std::make_shared<EchoCall>());
        Machine.SetGlobal("hostObject", Value::FromObject(std::make_shared<HostData>()));
        Compiled.Initialize(Machine);
        Check(Events == std::vector<Event>{
            {"__QuickOperatorRightAngleBucket", "你好，我是XXX"},
            {"__QuickOperatorHash", "接受|拒绝"},
            {"__QuickOperatorDollar", "https://example.com"},
            {"__QuickOperatorAmpersand", "  保留前导空格"},
            {"__QuickOperatorHash", "explicit"}},
            "top-level quick operator mapping or text");
        Machine.Run(Compiled.Functions.at("later"));
        Check(Events.size() == 6 &&
              Events.back() == Event{"__QuickOperatorRightAngleBucket", "inside //not-comment"},
              "block quick operator or escaped comment");
        Check(Machine.Run(Compiled.Functions.at("literal")).AsString() == "//",
              "normal string comments changed");
        Check(Machine.Run(Compiled.Functions.at("callHost")).AsString() == "called",
              "host native function was not callable from script");
        Check(Machine.Run(Compiled.Functions.at("readHost")).AsNumber() == 17,
              "host object was not available from script");

        auto EndOfFile = Compile("#tail   ");
        Vm Another(EndOfFile.Program);
        Another.RegisterNativeFunction("__QuickOperatorHash",
            std::make_shared<CaptureCall>("__QuickOperatorHash", Events));
        EndOfFile.Initialize(Another);
        Check(Events.back() == Event{"__QuickOperatorHash", "tail"}, "EOF quick line");

        try { (void)Compile("var x = 1; #not-at-line-start\n"); }
        catch (const std::runtime_error&) {
            std::cout << "Quick operator tests passed\n";
            return 0;
        }
        throw std::runtime_error("inline quick operator accepted");
    } catch (const std::exception& Failure) {
        std::cerr << "Quick operator tests failed: " << Failure.what() << '\n';
        return 1;
    }
}
