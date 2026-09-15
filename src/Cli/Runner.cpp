#include "Support.hpp"
#include <Feather/Import.hpp>
#include <Feather/StdIo.hpp>

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace Feather::Cli {
namespace {

void CheckResult(const Value& Result, const char* Stage) {
    if (!Result.IsError()) return;
    auto* Error = dynamic_cast<ErrorObject*>(Result.AsObject());
    throw std::runtime_error(std::string(Stage) + ": " +
                             (Error ? Error->GetMessage() : "unknown language error"));
}

} // namespace

int RunProgram(const CompiledProgram& Program) {
    if (Program.UsesQuickOperators)
        throw std::runtime_error("quick operators require host-provided __QuickOperator functions");
    constexpr std::size_t MaxCliScriptObjects = 100'000;
    constexpr std::size_t MaxCliInstructions = 10'000'000;
    Vm Machine(Program.Program, MaxCliScriptObjects, MaxCliInstructions);
    auto Library = std::make_shared<StdIoLibrary>(std::cin, std::cout);
    RegisterImport(Machine, [Library](std::string_view Specifier) -> Value {
        auto Standard = Library->Resolve(Specifier);
        if (Specifier == "std:console" || Specifier == "std:io") return Standard;
        throw std::runtime_error("import requires a host resolver");
    });
    CheckResult(Program.Initialize(Machine), "initializer failed");
    if (auto Main = Program.Functions.find("main"); Main != Program.Functions.end()) {
        const auto& Prototype = Program.Program->Constants.at(Main->second).Function;
        if (!Prototype || Prototype->ParameterCount != 0)
            throw std::runtime_error("main must have no parameters");
        CheckResult(Machine.Run(Main->second), "main failed");
    }
    return 0;
}

} // namespace Feather::Cli
