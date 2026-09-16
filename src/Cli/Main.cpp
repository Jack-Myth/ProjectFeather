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
        std::cerr << "usage: feather run <source.fe>\n";
#ifdef FEATHER_CLI_DEBUGGER
        std::cerr << "       feather debug --listen <host:port> [--wait-debugger] <source.fe>\n";
#endif
        return 2;
    }
    auto SourcePath = std::filesystem::path(Arguments[DebugCommand ? ArgCount - 1 : 2]);
    try {
        auto Program = Feather::Compile(Feather::Cli::ReadSource(SourcePath));
#ifdef FEATHER_CLI_DEBUGGER
        if (DebugCommand)
            return Feather::Cli::RunDebugProgram(
                Program, SourcePath, Feather::Cli::ModuleFileKind::Source,
                Feather::Cli::PathText(std::filesystem::path(Arguments[3])),
                HasWaitDebugger);
#endif
        return Feather::Cli::RunProgram(Program, SourcePath, Feather::Cli::ModuleFileKind::Source);
    } catch (const std::exception& Failure) {
        std::cerr << "feather: " << Feather::Cli::PathText(SourcePath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
