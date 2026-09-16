#include "Support.hpp"

#include <fstream>
#include <stdexcept>

namespace Feather::Cli {

std::string PathText(const std::filesystem::path& Path) {
    auto Utf8 = Path.u8string();
    std::string Result;
    Result.reserve(Utf8.size());
    for (char8_t Byte : Utf8) Result.push_back(static_cast<char>(Byte));
    return Result;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& Path) {
    std::ifstream Input(Path, std::ios::binary | std::ios::ate);
    if (!Input) throw std::runtime_error("cannot open input file");
    auto End = Input.tellg();
    if (End < 0 || static_cast<std::uintmax_t>(End) > MaxFileBytes)
        throw std::runtime_error("input file exceeds 64 MiB limit");
    std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(End));
    Input.seekg(0);
    if (!Bytes.empty()) Input.read(reinterpret_cast<char*>(Bytes.data()), Bytes.size());
    if (!Input) throw std::runtime_error("cannot read input file");
    return Bytes;
}

std::string ReadSource(const std::filesystem::path& Path) {
    auto Bytes = ReadFile(Path);
    return {Bytes.begin(), Bytes.end()};
}

void WriteFile(const std::filesystem::path& Path, std::span<const std::uint8_t> Bytes) {
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    if (!Output) throw std::runtime_error("cannot open output file");
    Output.write(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
    Output.close();
    if (!Output) throw std::runtime_error("cannot write output file");
}

#ifdef FEATHER_CLI_SOURCE_LOADER
CompiledProgram LoadSourceProgram(const std::filesystem::path& SourcePath) {
    auto BytecodePath = SourcePath;
    BytecodePath.replace_extension(".fbc");
    std::error_code Error;
    const bool HasBytecode = std::filesystem::is_regular_file(BytecodePath, Error);
    Error.clear();
    const auto SourceTime = std::filesystem::last_write_time(SourcePath, Error);
    if (!Error && HasBytecode) {
        Error.clear();
        const auto BytecodeTime = std::filesystem::last_write_time(BytecodePath, Error);
        if (!Error && BytecodeTime >= SourceTime) {
            try { return DeserializeProgram(ReadFile(BytecodePath)); }
            catch (const std::exception&) {
                // A cache is only an optimization. Compile the authoritative source below.
            }
        }
    }
    return Compile(ReadSource(SourcePath));
}
#endif

} // namespace Feather::Cli
