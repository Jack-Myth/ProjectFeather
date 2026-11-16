#pragma once
#include <Feather/Export.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Feather {

// Writes to an owned buffer or reads from a caller-owned byte span.
class FEATHER_API BinaryArchive final {
public:
    BinaryArchive() = default;
    explicit BinaryArchive(std::span<const std::uint8_t> Bytes)
        : Input(Bytes), Saving(false) {}

    bool IsSaving() const noexcept { return Saving; }
    bool IsLoading() const noexcept { return !Saving; }
    bool IsInMemory() const noexcept { return true; }
    std::size_t Remaining() const noexcept { return Input.size() - Position; }

    void Access(std::uint8_t& Data);
    void Access(std::uint32_t& Data);
    void Access(std::uint64_t& Data);
    void Access(bool& Data);
    void Access(double& Data);
    // Length-prefixed bytes. The caller applies any text validation.
    void Access(std::string& Data);
    void WriteBytes(std::string_view Data);
    std::vector<std::uint8_t> Finish() &&;

private:
    template<class UInt> void AccessInteger(UInt& Data);
    std::span<const std::uint8_t> Read(std::size_t Count);

    std::span<const std::uint8_t> Input;
    std::vector<std::uint8_t> Output;
    std::size_t Position = 0;
    bool Saving = true;
};

} // namespace Feather
