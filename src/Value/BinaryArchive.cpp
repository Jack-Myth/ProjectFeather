#include <Feather/BinaryArchive.hpp>

#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Feather {

template<class UInt>
void BinaryArchive::AccessInteger(UInt& Data) {
    if (IsSaving()) {
        for (std::size_t I = 0; I < sizeof(UInt); ++I)
            Output.push_back(static_cast<std::uint8_t>(Data >> (8 * I)));
        return;
    }
    const auto Bytes = Read(sizeof(UInt));
    Data = 0;
    for (std::size_t I = 0; I < Bytes.size(); ++I)
        Data = static_cast<UInt>(Data | (static_cast<UInt>(Bytes[I]) << (8 * I)));
}

void BinaryArchive::Access(std::uint8_t& Data) { AccessInteger(Data); }
void BinaryArchive::Access(std::uint32_t& Data) { AccessInteger(Data); }
void BinaryArchive::Access(std::uint64_t& Data) { AccessInteger(Data); }

void BinaryArchive::Access(bool& Data) {
    std::uint8_t Byte = IsSaving() && Data ? 1 : 0;
    Access(Byte);
    if (IsLoading()) {
        if (Byte > 1) throw std::invalid_argument("invalid archive boolean");
        Data = Byte != 0;
    }
}

void BinaryArchive::Access(double& Data) {
    auto Bits = IsSaving() ? std::bit_cast<std::uint64_t>(Data) : std::uint64_t{};
    Access(Bits);
    if (IsLoading()) Data = std::bit_cast<double>(Bits);
}

void BinaryArchive::Access(std::string& Data) {
    if (IsSaving()) {
        WriteBytes(Data);
        return;
    }
    std::uint32_t Count = 0;
    Access(Count);
    const auto Bytes = Read(Count);
    if (Bytes.empty()) Data.clear();
    else Data.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
}

void BinaryArchive::WriteBytes(std::string_view Data) {
    if (IsLoading()) throw std::logic_error("cannot write to a reading archive");
    if (Data.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("archive field is too large");
    auto Count = static_cast<std::uint32_t>(Data.size());
    Access(Count);
    if (!Data.empty()) Output.insert(Output.end(), Data.begin(), Data.end());
}

std::vector<std::uint8_t> BinaryArchive::Finish() && {
    if (IsLoading()) throw std::logic_error("cannot finish a reading archive");
    return std::move(Output);
}

std::span<const std::uint8_t> BinaryArchive::Read(std::size_t Count) {
    if (Count > Remaining()) throw std::invalid_argument("truncated binary archive");
    auto Bytes = Input.subspan(Position, Count);
    Position += Count;
    return Bytes;
}

} // namespace Feather
