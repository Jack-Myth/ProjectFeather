#include "../src/Cli/ModuleLoader.hpp"
#include "../src/FileModulePath.hpp"

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
        if (Files.size() != 1 || Files[0] != "./helper.fe")
            throw std::runtime_error("file module ID is not entry-relative");
        const auto Directory = std::filesystem::absolute(Arguments[1]).parent_path();
        const auto Sibling = Directory.parent_path() / "other.fe";
        if (FileModulePath::Id(Directory, Sibling) != "../other.fe" ||
            FileModulePath::Resolve(Directory, "../other.fe") != Sibling ||
            FileModulePath::Resolve(Directory, "helper.fe"))
            throw std::runtime_error("invalid relative module path conversion");
#ifdef _WIN32
        const std::filesystem::path Base = L"C:/project";
        for (const auto* Text : {"D:/libraries/math.fe", "//server/share/math.fe"}) {
            const auto Absolute = FileModulePath::FromUtf8(Text);
            if (FileModulePath::Id(Base, Absolute) != Text ||
                FileModulePath::Resolve(Base, Text) != Absolute)
                throw std::runtime_error("absolute module path did not round-trip");
        }
        if (FileModulePath::Resolve(Base, "C:helper.fe") || FileModulePath::Resolve(Base, "/helper.fe"))
            throw std::runtime_error("incomplete Windows path accepted");
        if (FileModulePath::Id(L"//server/one/app", L"//server/two/lib.fe") != "//server/two/lib.fe")
            throw std::runtime_error("module path crossed UNC shares with ..");
#endif
        std::cout << "module loader relative ID test passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "module loader relative ID test failed: " << Failure.what() << '\n';
        return 1;
    }
}
