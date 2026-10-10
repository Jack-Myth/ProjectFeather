#include "Network.hpp"
#include "Platform.hpp"
#include <Feather/ByteBuffer.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>

namespace Feather {
namespace {
namespace Net = NetworkPlatform;
Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}
using Method = std::function<Value(const std::vector<Value>&)>;
class Function final : public NativeObject {
public:
    Function(NativeObjectType& Type, Method Body) : NativeObject(Type), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& A) override {
        try { return Body(A); }
        catch (const std::exception& Failure) { return Error(Failure.what()); }
    }
private:
    Method Body;
};
class Namespace final : public NativeObject {
public:
    explicit Namespace(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    void Add(std::string Name, Value Input) { Members.emplace(std::move(Name), std::move(Input)); }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String) return Error("network member name must be a string");
        auto Found = Members.find(Key.AsString());
        return Found == Members.end() ? Error("unknown network member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};
void Arity(const std::vector<Value>& A, std::size_t Minimum, std::size_t Maximum) {
    if (A.size() < Minimum || A.size() > Maximum)
        throw std::invalid_argument("network invalid argument count");
}
std::size_t Integer(const Value& Input, std::size_t Maximum, const char* Name) {
    if (!Input.IsNumeric() || !std::isfinite(Input.AsNumber()) ||
        Input.AsNumber() < 0 || Input.AsNumber() > static_cast<double>(Maximum) ||
        std::trunc(Input.AsNumber()) != Input.AsNumber())
        throw std::invalid_argument(std::string("network invalid ") + Name);
    return static_cast<std::size_t>(Input.AsNumber());
}
const std::string& Text(const Value& Input) {
    if (Input.GetType() != ValueType::String) throw std::invalid_argument("network expected string");
    return Input.AsString();
}
Net::Family Family(const std::vector<Value>& A, std::size_t Index) {
    if (A.size() <= Index) return Net::Family::IPv4;
    auto& Name = Text(A[Index]);
    if (Name == "ipv4") return Net::Family::IPv4;
    if (Name == "ipv6") return Net::Family::IPv6;
    throw std::invalid_argument("network family must be ipv4 or ipv6");
}
std::uint16_t Port(const Value& Input, bool AllowZero = false) {
    auto Number = Integer(Input, 65535, "port");
    if (Number == 0 && !AllowZero) throw std::invalid_argument("network destination port must be nonzero");
    return static_cast<std::uint16_t>(Number);
}
std::string_view Data(const Value& Input, char& SingleByte) {
    if (Input.GetType() == ValueType::Byte) {
        SingleByte = static_cast<char>(Input.AsByte());
        return {&SingleByte, 1};
    }
    if (Input.GetType() == ValueType::String) return Input.AsString();
    if (const auto* Buffer = ByteBufferData(Input)) return *Buffer;
    throw std::invalid_argument("network data must be byte, string or ByteBuffer");
}
class SocketObject;
struct State {
    explicit State(NetworkLimits Limits) : Limits(Limits) {}
    Net::Runtime Runtime;
    NetworkLimits Limits;
    std::size_t ActiveSockets = 0;
    std::vector<std::weak_ptr<SocketObject>> Readers;
    std::size_t NextReader = 0;
    std::shared_ptr<IdleRegistration> Service;
};
enum class Phase { Created, Bound, Listening, Connecting, Connected, Closed };
struct Connection {
    Connection(std::shared_ptr<State> Owner, Net::Family Version, bool Tcp)
        : Owner(std::move(Owner)), Version(Version), Tcp(Tcp) {}
    ~Connection() { Close(); }
    void Adopt(Net::Handle Input) { Handle = Input; ++Owner->ActiveSockets; }
    void Close() noexcept {
        if (Handle != Net::Invalid) {
            Net::Close(Handle); Handle = Net::Invalid; --Owner->ActiveSockets;
        }
        Status = Phase::Closed;
    }
    std::shared_ptr<State> Owner;
    Net::Family Version;
    bool Tcp;
    Net::Handle Handle = Net::Invalid;
    Phase Status = Phase::Created;
    bool WriteClosed = false;
    bool ReadClosed = false;
    bool Receiving = false;
};
class SocketObject final : public NativeObject {
public:
    SocketObject(NativeObjectType& Type, std::shared_ptr<Connection> Body)
        : NativeObject(Type), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    std::shared_ptr<Connection> Body;
    Value Callback;
    std::uint64_t Generation = 0;
};
class SocketType final : public NativeObjectType {
public:
    SocketType(Vm& Machine, std::shared_ptr<State> Shared)
        : NativeObjectType(Machine, NetworkModuleGuid, "Socket"), Shared(std::move(Shared)) {}
    std::shared_ptr<Connection> Allocate(Net::Family Version, bool Tcp) {
        if (Shared->ActiveSockets >= Shared->Limits.MaxSockets)
            throw std::runtime_error("network socket limit exceeded");
        return std::make_shared<Connection>(Shared, Version, Tcp);
    }
    Value Wrap(std::shared_ptr<Connection> Body) {
        return Value::FromObject(CreateObject<SocketObject>(std::move(Body)));
    }
    SocketObject& GetObject(const Value& Input) {
        if (Input.GetType() != ValueType::Object || Input.IsScriptObject())
            throw std::invalid_argument("network expected socket");
        auto* Socket = dynamic_cast<SocketObject*>(Input.AsObject());
        if (!Socket || Socket->GetNativeObjectType() != this)
            throw std::invalid_argument("network expected socket from this module");
        return *Socket;
    }
    Connection& Get(const Value& Input, bool AllowClosed = false) {
        auto& Socket = GetObject(Input);
        if (!AllowClosed && Socket.Body->Status == Phase::Closed)
            throw std::invalid_argument("network socket is closed");
        return *Socket.Body;
    }
    std::vector<std::uint8_t> Serialize(const NativeObject&) const override {
        throw std::invalid_argument("network sockets cannot be saved; close and release them first");
    }
    std::shared_ptr<NativeObject> Deserialize(std::uint32_t, std::span<const std::uint8_t>) override {
        throw std::invalid_argument("network sockets cannot be restored");
    }
    std::shared_ptr<State> Shared;
};
void TcpConnected(const Connection& C) {
    if (!C.Tcp || C.Status != Phase::Connected)
        throw std::invalid_argument("network expected connected TCP socket");
}
void Udp(const Connection& C) {
    if (C.Tcp) throw std::invalid_argument("network expected UDP socket");
}
Value Record(Vm& Machine, std::initializer_list<std::pair<const char*, Value>> Fields) {
    auto* Object = Machine.CreateScriptObject();
    for (const auto& [Name, Input] : Fields) Object->SetRaw(Value::String(Name), Input);
    return Value::FromScript(Object);
}
Value Address(Vm& Machine, const Net::Address& A) {
    return Record(Machine, {{"Address", Value::String(A.Host)}, {"Port", Value::Number(A.Port)}});
}
void ClearCallback(SocketObject& Socket) {
    Socket.Body->Receiving = false;
    Socket.Callback = {};
    Socket.RemoveGcVisibleMember("receiveCallback");
    ++Socket.Generation;
}
Value DispatchOne(Vm& Machine, const std::shared_ptr<State>& Shared, const ByteBufferLibrary& Buffers) {
    auto& Readers = Shared->Readers;
    if (Readers.empty()) return Value::Bool(false);
    Shared->NextReader %= Readers.size();
    std::rotate(Readers.begin(), Readers.begin() + Shared->NextReader, Readers.end());
    std::erase_if(Readers, [](const auto& Entry) {
        auto Socket = Entry.lock();
        return !Socket || !Socket->Body->Receiving;
    });
    Shared->NextReader = 0;
    auto Count = Readers.size();
    for (std::size_t I = 0; I < Count; ++I) {
        auto Socket = Readers[Shared->NextReader++].lock();
        if (!Socket || !Socket->Body->Receiving) continue;
        auto& C = *Socket->Body;
        Value Callback = Socket->Callback;
        auto CallbackRoot = Machine.AddToRoot(Callback);
        auto Generation = Socket->Generation;
        std::string Kind = "data";
        std::string Bytes;
        Net::Address From;
        Value Failure;
        try {
            if (C.Tcp) {
                Bytes.resize(std::min<std::size_t>(65536, Shared->Limits.MaxTransferBytes));
                auto Read = Net::Receive(C.Handle, Bytes.data(), Bytes.size());
                if (Read < 0) continue;
                Bytes.resize(static_cast<std::size_t>(Read));
                if (Read == 0) { Kind = "eof"; C.ReadClosed = true; }
            } else {
                auto Limit = std::min<std::size_t>(65507, Shared->Limits.MaxTransferBytes);
                Bytes.resize(Limit + 1);
                auto Packet = Net::ReceiveFrom(C.Handle, Bytes.data(), Bytes.size());
                if (Packet.Count < 0) continue;
                if (Packet.Truncated || static_cast<std::size_t>(Packet.Count) > Limit)
                    throw std::runtime_error("network datagram exceeds receive limit; datagram discarded");
                Bytes.resize(static_cast<std::size_t>(Packet.Count));
                From = std::move(Packet.From);
            }
        } catch (const std::runtime_error& ErrorValue) {
            Kind = "error"; Failure = Error(ErrorValue.what());
            if (C.Tcp) C.Close();
        }
        if (Kind != "data") ClearCallback(*Socket);
        auto Event = Record(Machine, {{"Kind", Value::String(Kind)},
            {"Socket", Value::FromObject(Socket)}});
        auto EventRoot = Machine.AddToRoot(Event);
        if (Kind == "data") {
            Event.AsScriptObject()->SetRaw(Value::String("Data"), Buffers.Create(std::move(Bytes)));
            if (!C.Tcp) {
                Event.AsScriptObject()->SetRaw(Value::String("Address"), Value::String(From.Host));
                Event.AsScriptObject()->SetRaw(Value::String("Port"), Value::Number(From.Port));
            }
        } else if (Kind == "error") Event.AsScriptObject()->SetRaw(Value::String("Error"), Failure);
        Value Result;
        try { Result = Machine.Call(Callback, {Event}); }
        catch (...) {
            if (Socket->Generation == Generation) ClearCallback(*Socket);
            throw;
        }
        if (Result.IsError()) {
            if (Socket->Generation == Generation) ClearCallback(*Socket);
            return Result;
        }
        return Value::Bool(true);
    }
    return Value::Bool(false);
}
} // namespace

NetworkLibrary::NetworkLibrary(Vm& Machine, NetworkLimits Limits) {
    if (Limits.MaxSockets == 0 || Limits.MaxSockets > 65536 ||
        Limits.MaxTransferBytes == 0 || Limits.MaxTransferBytes > 16 * 1024 * 1024)
        throw std::invalid_argument("network invalid resource limits");
    auto Shared = std::make_shared<State>(Limits);
    ByteBufferLibrary Buffers(Machine);
    auto& Sockets = Machine.CreateNativeObjectType<SocketType>(Shared);
    auto& ModuleType = Machine.CreateNativeObjectType<NativeSingletonType>(NetworkModuleGuid, "Module");
    auto Module = ModuleType.Create<Namespace>();
    auto Add = [&](std::string Name, Method Body) {
        auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(NetworkModuleGuid, Name);
        Module->Add(std::move(Name), Value::FromObject(Type.Create<Function>(std::move(Body))));
    };
    auto Create = [&Sockets](bool Tcp, const std::vector<Value>& A) {
        Arity(A, 0, 1);
        auto Body = Sockets.Allocate(Family(A, 0), Tcp);
        Body->Adopt(Net::Open(Body->Version, Tcp));
        return Sockets.Wrap(std::move(Body));
    };
    Add("CreateTcp", [Create](const auto& A) { return Create(true, A); });
    Add("CreateUdp", [Create](const auto& A) { return Create(false, A); });
    Add("Bind", [&Sockets](const auto& A) {
        Arity(A, 3, 3); auto& C = Sockets.Get(A[0]);
        if (C.Status != Phase::Created) return Error("network Bind requires a new socket");
        Net::Bind(C.Handle, C.Version, Text(A[1]), Port(A[2], true));
        C.Status = Phase::Bound;
        return Value{};
    });
    Add("Listen", [&Sockets](const auto& A) {
        Arity(A, 1, 2); auto& C = Sockets.Get(A[0]);
        if (!C.Tcp || C.Status != Phase::Bound) return Error("network Listen requires a bound TCP socket");
        auto Backlog = A.size() == 2 ? Integer(A[1], 1024, "backlog") : 16;
        if (Backlog == 0) return Error("network backlog must be positive");
        Net::Listen(C.Handle, static_cast<int>(Backlog)); C.Status = Phase::Listening;
        return Value{};
    });
    Add("Connect", [&Sockets](const auto& A) {
        Arity(A, 3, 3); auto& C = Sockets.Get(A[0]);
        if (!C.Tcp || (C.Status != Phase::Created && C.Status != Phase::Bound))
            return Error("network Connect requires a new or bound TCP socket");
        const auto& Host = Text(A[1]); auto Destination = Port(A[2]);
        try {
            bool Done = Net::Connect(C.Handle, C.Version, Host, Destination);
            C.Status = Done ? Phase::Connected : Phase::Connecting;
            return Value::Bool(Done);
        } catch (const std::runtime_error&) { C.Close(); throw; }
    });
    Add("FinishConnect", [&Sockets](const auto& A) {
        Arity(A, 1, 1); auto& C = Sockets.Get(A[0]);
        if (!C.Tcp) return Error("network FinishConnect requires TCP");
        if (C.Status == Phase::Connected) return Value::Bool(true);
        if (C.Status != Phase::Connecting) return Error("network no connection is pending");
        try {
            bool Done = Net::FinishConnect(C.Handle);
            if (Done) C.Status = Phase::Connected;
            return Value::Bool(Done);
        } catch (const std::runtime_error&) { C.Close(); throw; }
    });
    Add("Accept", [&Sockets](const auto& A) {
        Arity(A, 1, 1); auto& C = Sockets.Get(A[0]);
        if (C.Status != Phase::Listening) return Error("network Accept requires a listener");
        auto Body = Sockets.Allocate(C.Version, true);
        auto Accepted = Net::Accept(C.Handle);
        if (Accepted == Net::Invalid) return Value{};
        Body->Adopt(Accepted); Body->Status = Phase::Connected;
        return Sockets.Wrap(std::move(Body));
    });
    Add("Poll", [&Sockets, &Machine](const auto& A) {
        Arity(A, 3, 4); auto& C = Sockets.Get(A[0]);
        if (A[1].GetType() != ValueType::Bool || A[2].GetType() != ValueType::Bool ||
            (!A[1].AsBool() && !A[2].AsBool()))
            return Error("network Poll requires read/write booleans, at least one true");
        auto Timeout = A.size() == 4 ? Integer(A[3], 60000, "timeout milliseconds") : 0;
        auto Ready = Net::Poll(C.Handle, A[1].AsBool(), A[2].AsBool(), static_cast<int>(Timeout));
        return Record(Machine, {{"Readable", Value::Bool(Ready.Readable)},
            {"Writable", Value::Bool(Ready.Writable)},
            {"TimedOut", Value::Bool(!Ready.Readable && !Ready.Writable)}});
    });
    Add("Send", [&Sockets](const auto& A) {
        Arity(A, 2, 4); auto& C = Sockets.Get(A[0]); TcpConnected(C);
        if (C.WriteClosed) return Error("network write side is closed");
        char Single = 0; auto Bytes = Data(A[1], Single);
        auto Offset = A.size() > 2 ? Integer(A[2], Bytes.size(), "send offset") : 0;
        auto Count = A.size() > 3 ? Integer(A[3], Bytes.size() - Offset, "send count") : Bytes.size() - Offset;
        if (Count > C.Owner->Limits.MaxTransferBytes) return Error("network transfer limit exceeded");
        if (Count == 0) return Value::Number(0);
        try {
            auto Written = Net::Send(C.Handle, Bytes.substr(Offset, Count));
            if (Written < 0) return Value{};
            return Value::Number(static_cast<double>(Written));
        } catch (const std::runtime_error&) { C.Close(); throw; }
    });
    Add("Receive", [&Sockets, Buffers](const auto& A) {
        Arity(A, 1, 2); auto& C = Sockets.Get(A[0]); TcpConnected(C);
        if (C.Receiving) return Error("network receive callback owns this socket");
        if (C.ReadClosed) return Value::Bool(false);
        auto Count = A.size() == 2 ? Integer(A[1], C.Owner->Limits.MaxTransferBytes, "receive count")
                                  : std::min<std::size_t>(65536, C.Owner->Limits.MaxTransferBytes);
        if (Count == 0) return Error("network TCP receive count must be positive");
        std::string Bytes(Count, '\0');
        std::ptrdiff_t Read;
        try { Read = Net::Receive(C.Handle, Bytes.data(), Bytes.size()); }
        catch (const std::runtime_error&) { C.Close(); throw; }
        if (Read < 0) return Value{};
        if (Read == 0) { C.ReadClosed = true; return Value::Bool(false); }
        Bytes.resize(static_cast<std::size_t>(Read));
        return Buffers.Create(std::move(Bytes));
    });
    Add("SendTo", [&Sockets](const auto& A) {
        Arity(A, 4, 4); auto& C = Sockets.Get(A[0]); Udp(C);
        char Single = 0; auto Bytes = Data(A[1], Single);
        if (Bytes.size() > std::min<std::size_t>(65507, C.Owner->Limits.MaxTransferBytes))
            return Error("network UDP payload limit exceeded");
        auto Written = Net::SendTo(C.Handle, C.Version, Bytes, Text(A[2]), Port(A[3]));
        if (Written < 0) return Value{};
        if (static_cast<std::size_t>(Written) != Bytes.size()) return Error("network incomplete datagram send");
        return Value::Number(static_cast<double>(Written));
    });
    Add("ReceiveFrom", [&Sockets, Buffers, &Machine](const auto& A) {
        Arity(A, 1, 2); auto& C = Sockets.Get(A[0]); Udp(C);
        if (C.Receiving) return Error("network receive callback owns this socket");
        auto Limit = std::min<std::size_t>(65507, C.Owner->Limits.MaxTransferBytes);
        auto Count = A.size() == 2 ? Integer(A[1], Limit, "datagram receive count") : Limit;
        // One extra byte distinguishes a valid zero-length packet from overflow,
        // and detects packets above the caller's bound without exposing a prefix.
        std::string Bytes(Count + 1, '\0');
        auto Packet = Net::ReceiveFrom(C.Handle, Bytes.data(), Bytes.size());
        if (Packet.Count < 0) return Value{};
        if (Packet.Truncated || static_cast<std::size_t>(Packet.Count) > Count)
            return Error("network datagram exceeds receive limit; datagram discarded");
        Bytes.resize(static_cast<std::size_t>(Packet.Count));
        return Record(Machine, {{"Data", Buffers.Create(std::move(Bytes))},
            {"Address", Value::String(Packet.From.Host)}, {"Port", Value::Number(Packet.From.Port)}});
    });
    Add("ShutdownWrite", [&Sockets](const auto& A) {
        Arity(A, 1, 1); auto& C = Sockets.Get(A[0]); TcpConnected(C);
        if (!C.WriteClosed) { Net::ShutdownWrite(C.Handle); C.WriteClosed = true; }
        return Value{};
    });
    Add("OnReceive", [&Sockets, Shared, Buffers, &Machine](const auto& A) {
        Arity(A, 2, 2); auto& Socket = Sockets.GetObject(A[0]);
        if (A[1].GetType() == ValueType::Null) { ClearCallback(Socket); return Value{}; }
        auto& C = Sockets.Get(A[0]);
        if (C.Tcp) TcpConnected(C);
        if ((!C.Tcp && C.Status != Phase::Bound) || C.ReadClosed)
            return Error("network receiver requires bound UDP or readable TCP");
        if (!Machine.IsCallable(A[1])) return Error("network receiver must be callable");
        if (!Shared->Service) {
            Shared->Service = Machine.RegisterIdleHook(
                [Weak = std::weak_ptr<State>(Shared), &Machine, Buffers](double) {
                    auto Owner = Weak.lock();
                    return Owner ? DispatchOne(Machine, Owner, Buffers) : Value::Bool(false);
                });
        }
        if (!C.Receiving) {
            // Drop obsolete entries before re-registering a live socket.
            std::erase_if(Shared->Readers, [&Socket](const auto& Entry) {
                auto Existing = Entry.lock();
                return !Existing || Existing.get() == &Socket;
            });
            Shared->Readers.push_back(std::static_pointer_cast<SocketObject>(A[0].AsNativeObject()));
        }
        Socket.SetGcVisibleMember("receiveCallback", A[1]);
        Socket.Callback = A[1]; C.Receiving = true; ++Socket.Generation;
        return Value{};
    });
    Add("Close", [&Sockets](const auto& A) {
        Arity(A, 1, 1); auto& Socket = Sockets.GetObject(A[0]);
        ClearCallback(Socket); Socket.Body->Close(); return Value{};
    });
    Add("LocalAddress", [&Sockets, &Machine](const auto& A) {
        Arity(A, 1, 1); return Address(Machine, Net::LocalAddress(Sockets.Get(A[0]).Handle));
    });
    Add("PeerAddress", [&Sockets, &Machine](const auto& A) {
        Arity(A, 1, 1); auto& C = Sockets.Get(A[0]); TcpConnected(C);
        return Address(Machine, Net::PeerAddress(C.Handle));
    });
    Add("Resolve", [Shared](const auto& A) {
        Arity(A, 1, 2);
        return Value::String(Net::Resolve(Text(A[0]), Family(A, 1)));
    });
    Add("Buffer", [Shared, Buffers](const auto& A) {
        std::string Bytes;
        for (const auto& Item : A) {
            char Single = 0; auto Part = Data(Item, Single);
            if (Part.size() > Shared->Limits.MaxTransferBytes - Bytes.size())
                return Error("network buffer limit exceeded");
            Bytes.append(Part);
        }
        return Buffers.Create(std::move(Bytes));
    });
    ModuleObject = std::move(Module);
}
NetworkLibrary::~NetworkLibrary() = default;
Value NetworkLibrary::GetModule() const { return Value::FromObject(ModuleObject); }
} // namespace Feather
