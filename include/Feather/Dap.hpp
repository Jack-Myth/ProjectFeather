#pragma once

#include <Feather/Export.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Feather {

// Host-provided message boundary between an IDE-side DAP transport and a
// Feather debug target transport. Messages are complete UTF-8 JSON objects;
// Content-Length framing, sockets and process ownership remain host concerns.
class FEATHER_DEBUG_API DapChannel {
public:
    virtual ~DapChannel() = default;
    virtual void SendDapMessage(std::string_view Message) noexcept = 0;
    virtual void SendTargetMessage(std::string_view Message) noexcept = 0;
};

struct DapAdapterOptions {
    // The primary source is represented by an empty Feather module ID.
    // Entry-relative file: module IDs are mapped to and from source paths.
    // Other module IDs are forwarded unchanged.
    std::string PrimarySourcePath;
    std::size_t MaxMessageBytes = 1024 * 1024;
};

// Transport-independent DAP adapter. It owns no VM and opens no connection.
// Dispatch methods are thread-safe; channel callbacks must not throw.
class FEATHER_DEBUG_API DapAdapter final {
public:
    explicit DapAdapter(std::shared_ptr<DapChannel> Channel,
                        DapAdapterOptions Options = {});
    ~DapAdapter();
    DapAdapter(const DapAdapter&) = delete;
    DapAdapter& operator=(const DapAdapter&) = delete;

    void DispatchDapMessage(std::string_view Message) noexcept;
    void DispatchTargetMessage(std::string_view Message) noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};

} // namespace Feather
