#include "SnapshotHost.hpp"
#include "Support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Feather::Cli {
namespace {

constexpr std::size_t MaxArchiveBytes = 64 * 1024 * 1024;
constexpr std::uint32_t MaxManifestEntries = 4096;

Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}

std::filesystem::path Utf8Path(std::string_view Text) {
    std::u8string Bytes;
    Bytes.reserve(Text.size());
    for (unsigned char Byte : Text) Bytes.push_back(static_cast<char8_t>(Byte));
    return std::filesystem::path(Bytes);
}

std::uint32_t Checksum(std::span<const std::uint8_t> Bytes) {
    std::uint32_t Crc = 0xffffffffu;
    for (auto Byte : Bytes) {
        Crc ^= Byte;
        for (int Bit = 0; Bit < 8; ++Bit)
            Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320u : 0u);
    }
    return ~Crc;
}

class Writer final {
public:
    void U8(std::uint8_t Input) { Bytes.push_back(Input); }
    void U16(std::uint16_t Input) {
        U8(static_cast<std::uint8_t>(Input)); U8(static_cast<std::uint8_t>(Input >> 8));
    }
    void U32(std::uint32_t Input) {
        for (unsigned I = 0; I < 4; ++I) U8(static_cast<std::uint8_t>(Input >> (I * 8)));
    }
    void Blob(std::span<const std::uint8_t> Input) {
        if (Input.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("snapshot archive field is too large");
        U32(static_cast<std::uint32_t>(Input.size()));
        Bytes.insert(Bytes.end(), Input.begin(), Input.end());
    }
    void String(std::string_view Input) {
        Blob(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(Input.data()), Input.size()));
    }
    std::vector<std::uint8_t> Bytes;
};

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> Input) : Input(Input) {}
    std::uint8_t U8() {
        if (At == Input.size()) throw std::invalid_argument("truncated snapshot archive");
        return Input[At++];
    }
    std::uint16_t U16() { return std::uint16_t(U8()) | (std::uint16_t(U8()) << 8); }
    std::uint32_t U32() {
        std::uint32_t Result = 0;
        for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(U8()) << (I * 8);
        return Result;
    }
    std::span<const std::uint8_t> Blob() {
        auto Size = U32();
        if (Size > Input.size() - At) throw std::invalid_argument("truncated snapshot archive field");
        auto Result = Input.subspan(At, Size);
        At += Size;
        return Result;
    }
    std::string String() {
        auto Bytes = Blob();
        std::string Result(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        (void)Value::String(Result);
        return Result;
    }
    bool Done() const { return At == Input.size(); }
private:
    std::span<const std::uint8_t> Input;
    std::size_t At = 0;
};

std::vector<std::uint8_t> Encode(const SnapshotArchive& Archive) {
    Writer Output;
    Output.U8('F'); Output.U8('T'); Output.U8('H'); Output.U8('A');
    Output.U16(1); Output.U16(0);
    auto WriteNativeList = [&](const std::vector<NativeModuleIdentity>& List) {
        if (List.size() > MaxManifestEntries)
            throw std::length_error("too many snapshot archive modules");
        Output.U32(static_cast<std::uint32_t>(List.size()));
        for (const auto& Item : List) {
            Output.String(Item.Name);
            Output.Bytes.insert(Output.Bytes.end(), Item.Id.Bytes.begin(), Item.Id.Bytes.end());
        }
    };
    auto WriteFileList = [&](const std::vector<std::string>& List) {
        if (List.size() > MaxManifestEntries)
            throw std::length_error("too many snapshot archive modules");
        Output.U32(static_cast<std::uint32_t>(List.size()));
        for (const auto& Item : List) Output.String(Item);
    };
    WriteNativeList(Archive.NativeModules);
    WriteFileList(Archive.FeatherModules);
    Output.Blob(Archive.VmBytes);
    Output.U32(Checksum(Output.Bytes));
    if (Output.Bytes.size() > MaxArchiveBytes)
        throw std::length_error("snapshot archive exceeds 64 MiB limit");
    return std::move(Output.Bytes);
}

SnapshotArchive Decode(const std::vector<std::uint8_t>& Bytes) {
    if (Bytes.size() < 12 || Bytes.size() > MaxArchiveBytes)
        throw std::invalid_argument("invalid snapshot archive size");
    Reader Input(std::span<const std::uint8_t>(Bytes.data(), Bytes.size() - 4));
    if (Input.U8() != 'F' || Input.U8() != 'T' || Input.U8() != 'H' || Input.U8() != 'A')
        throw std::invalid_argument("invalid snapshot archive magic");
    if (Input.U16() != 1 || Input.U16() != 0)
        throw std::invalid_argument("unsupported snapshot archive version");
    SnapshotArchive Result;
    auto NativeCount = Input.U32();
    if (NativeCount > MaxManifestEntries)
        throw std::invalid_argument("too many snapshot archive modules");
    Result.NativeModules.reserve(NativeCount);
    for (std::uint32_t I = 0; I < NativeCount; ++I) {
        NativeModuleIdentity Identity;
        Identity.Name = Input.String();
        for (auto& Byte : Identity.Id.Bytes) Byte = Input.U8();
        Result.NativeModules.push_back(std::move(Identity));
    }
    auto ReadFileList = [&](std::vector<std::string>& List) {
        auto Count = Input.U32();
        if (Count > MaxManifestEntries) throw std::invalid_argument("too many snapshot archive modules");
        List.reserve(Count);
        for (std::uint32_t I = 0; I < Count; ++I) List.push_back(Input.String());
    };
    ReadFileList(Result.FeatherModules);
    auto Vm = Input.Blob();
    Result.VmBytes.assign(Vm.begin(), Vm.end());
    if (!Input.Done()) throw std::invalid_argument("trailing snapshot archive data");
    Reader Footer(std::span<const std::uint8_t>(Bytes.data() + Bytes.size() - 4, 4));
    if (Footer.U32() != Checksum(std::span<const std::uint8_t>(Bytes.data(), Bytes.size() - 4)))
        throw std::invalid_argument("snapshot archive checksum mismatch");
    return Result;
}

std::filesystem::path AbsolutePath(std::string_view Text) {
    auto Path = Utf8Path(Text);
    if (Path.empty()) throw std::invalid_argument("snapshot path is empty");
    return std::filesystem::absolute(Path).lexically_normal();
}

void AtomicWrite(const std::filesystem::path& Target, std::span<const std::uint8_t> Bytes) {
    static std::atomic<std::uint64_t> Counter{0};
    auto Suffix = ".tmp." + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(Counter.fetch_add(1, std::memory_order_relaxed));
    auto Temporary = Target;
    Temporary += Suffix;
    struct Cleanup {
        std::filesystem::path Path;
        ~Cleanup() { std::error_code Error; std::filesystem::remove(Path, Error); }
    } Cleanup{Temporary};

    std::FILE* File = nullptr;
#ifdef _WIN32
    _wfopen_s(&File, Temporary.c_str(), L"wb");
#else
    File = std::fopen(Temporary.c_str(), "wb");
#endif
    if (!File) throw std::runtime_error("cannot create temporary snapshot file");
    bool Good = Bytes.empty() || std::fwrite(Bytes.data(), 1, Bytes.size(), File) == Bytes.size();
    Good = Good && std::fflush(File) == 0;
#ifdef _WIN32
    Good = Good && _commit(_fileno(File)) == 0;
#else
    Good = Good && fsync(fileno(File)) == 0;
#endif
    Good = std::fclose(File) == 0 && Good;
    if (!Good) throw std::runtime_error("cannot write snapshot file");
#ifdef _WIN32
    if (!MoveFileExW(Temporary.c_str(), Target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot commit snapshot file (Windows error " +
                                 std::to_string(GetLastError()) + ")");
#else
    if (std::rename(Temporary.c_str(), Target.c_str()) != 0)
        throw std::runtime_error("cannot commit snapshot file");
#endif
    Cleanup.Path.clear();
}

} // namespace

Value CliSnapshotHost::Checkpoint(Vm& Machine, std::string_view Path) {
    try {
        SnapshotArchive Archive{Native.LoadedIdentities(), Loader.LoadedFiles(),
                                Machine.CaptureSnapshot()};
        AtomicWrite(AbsolutePath(Path), Encode(Archive));
        return Value::Bool(false);
    } catch (const std::exception& Failure) {
        return Error(Failure.what());
    }
}

Value CliSnapshotHost::Restore(std::string_view Path) {
    try {
        auto Archive = Decode(ReadFile(AbsolutePath(Path)));
        throw SnapshotRestoreRequest{std::move(Archive)};
    } catch (const SnapshotRestoreRequest&) {
        throw;
    } catch (const std::exception& Failure) {
        return Error(Failure.what());
    }
}

} // namespace Feather::Cli
