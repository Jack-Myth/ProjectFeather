#include "Support.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
    const bool IsRun = ArgCount >= 2 && std::wstring_view(Arguments[1]) == L"run";
#else
int main(int ArgCount, char** Arguments) {
    const bool IsRun = ArgCount >= 2 && std::string_view(Arguments[1]) == "run";
#endif
    if (ArgCount != 3 || !IsRun) {
        std::cerr << "usage: feather run <source.fe>\n";
        return 2;
    }
    auto SourcePath = std::filesystem::path(Arguments[2]);
    try {
        return Feather::Cli::RunProgram(
            Feather::Compile(Feather::Cli::ReadSource(SourcePath)));
    } catch (const std::exception& Failure) {
        std::cerr << "feather: " << Feather::Cli::PathText(SourcePath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
