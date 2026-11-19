#include "Support.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
bool SameFile(const std::filesystem::path& A, const std::filesystem::path& B) {
    const bool ExistsA = std::filesystem::exists(A);
    const bool ExistsB = std::filesystem::exists(B);
    if (ExistsA || ExistsB) return ExistsA && ExistsB && std::filesystem::equivalent(A, B);
    const auto Left = std::filesystem::absolute(A);
    const auto Right = std::filesystem::absolute(B);
    if (!std::filesystem::equivalent(Left.parent_path(), Right.parent_path())) return false;
#ifdef _WIN32
    return CompareStringOrdinal(Left.filename().c_str(), -1, Right.filename().c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return Left.filename() == Right.filename();
#endif
}
}

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
    try {
        if (SameFile(SourcePath, OutputPath) ||
            (HasSymbols && (SameFile(SourcePath, SymbolPath) || SameFile(OutputPath, SymbolPath)))) {
            std::cerr << "featherc: source, bytecode and symbols need distinct paths\n";
            return 2;
        }
    } catch (const std::filesystem::filesystem_error& Failure) {
        std::cerr << "featherc: " << Failure.what() << '\n';
        return 2;
    }
    std::vector<std::uint8_t> Bytes;
    std::vector<std::uint8_t> Symbols;
    try {
        auto Source = Feather::Cli::ReadSource(SourcePath);
        auto Program = Feather::Compile(Source);
        Bytes = Feather::SerializeProgram(Program, Feather::Cli::MaxFileBytes, Source.size());
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
