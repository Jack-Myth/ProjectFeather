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

struct DebugTargetOptions {
    std::size_t MaxMessageBytes = 1024 * 1024;
    // A host may start its VM only after the remote debugger sends
    // Runtime.runIfWaitingForDebugger. Ordinary embedded targets leave this
    // disabled and retain their existing execution lifecycle.
    bool WaitForDebugger = false;
    // Disable this when several Vm::Run calls form one user-visible program,
    // then call NotifyExecutionFinished once for the whole program.
    bool ReportEachExecution = true;
};

// An embeddable, transport-independent Feather debug protocol endpoint.
// DispatchProtocolMessage is thread-safe and never directly accesses the VM.
class FEATHER_DEBUG_API DebugTarget final : public VmDebugController {
public:
    explicit DebugTarget(std::shared_ptr<DebugChannel> Channel,
                         DebugTargetOptions Options = {});
    ~DebugTarget() override;
    DebugTarget(const DebugTarget&) = delete;
    DebugTarget& operator=(const DebugTarget&) = delete;

    void DispatchProtocolMessage(std::string_view Message) noexcept;
    // Used by hosts that selected WaitForDebugger. Returns false if the
    // target was closed before execution was released.
    bool WaitForExecutionPermission() noexcept;
    void NotifyExecutionFinished(bool Faulted) noexcept;
    // Permanently closes this target and releases a paused VM. Call when the
    // host transport disconnects; remove the controller after the VM is idle.
    void Close() noexcept;
    void OnSafePoint(const VmDebugContext& Context) override;
    void OnError(const VmDebugContext& Context, const Value& Error,
                 DebugErrorOrigin Origin) override;
    void OnExecutionFinished(bool Faulted) override;

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};

} // namespace Feather
