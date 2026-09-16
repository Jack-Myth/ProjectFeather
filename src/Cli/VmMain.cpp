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
    if (ArtifactPath.extension() != ".fbc") {
        std::cerr << "feathervm: input must be a .fbc file\n";
        return 2;
    }
    try {
        auto Bytes = Feather::Cli::ReadFile(ArtifactPath);
        auto Program = Feather::DeserializeProgram(Bytes);
        return Feather::Cli::RunProgram(Program, ArtifactPath,
                                        Feather::Cli::ModuleFileKind::Bytecode);
    } catch (const std::exception& Failure) {
        std::cerr << "feathervm: " << Feather::Cli::PathText(ArtifactPath) << ": "
                  << Failure.what() << '\n';
        return 1;
    }
}
