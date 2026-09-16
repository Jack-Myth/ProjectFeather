#include "DebugHost.hpp"
#include "Socket.hpp"

#include <Feather/Debug.hpp>

#include <iostream>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Feather::Cli {
namespace {

constexpr std::size_t MaxDebugMessageBytes = 1024 * 1024;

class SocketDebugChannel final : public DebugChannel {
public:
    void Attach(std::shared_ptr<TcpConnection> Input) {
        std::lock_guard Lock(Mutex);
        if (Closed) {
            Input->Close();
            return;
        }
        Connection = std::move(Input);
    }

    void SendProtocolMessage(std::string_view Message) noexcept override {
        std::lock_guard Lock(Mutex);
        if (Closed || !Connection) return;
        try { Connection->SendMessage(Message); }
        catch (...) { Connection->Close(); }
    }

    void Close() noexcept {
        std::lock_guard Lock(Mutex);
        Closed = true;
        if (Connection) Connection->Close();
    }

private:
    std::mutex Mutex;
    std::shared_ptr<TcpConnection> Connection;
    bool Closed = false;
};

} // namespace

int RunDebugProgram(const CompiledProgram& Program, const std::filesystem::path& EntryPath,
                    ModuleFileKind Kind, std::string_view ListenEndpoint,
                    bool WaitForDebugger) {
    auto Endpoint = ParseTcpEndpoint(ListenEndpoint);
    auto Listener = std::make_shared<TcpListener>(TcpListener::Listen(Endpoint));
    std::cerr << "debug target listening on " << FormatTcpEndpoint(Endpoint) << '\n';

    auto Channel = std::make_shared<SocketDebugChannel>();
    auto Target = std::make_shared<DebugTarget>(Channel, DebugTargetOptions{
        .MaxMessageBytes = MaxDebugMessageBytes,
        .WaitForDebugger = WaitForDebugger,
        .ReportEachExecution = false,
    });
    std::mutex FailureMutex;
    std::condition_variable ConnectionChanged;
    std::string TransportFailure;
    bool Connected = false;
    bool ReceiverFinished = false;
    std::jthread Receiver([&] {
        try {
            auto Connection = std::make_shared<TcpConnection>(Listener->Accept());
            Listener->Close();
            Channel->Attach(Connection);
            {
                std::lock_guard Lock(FailureMutex);
                Connected = true;
            }
            ConnectionChanged.notify_all();
            while (auto Message = Connection->ReceiveMessage(MaxDebugMessageBytes))
                Target->DispatchProtocolMessage(*Message);
        } catch (const std::exception& Failure) {
            std::lock_guard Lock(FailureMutex);
            TransportFailure = Failure.what();
        }
        Target->Close();
        {
            std::lock_guard Lock(FailureMutex);
            ReceiverFinished = true;
        }
        ConnectionChanged.notify_all();
    });

    if (WaitForDebugger) {
        {
            std::unique_lock Lock(FailureMutex);
            ConnectionChanged.wait(Lock, [&] { return Connected || ReceiverFinished; });
            if (!Connected) {
                auto Failure = TransportFailure;
                Lock.unlock();
                Listener->Close();
                Receiver.join();
                throw std::runtime_error(Failure.empty() ?
                    "debugger disconnected before connecting" : "debug transport: " + Failure);
            }
        }
        if (!Target->WaitForExecutionPermission()) {
            Channel->Close();
            Receiver.join();
            throw std::runtime_error("debugger disconnected before execution started");
        }
    }

    try {
        auto Result = RunProgram(Program, EntryPath, Kind, Target);
        Target->NotifyExecutionFinished(false);
        if (!WaitForDebugger) {
            Target->Close();
            Listener->Close();
            Channel->Close();
        }
        Receiver.join();
        Channel->Close();
        return Result;
    } catch (...) {
        Target->NotifyExecutionFinished(true);
        if (!WaitForDebugger) {
            Target->Close();
            Listener->Close();
            Channel->Close();
        }
        Receiver.join();
        Channel->Close();
        throw;
    }
}

} // namespace Feather::Cli
