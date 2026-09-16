#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Feather::Cli {

struct TcpEndpoint {
    std::string Host;
    std::uint16_t Port = 0;
};

TcpEndpoint ParseTcpEndpoint(std::string_view Text);
std::string FormatTcpEndpoint(const TcpEndpoint& Endpoint);

// A single full-duplex TCP stream carrying Content-Length framed UTF-8
// messages. Exactly one thread may call ReceiveMessage; SendMessage is
// serialized and may be called from multiple threads.
class TcpConnection final {
public:
    TcpConnection() = default;
    ~TcpConnection();
    TcpConnection(TcpConnection&&) noexcept;
    TcpConnection& operator=(TcpConnection&&) noexcept;
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    static TcpConnection Connect(const TcpEndpoint& Endpoint);
    void SendMessage(std::string_view Message);
    std::optional<std::string> ReceiveMessage(std::size_t MaxMessageBytes);
    void Close() noexcept;
    explicit operator bool() const noexcept;

private:
    struct State;
    explicit TcpConnection(std::shared_ptr<State> Input);
    std::shared_ptr<State> Data;
    friend class TcpListener;
};

class TcpListener final {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    static TcpListener Listen(const TcpEndpoint& Endpoint);
    TcpConnection Accept();
    std::uint16_t GetPort() const;
    void Close() noexcept;

private:
    struct State;
    explicit TcpListener(std::unique_ptr<State> Input);
    std::unique_ptr<State> Data;
};

} // namespace Feather::Cli
