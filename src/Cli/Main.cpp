#include "Support.hpp"
#ifdef FEATHER_CLI_DEBUGGER
#include "DebugHost.hpp"
#endif

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    const bool IsRun = ArgCount >= 2 && std::wstring_view(Arguments[1]) == L"run";
#ifdef FEATHER_CLI_DEBUGGER
    const bool IsDebug = ArgCount >= 2 && std::wstring_view(Arguments[1]) == L"debug";
    const bool HasListen = ArgCount >= 3 && std::wstring_view(Arguments[2]) == L"--listen";
    const bool HasWaitDebugger = ArgCount >= 5 &&
        std::wstring_view(Arguments[4]) == L"--wait-debugger";
#endif
#else
int main(int ArgCount, char** Arguments) {
    const bool IsRun = ArgCount >= 2 && std::string_view(Arguments[1]) == "run";
#ifdef FEATHER_CLI_DEBUGGER
    const bool IsDebug = ArgCount >= 2 && std::string_view(Arguments[1]) == "debug";
    const bool HasListen = ArgCount >= 3 && std::string_view(Arguments[2]) == "--listen";
    const bool HasWaitDebugger = ArgCount >= 5 &&
        std::string_view(Arguments[4]) == "--wait-debugger";
#endif
#endif
    const bool RunCommand = ArgCount == 3 && IsRun;
#ifdef FEATHER_CLI_DEBUGGER
    const bool DebugCommand = IsDebug && HasListen &&
        (ArgCount == 5 || (ArgCount == 6 && HasWaitDebugger));
#else
    constexpr bool DebugCommand = false;
#endif
    if (!RunCommand && !DebugCommand) {
        std::cerr << "usage: feather run <program.fe|program.fbc>\n";
#ifdef FEATHER_CLI_DEBUGGER
        std::cerr << "       feather debug --listen <host:port> [--wait-debugger] <source.fe>\n";
#endif
        return 2;
    }
    auto ProgramPath = std::filesystem::path(Arguments[DebugCommand ? ArgCount - 1 : 2]);
    try {
        const auto Extension = ProgramPath.extension();
        if (DebugCommand && Extension != ".fe")
            throw std::invalid_argument("debug requires a .fe source file");
        if (!DebugCommand && Extension != ".fe" && Extension != ".fbc")
            throw std::invalid_argument("run requires a .fe or .fbc file");
        const auto Kind = Extension == ".fbc" ? Feather::Cli::ModuleFileKind::Bytecode :
            Feather::Cli::ModuleFileKind::Source;
        auto Program = Kind == Feather::Cli::ModuleFileKind::Bytecode ?
            Feather::DeserializeProgram(Feather::Cli::ReadFile(ProgramPath)) :
            (DebugCommand ? Feather::Compile(Feather::Cli::ReadSource(ProgramPath)) :
                            Feather::Cli::LoadSourceProgram(ProgramPath));
#ifdef FEATHER_CLI_DEBUGGER
        if (DebugCommand)
            return Feather::Cli::RunDebugProgram(
                Program, ProgramPath, Feather::Cli::ModuleFileKind::Source,
                Feather::Cli::PathText(std::filesystem::path(Arguments[3])),
                HasWaitDebugger);
#endif
        return Feather::Cli::RunProgram(Program, ProgramPath, Kind);
    } catch (const std::exception& Failure) {
        std::cerr << "feather: " << Feather::Cli::PathText(ProgramPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
