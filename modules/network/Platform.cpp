#include "Platform.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Feather::NetworkPlatform {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using Length = int;
Socket Native(Handle H) { return static_cast<SOCKET>(H); }
int LastError() { return WSAGetLastError(); }
bool Again(int E) { return E == WSAEWOULDBLOCK; }
bool Interrupted(int E) { return E == WSAEINTR; }
bool Pending(int E) { return Again(E) || E == WSAEINPROGRESS || E == WSAEALREADY; }
#else
using Socket = int;
using Length = socklen_t;
Socket Native(Handle H) { return static_cast<int>(H); }
int LastError() { return errno; }
bool Again(int E) { return E == EAGAIN || E == EWOULDBLOCK; }
bool Interrupted(int E) { return E == EINTR; }
bool Pending(int E) { return E == EINPROGRESS || E == EALREADY || Interrupted(E); }
#endif
[[noreturn]] void Fail(const char* Operation, int Code) {
    throw std::runtime_error(std::string("network ") + Operation + " failed (OS error " +
                             std::to_string(Code) + ")");
}
int Domain(Family Version) { return Version == Family::IPv4 ? AF_INET : AF_INET6; }
struct Endpoint { sockaddr_storage Data{}; Length Size = 0; };
Endpoint Numeric(Family Version, const std::string& Host, std::uint16_t Port) {
    if (Host.empty() || Host.find('\0') != std::string::npos)
        throw std::invalid_argument("network expects a numeric IP address");
    Endpoint Result;
    if (Version == Family::IPv4) {
        auto* A = reinterpret_cast<sockaddr_in*>(&Result.Data);
        A->sin_family = AF_INET; A->sin_port = htons(Port); Result.Size = sizeof(*A);
        if (inet_pton(AF_INET, Host.c_str(), &A->sin_addr) != 1)
            throw std::invalid_argument("network expects a numeric IPv4 address");
    } else {
        auto* A = reinterpret_cast<sockaddr_in6*>(&Result.Data);
        A->sin6_family = AF_INET6; A->sin6_port = htons(Port); Result.Size = sizeof(*A);
        if (inet_pton(AF_INET6, Host.c_str(), &A->sin6_addr) != 1)
            throw std::invalid_argument("network expects a numeric IPv6 address without a scope ID");
    }
    return Result;
}
Address Describe(const sockaddr_storage& Input) {
    char Text[INET6_ADDRSTRLEN]{};
    const void* Bytes;
    std::uint16_t Port;
    if (Input.ss_family == AF_INET) {
        auto* A = reinterpret_cast<const sockaddr_in*>(&Input);
        Bytes = &A->sin_addr; Port = ntohs(A->sin_port);
    } else if (Input.ss_family == AF_INET6) {
        auto* A = reinterpret_cast<const sockaddr_in6*>(&Input);
        Bytes = &A->sin6_addr; Port = ntohs(A->sin6_port);
    } else throw std::runtime_error("network unsupported address family");
    if (!inet_ntop(Input.ss_family, Bytes, Text, sizeof(Text))) Fail("address", LastError());
    return {Text, Port};
}
void Configure(Handle H) {
    auto S = Native(H);
#ifdef _WIN32
    u_long Mode = 1;
    if (ioctlsocket(S, FIONBIO, &Mode) != 0) Fail("nonblocking", LastError());
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(S), HANDLE_FLAG_INHERIT, 0))
        Fail("non-inheritable socket", static_cast<int>(GetLastError()));
#else
    int Flags = fcntl(S, F_GETFL, 0);
    if (Flags == -1 || fcntl(S, F_SETFL, Flags | O_NONBLOCK) == -1) Fail("nonblocking", LastError());
    Flags = fcntl(S, F_GETFD, 0);
    if (Flags == -1 || fcntl(S, F_SETFD, Flags | FD_CLOEXEC) == -1) Fail("close-on-exec", LastError());
#ifdef SO_NOSIGPIPE
    int Enabled = 1;
    if (setsockopt(S, SOL_SOCKET, SO_NOSIGPIPE, &Enabled, sizeof(Enabled)) != 0)
        Fail("SO_NOSIGPIPE", LastError());
#endif
#endif
}
constexpr int SendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
} // namespace

Runtime::Runtime() {
#ifdef _WIN32
    WSADATA Data{};
    int Code = WSAStartup(MAKEWORD(2, 2), &Data);
    if (Code != 0) Fail("WSAStartup", Code);
    if (Data.wVersion != MAKEWORD(2, 2)) {
        WSACleanup();
        throw std::runtime_error("network requires Winsock 2.2");
    }
#endif
}
Runtime::~Runtime() {
#ifdef _WIN32
    WSACleanup();
#endif
}
void Close(Handle H) noexcept {
    if (H == Invalid) return;
#ifdef _WIN32
    closesocket(Native(H));
#else
    // Never retry close after EINTR: the descriptor may already have been reused.
    close(Native(H));
#endif
}
Handle Open(Family Version, bool Tcp) {
    auto S = socket(Domain(Version), Tcp ? SOCK_STREAM : SOCK_DGRAM, Tcp ? IPPROTO_TCP : IPPROTO_UDP);
#ifdef _WIN32
    if (S == INVALID_SOCKET) Fail("socket", LastError());
#else
    if (S < 0) Fail("socket", LastError());
#endif
    auto H = static_cast<Handle>(S);
    try {
        Configure(H);
        if (Version == Family::IPv6) {
            int Enabled = 1;
            if (setsockopt(S, IPPROTO_IPV6, IPV6_V6ONLY,
                          reinterpret_cast<const char*>(&Enabled), sizeof(Enabled)) != 0)
                Fail("IPV6_V6ONLY", LastError());
        }
    } catch (...) { Close(H); throw; }
    return H;
}
void Bind(Handle H, Family Version, const std::string& Host, std::uint16_t Port) {
    auto A = Numeric(Version, Host, Port);
    if (bind(Native(H), reinterpret_cast<sockaddr*>(&A.Data), A.Size) != 0) Fail("bind", LastError());
}
void Listen(Handle H, int Backlog) {
    if (listen(Native(H), Backlog) != 0) Fail("listen", LastError());
}
bool Connect(Handle H, Family Version, const std::string& Host, std::uint16_t Port) {
    auto A = Numeric(Version, Host, Port);
    if (connect(Native(H), reinterpret_cast<sockaddr*>(&A.Data), A.Size) == 0) return true;
    int Code = LastError();
    if (Pending(Code)) return false;
    Fail("connect", Code);
}
Ready Poll(Handle H, bool Read, bool Write, int Milliseconds) {
    using Clock = std::chrono::steady_clock;
    auto Deadline = Clock::now() + std::chrono::milliseconds(Milliseconds);
    for (;;) {
        auto Remaining = std::chrono::ceil<std::chrono::milliseconds>(Deadline - Clock::now()).count();
        int Wait = static_cast<int>(std::max<std::int64_t>(0, Remaining));
#ifdef _WIN32
        fd_set Reads, Writes, Errors;
        FD_ZERO(&Reads); FD_ZERO(&Writes); FD_ZERO(&Errors);
        if (Read) FD_SET(Native(H), &Reads);
        if (Write) FD_SET(Native(H), &Writes);
        FD_SET(Native(H), &Errors);
        timeval Timeout{Wait / 1000, (Wait % 1000) * 1000};
        int Result = select(0, &Reads, &Writes, &Errors, &Timeout);
        if (Result >= 0) {
            bool Error = FD_ISSET(Native(H), &Errors) != 0;
            return {Read && (FD_ISSET(Native(H), &Reads) || Error),
                    Write && (FD_ISSET(Native(H), &Writes) || Error)};
        }
#else
        pollfd Item{Native(H), static_cast<short>((Read ? POLLIN : 0) | (Write ? POLLOUT : 0)), 0};
        int Result = poll(&Item, 1, Wait);
        if (Result >= 0) {
            if (Item.revents & POLLNVAL) throw std::runtime_error("network poll: invalid socket");
            bool Error = (Item.revents & (POLLERR | POLLHUP)) != 0;
            return {Read && ((Item.revents & POLLIN) || Error),
                    Write && ((Item.revents & POLLOUT) || Error)};
        }
#endif
        int Code = LastError();
        if (!Interrupted(Code)) Fail("poll", Code);
        if (Clock::now() >= Deadline) return {};
    }
}
bool FinishConnect(Handle H) {
    if (!Poll(H, false, true, 0).Writable) return false;
    int Code = 0; Length Size = sizeof(Code);
    if (getsockopt(Native(H), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&Code), &Size) != 0)
        Fail("connect status", LastError());
    if (Code) Fail("connect", Code);
    return true;
}
Handle Accept(Handle H) {
    for (;;) {
        auto S = accept(Native(H), nullptr, nullptr);
#ifdef _WIN32
        bool Good = S != INVALID_SOCKET;
#else
        bool Good = S >= 0;
#endif
        if (Good) {
            auto Accepted = static_cast<Handle>(S);
            try { Configure(Accepted); }
            catch (...) { Close(Accepted); throw; }
            return Accepted;
        }
        int Code = LastError();
        if (Again(Code)) return Invalid;
        if (!Interrupted(Code)) Fail("accept", Code);
    }
}
void ShutdownWrite(Handle H) {
#ifdef _WIN32
    int How = SD_SEND;
#else
    int How = SHUT_WR;
#endif
    if (shutdown(Native(H), How) != 0) Fail("shutdown", LastError());
}
Address LocalAddress(Handle H) {
    sockaddr_storage A{}; Length Size = sizeof(A);
    if (getsockname(Native(H), reinterpret_cast<sockaddr*>(&A), &Size) != 0) Fail("local address", LastError());
    return Describe(A);
}
Address PeerAddress(Handle H) {
    sockaddr_storage A{}; Length Size = sizeof(A);
    if (getpeername(Native(H), reinterpret_cast<sockaddr*>(&A), &Size) != 0) Fail("peer address", LastError());
    return Describe(A);
}
std::ptrdiff_t Send(Handle H, std::string_view Data) {
    for (;;) {
        auto Count = send(Native(H), Data.data(), static_cast<int>(Data.size()), SendFlags());
        if (Count >= 0) return Count;
        int Code = LastError();
        if (Again(Code)) return -1;
        if (!Interrupted(Code)) Fail("send", Code);
    }
}
std::ptrdiff_t Receive(Handle H, char* Data, std::size_t Size) {
    for (;;) {
        auto Count = recv(Native(H), Data, static_cast<int>(Size), 0);
        if (Count >= 0) return Count;
        int Code = LastError();
        if (Again(Code)) return -1;
        if (!Interrupted(Code)) Fail("receive", Code);
    }
}
std::ptrdiff_t SendTo(Handle H, Family Version, std::string_view Data,
                     const std::string& Host, std::uint16_t Port) {
    auto A = Numeric(Version, Host, Port);
    for (;;) {
        auto Count = sendto(Native(H), Data.data(), static_cast<int>(Data.size()), SendFlags(),
                            reinterpret_cast<sockaddr*>(&A.Data), A.Size);
        if (Count >= 0) return Count;
        int Code = LastError();
        if (Again(Code)) return -1;
        if (!Interrupted(Code)) Fail("sendto", Code);
    }
}
Datagram ReceiveFrom(Handle H, char* Data, std::size_t Size) {
    for (;;) {
        sockaddr_storage A{};
#ifdef _WIN32
        Length AddressSize = sizeof(A);
        auto Count = recvfrom(Native(H), Data, static_cast<int>(Size), 0,
                              reinterpret_cast<sockaddr*>(&A), &AddressSize);
        if (Count >= 0) return {Count, false, Describe(A)};
        int Code = LastError();
        if (Code == WSAEMSGSIZE) return {0, true, {}};
#else
        iovec Buffer{Data, Size};
        msghdr Message{};
        Message.msg_name = &A; Message.msg_namelen = sizeof(A);
        Message.msg_iov = &Buffer; Message.msg_iovlen = 1;
        auto Count = recvmsg(Native(H), &Message, 0);
        if (Count >= 0) return {Count, (Message.msg_flags & MSG_TRUNC) != 0, Describe(A)};
        int Code = LastError();
#endif
        if (Again(Code)) return {};
        if (!Interrupted(Code)) Fail("recvfrom", Code);
    }
}
std::string Resolve(const std::string& Host, Family Version) {
    if (Host.empty() || Host.size() > 253 || Host.find('\0') != std::string::npos)
        throw std::invalid_argument("network invalid host name");
    addrinfo Hints{}; Hints.ai_family = Domain(Version); Hints.ai_socktype = SOCK_STREAM;
    addrinfo* Addresses = nullptr;
    int Code = getaddrinfo(Host.c_str(), nullptr, &Hints, &Addresses);
    if (Code != 0) Fail("resolve", Code);
    struct Cleanup { addrinfo* Data; ~Cleanup() { freeaddrinfo(Data); } } Owner{Addresses};
    if (!Addresses) throw std::runtime_error("network no matching address");
    sockaddr_storage A{};
    if (Addresses->ai_addrlen > sizeof(A)) throw std::runtime_error("network invalid resolver address");
    std::copy_n(reinterpret_cast<const char*>(Addresses->ai_addr), Addresses->ai_addrlen,
                reinterpret_cast<char*>(&A));
    return Describe(A).Host;
}
} // namespace Feather::NetworkPlatform
