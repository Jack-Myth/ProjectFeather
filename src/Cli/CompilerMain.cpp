#include "Support.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    const bool HasOutput = ArgCount >= 3 && std::wstring_view(Arguments[2]) == L"-o";
    const bool HasSymbols = ArgCount == 6 && std::wstring_view(Arguments[4]) == L"--symbols";
#else
int main(int ArgCount, char** Arguments) {
    const bool HasOutput = ArgCount >= 3 && std::string_view(Arguments[2]) == "-o";
    const bool HasSymbols = ArgCount == 6 && std::string_view(Arguments[4]) == "--symbols";
#endif
    if ((ArgCount != 4 && !HasSymbols) || !HasOutput) {
        std::cerr << "usage: featherc <source.fe> -o <program.fbc> [--symbols <program.fbs>]\n";
        return 2;
    }
    auto SourcePath = std::filesystem::path(Arguments[1]);
    auto OutputPath = std::filesystem::path(Arguments[3]);
    auto SymbolPath = HasSymbols ? std::filesystem::path(Arguments[5]) : std::filesystem::path{};
    if (HasSymbols && OutputPath.lexically_normal() == SymbolPath.lexically_normal()) {
        std::cerr << "featherc: bytecode and symbols need distinct output paths\n";
        return 2;
    }
    std::vector<std::uint8_t> Bytes;
    std::vector<std::uint8_t> Symbols;
    try {
        auto Program = Feather::Compile(
            Feather::Cli::ReadSource(SourcePath));
        Bytes = Feather::SerializeProgram(Program);
        if (HasSymbols) Symbols = Feather::SerializeSymbols(Program);
    } catch (const std::exception& Failure) {
        std::cerr << "featherc: " << Feather::Cli::PathText(SourcePath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
    try {
        Feather::Cli::WriteFile(OutputPath, Bytes);
    } catch (const std::exception& Failure) {
        std::cerr << "featherc: " << Feather::Cli::PathText(OutputPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
    if (HasSymbols) try {
        Feather::Cli::WriteFile(SymbolPath, Symbols);
    } catch (const std::exception& Failure) {
        std::cerr << "featherc: " << Feather::Cli::PathText(SymbolPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
    return 0;
}
