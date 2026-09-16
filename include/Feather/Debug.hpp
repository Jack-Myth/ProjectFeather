#pragma once

#include <Feather/Runtime.hpp>

#include <memory>
#include <cstddef>
#include <string_view>

namespace Feather {

// A transport supplied by the host. Each call carries one complete UTF-8 JSON
// protocol message. Calls may arrive from the transport-dispatch or VM thread.
class FEATHER_DEBUG_API DebugChannel {
public:
    virtual ~DebugChannel() = default;
    virtual void SendProtocolMessage(std::string_view Message) noexcept = 0;
};

// An embeddable, transport-independent Feather debug protocol endpoint.
// DispatchProtocolMessage is thread-safe and never directly accesses the VM.
class FEATHER_DEBUG_API DebugTarget final : public VmDebugController {
public:
    explicit DebugTarget(std::shared_ptr<DebugChannel> Channel,
                         std::size_t MaxMessageBytes = 1024 * 1024);
    ~DebugTarget() override;
    DebugTarget(const DebugTarget&) = delete;
    DebugTarget& operator=(const DebugTarget&) = delete;

    void DispatchProtocolMessage(std::string_view Message) noexcept;
    // Permanently closes this target and releases a paused VM. Call when the
    // host transport disconnects; remove the controller after the VM is idle.
    void Close() noexcept;
    void OnSafePoint(const VmDebugContext& Context) override;
    void OnExecutionFinished(bool Faulted) override;

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};

} // namespace Feather
