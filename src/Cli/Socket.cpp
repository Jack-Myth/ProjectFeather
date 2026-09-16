#include "Socket.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#undef SendMessage
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Feather::Cli {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
using SocketLength = int;
int LastSocketError() { return WSAGetLastError(); }
void CloseSocket(SocketHandle Handle) { closesocket(Handle); }
void ShutdownSocket(SocketHandle Handle) { shutdown(Handle, SD_BOTH); }
void InitializeSockets() {
    static std::once_flag Once;
    static int Result = 0;
    std::call_once(Once, [] {
        WSADATA Data{};
        Result = WSAStartup(MAKEWORD(2, 2), &Data);
    });
    if (Result != 0) throw std::runtime_error("cannot initialize Winsock: " + std::to_string(Result));
}
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
using SocketLength = socklen_t;
int LastSocketError() { return errno; }
void CloseSocket(SocketHandle Handle) { close(Handle); }
void ShutdownSocket(SocketHandle Handle) { shutdown(Handle, SHUT_RDWR); }
void InitializeSockets() {}
#endif

[[noreturn]] void SocketFailure(std::string_view Action) {
    throw std::runtime_error(std::string(Action) + " failed (socket error " +
                             std::to_string(LastSocketError()) + ")");
}

SocketHandle OpenConnected(const TcpEndpoint& Endpoint) {
    InitializeSockets();
    addrinfo Hints{};
    Hints.ai_family = AF_UNSPEC;
    Hints.ai_socktype = SOCK_STREAM;
    Hints.ai_protocol = IPPROTO_TCP;
    addrinfo* Addresses = nullptr;
    auto Port = std::to_string(Endpoint.Port);
    auto Error = getaddrinfo(Endpoint.Host.c_str(), Port.c_str(), &Hints, &Addresses);
    if (Error != 0) throw std::runtime_error("cannot resolve TCP endpoint");
    SocketHandle Result = InvalidSocket;
    for (auto* Address = Addresses; Address; Address = Address->ai_next) {
        auto Candidate = socket(Address->ai_family, Address->ai_socktype, Address->ai_protocol);
        if (Candidate == InvalidSocket) continue;
        if (connect(Candidate, Address->ai_addr, static_cast<SocketLength>(Address->ai_addrlen)) == 0) {
            Result = Candidate;
            break;
        }
        CloseSocket(Candidate);
    }
    freeaddrinfo(Addresses);
    if (Result == InvalidSocket) SocketFailure("connect");
    return Result;
}

SocketHandle OpenListener(const TcpEndpoint& Endpoint) {
    InitializeSockets();
    addrinfo Hints{};
    Hints.ai_family = AF_UNSPEC;
    Hints.ai_socktype = SOCK_STREAM;
    Hints.ai_protocol = IPPROTO_TCP;
    Hints.ai_flags = AI_PASSIVE;
    addrinfo* Addresses = nullptr;
    auto Port = std::to_string(Endpoint.Port);
    const char* Host = Endpoint.Host.empty() ? nullptr : Endpoint.Host.c_str();
    auto Error = getaddrinfo(Host, Port.c_str(), &Hints, &Addresses);
    if (Error != 0) throw std::runtime_error("cannot resolve TCP listen endpoint");
    SocketHandle Result = InvalidSocket;
    for (auto* Address = Addresses; Address; Address = Address->ai_next) {
        auto Candidate = socket(Address->ai_family, Address->ai_socktype, Address->ai_protocol);
        if (Candidate == InvalidSocket) continue;
        int Reuse = 1;
        setsockopt(Candidate, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&Reuse), sizeof(Reuse));
        if (bind(Candidate, Address->ai_addr, static_cast<SocketLength>(Address->ai_addrlen)) == 0 &&
            listen(Candidate, 1) == 0) {
            Result = Candidate;
            break;
        }
        CloseSocket(Candidate);
    }
    freeaddrinfo(Addresses);
    if (Result == InvalidSocket) SocketFailure("listen");
    return Result;
}

std::size_t ParseLength(std::string_view Header) {
    std::optional<std::size_t> Length;
    std::size_t Start = 0;
    while (Start < Header.size()) {
        auto End = Header.find("\r\n", Start);
        if (End == std::string_view::npos) End = Header.size();
        auto Line = Header.substr(Start, End - Start);
        auto Colon = Line.find(':');
        if (Colon == std::string_view::npos) throw std::runtime_error("invalid framed-message header");
        auto Name = Line.substr(0, Colon);
        std::string Lower(Name);
        std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                       [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        if (Lower == "content-length") {
            if (Length) throw std::runtime_error("duplicate Content-Length header");
            auto Value = Line.substr(Colon + 1);
            while (!Value.empty() && (Value.front() == ' ' || Value.front() == '\t')) Value.remove_prefix(1);
            std::size_t Parsed = 0;
            auto Result = std::from_chars(Value.data(), Value.data() + Value.size(), Parsed);
            if (Result.ec != std::errc{} || Result.ptr != Value.data() + Value.size())
                throw std::runtime_error("invalid Content-Length header");
            Length = Parsed;
        }
        Start = End + 2;
    }
    if (!Length) throw std::runtime_error("missing Content-Length header");
    return *Length;
}

} // namespace

struct TcpConnection::State final {
    explicit State(SocketHandle Input) : Handle(Input) {}
    ~State() { Close(); }
    void Close() noexcept {
        auto Old = Handle.exchange(InvalidSocket);
        if (Old != InvalidSocket) {
            ShutdownSocket(Old);
            CloseSocket(Old);
        }
    }
    std::atomic<SocketHandle> Handle{InvalidSocket};
    std::mutex SendMutex;
};

struct TcpListener::State final {
    explicit State(SocketHandle Input) : Handle(Input) {}
    ~State() { Close(); }
    void Close() noexcept {
        auto Old = Handle.exchange(InvalidSocket);
        if (Old != InvalidSocket) CloseSocket(Old);
    }
    std::atomic<SocketHandle> Handle{InvalidSocket};
};

TcpEndpoint ParseTcpEndpoint(std::string_view Text) {
    auto Colon = Text.rfind(':');
    if (Colon == std::string_view::npos || Colon == 0 || Colon + 1 == Text.size())
        throw std::invalid_argument("TCP endpoint must be host:port");
    auto Host = Text.substr(0, Colon);
    if (Host.front() == '[' && Host.back() == ']') {
        Host.remove_prefix(1);
        Host.remove_suffix(1);
    }
    unsigned Port = 0;
    auto PortText = Text.substr(Colon + 1);
    auto Result = std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port);
    if (Result.ec != std::errc{} || Result.ptr != PortText.data() + PortText.size() ||
        Port == 0 || Port > 65535)
        throw std::invalid_argument("TCP port must be between 1 and 65535");
    return {std::string(Host), static_cast<std::uint16_t>(Port)};
}

std::string FormatTcpEndpoint(const TcpEndpoint& Endpoint) {
    auto Host = Endpoint.Host.find(':') == std::string::npos
        ? Endpoint.Host : "[" + Endpoint.Host + "]";
    return Host + ":" + std::to_string(Endpoint.Port);
}

TcpConnection::TcpConnection(std::shared_ptr<State> Input) : Data(std::move(Input)) {}
TcpConnection::~TcpConnection() { Close(); }
TcpConnection::TcpConnection(TcpConnection&&) noexcept = default;
TcpConnection& TcpConnection::operator=(TcpConnection&&) noexcept = default;

TcpConnection TcpConnection::Connect(const TcpEndpoint& Endpoint) {
    return TcpConnection(std::make_shared<State>(OpenConnected(Endpoint)));
}

void TcpConnection::SendMessage(std::string_view Message) {
    if (!Data) throw std::runtime_error("TCP connection is closed");
    std::string Header = "Content-Length: " + std::to_string(Message.size()) + "\r\n\r\n";
    std::lock_guard Lock(Data->SendMutex);
    auto SendAll = [&](std::string_view Bytes) {
        while (!Bytes.empty()) {
            auto Handle = Data->Handle.load();
            if (Handle == InvalidSocket) throw std::runtime_error("TCP connection is closed");
            auto Count = static_cast<int>(std::min<std::size_t>(Bytes.size(),
                static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
            auto Sent = send(Handle, Bytes.data(), Count, 0);
#else
            auto Sent = send(Handle, Bytes.data(), Count, MSG_NOSIGNAL);
#endif
            if (Sent <= 0) SocketFailure("send");
            Bytes.remove_prefix(static_cast<std::size_t>(Sent));
        }
    };
    SendAll(Header);
    SendAll(Message);
}

std::optional<std::string> TcpConnection::ReceiveMessage(std::size_t MaxMessageBytes) {
    if (!Data) return std::nullopt;
    auto ReceiveByte = [&](char& Byte) -> bool {
        auto Handle = Data->Handle.load();
        if (Handle == InvalidSocket) return false;
        auto Count = recv(Handle, &Byte, 1, 0);
        if (Count == 0) return false;
        if (Count < 0) SocketFailure("receive");
        return true;
    };
    std::string Header;
    while (!Header.ends_with("\r\n\r\n")) {
        char Byte = 0;
        if (!ReceiveByte(Byte)) {
            if (Header.empty()) return std::nullopt;
            throw std::runtime_error("connection closed inside framed-message header");
        }
        Header.push_back(Byte);
        if (Header.size() > 8192) throw std::runtime_error("framed-message header exceeds 8 KiB");
    }
    Header.resize(Header.size() - 4);
    auto Length = ParseLength(Header);
    if (Length > MaxMessageBytes) throw std::runtime_error("framed message exceeds its size limit");
    std::string Message(Length, '\0');
    std::size_t Offset = 0;
    while (Offset < Length) {
        auto Handle = Data->Handle.load();
        if (Handle == InvalidSocket) throw std::runtime_error("connection closed inside framed message");
        auto Count = static_cast<int>(std::min<std::size_t>(Length - Offset,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        auto Received = recv(Handle, Message.data() + Offset, Count, 0);
        if (Received == 0) throw std::runtime_error("connection closed inside framed message");
        if (Received < 0) SocketFailure("receive");
        Offset += static_cast<std::size_t>(Received);
    }
    return Message;
}

void TcpConnection::Close() noexcept {
    if (Data) Data->Close();
}

TcpConnection::operator bool() const noexcept {
    return Data && Data->Handle.load() != InvalidSocket;
}

TcpListener::TcpListener(std::unique_ptr<State> Input) : Data(std::move(Input)) {}
TcpListener::~TcpListener() { Close(); }
TcpListener::TcpListener(TcpListener&&) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&&) noexcept = default;

TcpListener TcpListener::Listen(const TcpEndpoint& Endpoint) {
    return TcpListener(std::make_unique<State>(OpenListener(Endpoint)));
}

TcpConnection TcpListener::Accept() {
    if (!Data) throw std::runtime_error("TCP listener is closed");
    auto Handle = Data->Handle.load();
    auto Accepted = accept(Handle, nullptr, nullptr);
    if (Accepted == InvalidSocket) SocketFailure("accept");
    return TcpConnection(std::make_shared<TcpConnection::State>(Accepted));
}

std::uint16_t TcpListener::GetPort() const {
    if (!Data) throw std::runtime_error("TCP listener is closed");
    sockaddr_storage Address{};
    SocketLength Length = sizeof(Address);
    if (getsockname(Data->Handle.load(), reinterpret_cast<sockaddr*>(&Address), &Length) != 0)
        SocketFailure("getsockname");
    if (Address.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&Address)->sin_port);
    if (Address.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&Address)->sin6_port);
    throw std::runtime_error("unknown TCP address family");
}

void TcpListener::Close() noexcept {
    if (Data) Data->Close();
}

} // namespace Feather::Cli
