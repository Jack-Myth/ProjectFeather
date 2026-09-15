#include "Support.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    const bool HasOutput = ArgCount >= 3 && std::wstring_view(Arguments[2]) == L"-o";
#else
int main(int ArgCount, char** Arguments) {
    const bool HasOutput = ArgCount >= 3 && std::string_view(Arguments[2]) == "-o";
#endif
    if (ArgCount != 4 || !HasOutput) {
        std::cerr << "usage: featherc <source.fe> -o <program.fbc>\n";
        return 2;
    }
    auto SourcePath = std::filesystem::path(Arguments[1]);
    auto OutputPath = std::filesystem::path(Arguments[3]);
    std::vector<std::uint8_t> Bytes;
    try {
        auto Program = Feather::Compile(
            Feather::Cli::ReadSource(SourcePath));
        Bytes = Feather::SerializeProgram(Program);
    } catch (const std::exception& Failure) {
        std::cerr << "featherc: " << Feather::Cli::PathText(SourcePath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
    try {
        Feather::Cli::WriteFile(OutputPath, Bytes);
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "featherc: " << Feather::Cli::PathText(OutputPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
