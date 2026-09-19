#include "../src/Cli/ModuleLoader.hpp"

#include <Feather/Compiler.hpp>

#include <iostream>
#include <stdexcept>

using namespace Feather;

int main(int ArgumentCount, char** Arguments) {
    try {
        if (ArgumentCount != 2) throw std::runtime_error("missing entry path");
        auto Program = Compile("var helper = import(\"./helper.fe\");");
        Feather::Cli::NativeModules Native(std::cin, std::cout);
        Vm Machine(Program.Program);
        auto Loader = std::make_shared<Feather::Cli::ModuleLoader>(
            Machine, std::filesystem::path(Arguments[1]),
            Feather::Cli::ModuleFileKind::Source, Native);
        Loader->InstallRootImport();
        auto Result = Program.Initialize(Machine);
        if (Result.IsError()) throw std::runtime_error("module import failed");
        auto Files = Loader->LoadedFiles();
        if (Files.size() != 1 || Files[0] != "file:helper.fe")
            throw std::runtime_error("file module ID is not entry-relative");
        std::cout << "module loader relative ID test passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "module loader relative ID test failed: " << Failure.what() << '\n';
        return 1;
    }
}
