#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Feather::NetworkPlatform {
using Handle = std::uintptr_t;
inline constexpr Handle Invalid = static_cast<Handle>(-1);
enum class Family { IPv4, IPv6 };
struct Address { std::string Host; std::uint16_t Port = 0; };
struct Ready { bool Readable = false, Writable = false; };
struct Datagram { std::ptrdiff_t Count = -1; bool Truncated = false; Address From; };
class Runtime final {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};
Handle Open(Family Version, bool Tcp);
void Close(Handle Socket) noexcept;
void Bind(Handle Socket, Family Version, const std::string& Host, std::uint16_t Port);
void Listen(Handle Socket, int Backlog);
bool Connect(Handle Socket, Family Version, const std::string& Host, std::uint16_t Port);
bool FinishConnect(Handle Socket);
Handle Accept(Handle Socket);
void ShutdownWrite(Handle Socket);
Address LocalAddress(Handle Socket);
Address PeerAddress(Handle Socket);
Ready Poll(Handle Socket, bool Read, bool Write, int Milliseconds);
std::ptrdiff_t Send(Handle Socket, std::string_view Data);
std::ptrdiff_t Receive(Handle Socket, char* Data, std::size_t Size);
std::ptrdiff_t SendTo(Handle Socket, Family Version, std::string_view Data,
                      const std::string& Host, std::uint16_t Port);
Datagram ReceiveFrom(Handle Socket, char* Data, std::size_t Size);
std::string Resolve(const std::string& Host, Family Version);
} // namespace Feather::NetworkPlatform
