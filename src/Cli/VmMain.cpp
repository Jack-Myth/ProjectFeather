#include "Support.hpp"

#include <filesystem>
#include <iostream>

#ifdef _WIN32
int wmain(int ArgCount, wchar_t** Arguments) {
#else
int main(int ArgCount, char** Arguments) {
#endif
    if (ArgCount != 2) {
        std::cerr << "usage: feathervm <program.fbc>\n";
        return 2;
    }
    auto ArtifactPath = std::filesystem::path(Arguments[1]);
    try {
        auto Bytes = Feather::Cli::ReadFile(ArtifactPath);
        return Feather::Cli::RunProgram(Feather::DeserializeProgram(Bytes));
    } catch (const std::exception& Failure) {
        std::cerr << "feathervm: " << Feather::Cli::PathText(ArtifactPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
