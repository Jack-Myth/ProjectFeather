#include "Support.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    const bool HasSymbols = ArgCount == 4 && std::wstring_view(Arguments[2]) == L"--symbols";
#else
int main(int ArgCount, char** Arguments) {
    const bool HasSymbols = ArgCount == 4 && std::string_view(Arguments[2]) == "--symbols";
#endif
    if (ArgCount != 2 && !HasSymbols) {
        std::cerr << "usage: feathervm <program.fbc> [--symbols <program.fbs>]\n";
        return 2;
    }
    auto ArtifactPath = std::filesystem::path(Arguments[1]);
    auto SymbolPath = HasSymbols ? std::filesystem::path(Arguments[3]) : std::filesystem::path{};
    try {
        auto Bytes = Feather::Cli::ReadFile(ArtifactPath);
        auto Program = Feather::DeserializeProgram(Bytes);
        if (HasSymbols) Feather::AttachSymbols(Program, Feather::Cli::ReadFile(SymbolPath));
        return Feather::Cli::RunProgram(Program, ArtifactPath,
                                        Feather::Cli::ModuleFileKind::Bytecode);
    } catch (const std::exception& Failure) {
        std::cerr << "feathervm: " << Feather::Cli::PathText(ArtifactPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
