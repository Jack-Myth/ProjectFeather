#include "Support.hpp"
#include "ModuleLoader.hpp"
#include <Feather/Import.hpp>

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace Feather::Cli {
namespace {

std::string FormatSource(const SourceLocation& Source) {
    return (Source.ModuleId.empty() ? std::string{} : "module " + Source.ModuleId + ": ") +
           "source byte " + std::to_string(Source.ByteOffset) + " (" +
           std::to_string(Source.Line) + ":" + std::to_string(Source.Column) + "): ";
}

void CheckResult(const Value& Result, const char* Stage, const Vm& Machine) {
    if (!Result.IsError()) return;
    auto* Error = dynamic_cast<ErrorObject*>(Result.AsObject());
    std::string Message = std::string(Stage) + ": ";
    if (auto Source = Machine.GetErrorLocation(Result))
        Message += FormatSource(*Source);
    Message += Error ? Error->GetMessage() : "unknown language error";
    throw std::runtime_error(Message);
}

} // namespace

int RunProgram(const CompiledProgram& Program, const std::filesystem::path& EntryPath,
               ModuleFileKind Kind, std::shared_ptr<VmDebugController> DebugController) {
    if (Program.UsesQuickOperators)
        throw std::runtime_error("quick operators require host-provided __QuickOperator functions");
    constexpr std::size_t MaxCliScriptObjects = 100'000;
    constexpr std::size_t MaxCliInstructions = 10'000'000;
    NativeModules Native(std::cin, std::cout);
    Vm Machine(Program.Program, MaxCliScriptObjects, MaxCliInstructions);
    Machine.SetDebugController(std::move(DebugController));
    struct NativeRootGuard {
        NativeModules& Owner;
        ~NativeRootGuard() { Owner.ReleaseRoots(); }
    } RootGuard{Native};
    auto Loader = std::make_shared<ModuleLoader>(Machine, EntryPath, Kind, Native);
    Loader->InstallRootImport();
    auto ExecuteWithLocation = [&](auto&& Action) -> Value {
        try { return Action(); }
        catch (const std::exception& Failure) {
            if (auto Source = Machine.GetFaultLocation())
                throw std::runtime_error(FormatSource(*Source) + Failure.what());
            throw;
        }
    };
    CheckResult(ExecuteWithLocation([&] { return Program.Initialize(Machine); }),
                "initializer failed", Machine);
    if (auto Main = Program.Functions.find("main"); Main != Program.Functions.end()) {
        const auto& Prototype = Program.Program->Constants.at(Main->second).Function;
        if (!Prototype || Prototype->ParameterCount != 0)
            throw std::runtime_error("main must have no parameters");
        CheckResult(ExecuteWithLocation([&] { return Machine.Run(Main->second); }),
                    "main failed", Machine);
    }
    return 0;
}

} // namespace Feather::Cli
