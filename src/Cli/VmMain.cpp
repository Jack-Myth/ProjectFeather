#include "Support.hpp"
#ifdef FEATHER_CLI_DEBUGGER
#include "DebugHost.hpp"
#endif

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    auto IsOption = [&](int Index, std::wstring_view Name) {
        return Index < ArgCount && std::wstring_view(Arguments[Index]) == Name;
    };
#else
int main(int ArgCount, char** Arguments) {
    auto IsOption = [&](int Index, std::string_view Name) {
        return Index < ArgCount && std::string_view(Arguments[Index]) == Name;
    };
#endif
    if (ArgCount < 2) {
        std::cerr << "usage: feathervm <program.fbc> [--symbols <program.fbs>]\n";
#ifdef FEATHER_CLI_DEBUGGER
        std::cerr << "       feathervm <program.fbc> [--symbols <program.fbs>] "
                     "[--debug-listen <host:port>] [--wait-debugger]\n";
#endif
        return 2;
    }
    auto ArtifactPath = std::filesystem::path(Arguments[1]);
    std::filesystem::path SymbolPath;
    std::string DebugEndpoint;
    bool WaitForDebugger = false;
    for (int I = 2; I < ArgCount;) {
#ifdef _WIN32
        if (IsOption(I, L"--symbols") && I + 1 < ArgCount && SymbolPath.empty()) {
#else
        if (IsOption(I, "--symbols") && I + 1 < ArgCount && SymbolPath.empty()) {
#endif
            SymbolPath = std::filesystem::path(Arguments[I + 1]);
            I += 2;
            continue;
        }
#ifdef FEATHER_CLI_DEBUGGER
#ifdef _WIN32
        if (IsOption(I, L"--debug-listen") && I + 1 < ArgCount && DebugEndpoint.empty()) {
#else
        if (IsOption(I, "--debug-listen") && I + 1 < ArgCount && DebugEndpoint.empty()) {
#endif
            DebugEndpoint = Feather::Cli::PathText(std::filesystem::path(Arguments[I + 1]));
            I += 2;
            continue;
        }
#ifdef _WIN32
        if (IsOption(I, L"--wait-debugger") && !WaitForDebugger) {
#else
        if (IsOption(I, "--wait-debugger") && !WaitForDebugger) {
#endif
            WaitForDebugger = true;
            ++I;
            continue;
        }
#endif
        std::cerr << "feathervm: invalid or duplicate option\n";
        return 2;
    }
    if (WaitForDebugger && DebugEndpoint.empty()) {
        std::cerr << "feathervm: --wait-debugger requires --debug-listen\n";
        return 2;
    }
    try {
        auto Bytes = Feather::Cli::ReadFile(ArtifactPath);
        auto Program = Feather::DeserializeProgram(Bytes);
        if (!SymbolPath.empty()) Feather::AttachSymbols(Program, Feather::Cli::ReadFile(SymbolPath));
#ifdef FEATHER_CLI_DEBUGGER
        if (!DebugEndpoint.empty())
            return Feather::Cli::RunDebugProgram(
                Program, ArtifactPath, Feather::Cli::ModuleFileKind::Bytecode,
                DebugEndpoint, WaitForDebugger);
#endif
        return Feather::Cli::RunProgram(Program, ArtifactPath,
                                        Feather::Cli::ModuleFileKind::Bytecode);
    } catch (const std::exception& Failure) {
        std::cerr << "feathervm: " << Feather::Cli::PathText(ArtifactPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
