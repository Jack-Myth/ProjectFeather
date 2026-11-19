#include <Feather/NativeModule.hpp>

#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef GetMessage
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Feather {
namespace {

constexpr NativeModuleGuid ThreadsGuid{{
    0x82, 0x34, 0x68, 0x3d, 0x70, 0x2e, 0x4d, 0xf5,
    0xa6, 0x87, 0xc4, 0xe8, 0x54, 0x28, 0x9f, 0xb1}};


void Check(const Value& Result) {
    if (!Result.IsError()) return;
    const auto* Failure = dynamic_cast<const ErrorObject*>(Result.AsObject());
    throw std::runtime_error(Failure ? Failure->GetMessage() : "unknown language error");
}

using ThreadIdentifier = std::uint64_t;
// Feather numbers must represent IDs exactly, including after serialization.
constexpr ThreadIdentifier MaxThreadId = (ThreadIdentifier{1} << 53) - 1;

ThreadIdentifier NativeThreadId() {
    ThreadIdentifier Id = 0;
#ifdef _WIN32
    Id = GetCurrentThreadId();
#elif defined(__APPLE__)
    if (pthread_threadid_np(nullptr, &Id) != 0) Id = 0;
#elif defined(__linux__)
    const auto Tid = syscall(SYS_gettid);
    if (Tid > 0) Id = static_cast<ThreadIdentifier>(Tid);
#endif
    if (Id == 0 || Id > MaxThreadId)
        throw std::runtime_error("cannot represent the native thread ID as a Feather number");
    return Id;
}

ThreadIdentifier ThreadId(const Value& Input) {
    if (!Input.IsNumeric())
        throw std::invalid_argument("expected a thread ID");
    auto Number = Input.AsNumber();
    if (!std::isfinite(Number) || Number <= 0 ||
        Number > static_cast<double>(MaxThreadId) || std::trunc(Number) != Number)
        throw std::invalid_argument("expected a thread ID");
    return static_cast<ThreadIdentifier>(Number);
}

struct Message {
    ScriptFunctionAddress Function;
    std::vector<std::uint8_t> Arguments;
};

struct Endpoint {
    std::deque<Message> Queue;
    std::thread Worker;
    std::exception_ptr Failure;
    bool Registered = false;
    bool Closed = false;
    bool Retiring = false;
};

enum class Operation { Start, Post, Current, GetMainThread, Pump, Join };

struct ThreadGroup {
    explicit ThreadGroup(NativeModuleHost& Host) : Host(Host), MainThreadId(NativeThreadId()) {
        Endpoints.emplace(MainThreadId, std::make_unique<Endpoint>());
    }
    ~ThreadGroup() { Stop(); }

    class Method final : public NativeObject {
    public:
        Method(ThreadGroup& Group, Vm& Machine, Operation Action, ThreadIdentifier CurrentId)
            : Group(Group), Machine(Machine), Action(Action), CurrentId(CurrentId) {}
        ObjectType GetObjectType() const override { return ObjectType::Host; }
        bool IsCallable() const override { return true; }
        Value Call(const std::vector<Value>& Args) override {
            try {
                switch (Action) {
                case Operation::Start:
                    if (Args.size() != 2) return Value::Error("threads.Start expects a function and context");
                    return Value::Number(static_cast<double>(Group.Start(
                        Pack(Args[0], std::span(Args).subspan(1)), Machine.GetModulePaths())));
                case Operation::Post: {
                    if (Args.size() < 2 || Args.size() > 3)
                        return Value::Error("threads.Post expects a target thread ID, function and optional arguments");
                    auto Arguments = std::span(Args).subspan(2);
                    if (!Arguments.empty() && Arguments.front().IsScriptObject() &&
                        Arguments.front().AsScriptObject()->IsArray())
                        Arguments = Arguments.front().AsScriptObject()->GetArrayElements();
                    Group.Post(ThreadId(Args[0]), Pack(Args[1], Arguments));
                    return Value::Bool(true);
                }
                case Operation::Current:
                    if (!Args.empty()) return Value::Error("threads.Current expects no arguments");
                    return Value::Number(static_cast<double>(CurrentId));
                case Operation::GetMainThread:
                    if (!Args.empty()) return Value::Error("threads.GetMainThread expects no arguments");
                    return Value::Number(static_cast<double>(Group.MainThreadId));
                case Operation::Pump: {
                    if (CurrentId != Group.MainThreadId) return Value::Error("threads.Pump requires the entry thread");
                    if (Args.size() != 1 || !Args[0].IsNumeric()) return Value::Error("threads.Pump expects a message limit");
                    double Limit = Args[0].AsNumber();
                    if (!std::isfinite(Limit) || Limit < 0 || Limit > static_cast<double>(MaxThreadId) ||
                        std::trunc(Limit) != Limit)
                        return Value::Error("threads.Pump expects a nonnegative integer limit");
                    return Value::Number(static_cast<double>(Group.Pump(static_cast<std::uint64_t>(Limit))));
                }
                case Operation::Join:
                    if (Args.size() != 1) return Value::Error("threads.Join expects a worker thread ID");
                    if (CurrentId != Group.MainThreadId)
                        return Value::Error("threads.Join is available on the CLI entry thread");
                    Group.Join(ThreadId(Args[0]));
                    return Value::Bool(true);
                }
            } catch (const std::exception& Failure) {
                return Value::Error(Failure.what());
            }
            return Value::Error("unknown threads operation");
        }
    private:
        Message Pack(const Value& Function, std::span<const Value> Arguments) {
            return {Machine.GetFunctionAddress(Function), Machine.SerializeArguments(Arguments)};
        }
        ThreadGroup& Group;
        Vm& Machine;
        Operation Action;
        ThreadIdentifier CurrentId;
    };

    class Library final : public NativeObject {
    public:
        Library(ThreadGroup& Group, Vm& Machine, ThreadIdentifier CurrentId)
            : Group(Group), Machine(Machine), CurrentId(CurrentId) {}
        ObjectType GetObjectType() const override { return ObjectType::Host; }
        Value GetMember(const Value& Key) override {
            if (Key.GetType() == ValueType::String) {
                const auto& Name = Key.AsString();
                if (Name == "Start") return MakeMethod(Operation::Start);
                if (Name == "Post") return MakeMethod(Operation::Post);
                if (Name == "Current") return MakeMethod(Operation::Current);
                if (Name == "GetMainThread") return MakeMethod(Operation::GetMainThread);
                if (Name == "Pump") return MakeMethod(Operation::Pump);
                if (Name == "Join") return MakeMethod(Operation::Join);
            }
            return Value::Error("unknown threads member");
        }
    private:
        Value MakeMethod(Operation Action) {
            return Value::FromObject(std::make_shared<Method>(Group, Machine, Action, CurrentId));
        }
        ThreadGroup& Group;
        Vm& Machine;
        ThreadIdentifier CurrentId;
    };

    ThreadIdentifier Start(Message Input, std::vector<std::string> ModulePaths) {
        std::unique_lock Lock(Mutex);
        if (Stopped) throw std::runtime_error("thread group is closing");
        auto Thread = std::make_unique<Endpoint>();
        Thread->Queue.push_back(std::move(Input));
        auto* Target = Thread.get();
        std::promise<ThreadIdentifier> Ready;
        auto WorkerId = Ready.get_future();
        Target->Worker = std::thread(
            [this, Target, Ready = std::move(Ready), Paths = std::move(ModulePaths)]() mutable {
                ThreadIdentifier Id;
                try { Id = NativeThreadId(); }
                catch (...) { Ready.set_exception(std::current_exception()); return; }
                Ready.set_value(Id);
                {
                    // Start holds Mutex until it registers this ID and counts the initial message.
                    std::lock_guard Publication(Mutex);
                    if (!Target->Registered) return;
                }
                RunWorker(*Target, std::move(Paths));
                // Keep an initialization failure associated with its live OS ID until Join or Stop.
                std::unique_lock Retirement(Mutex);
                Changed.wait(Retirement, [&] { return Target->Retiring; });
            });
        try {
            auto Id = WorkerId.get();
            auto [Slot, Inserted] = Endpoints.try_emplace(Id);
            if (!Inserted) throw std::runtime_error("native thread ID is already registered");
            Slot->second = std::move(Thread);
            Target->Registered = true;
            ++Pending;
            return Id;
        } catch (...) {
            Lock.unlock();
            Target->Worker.join();
            throw;
        }
    }

    void Post(ThreadIdentifier Id, Message Input) {
        {
            std::lock_guard Lock(Mutex);
            auto Found = Endpoints.find(Id);
            if (Stopped || Found == Endpoints.end() || Found->second->Closed)
                throw std::runtime_error("thread is unavailable: " + std::to_string(Id));
            Found->second->Queue.push_back(std::move(Input));
            ++Pending;
        }
        Changed.notify_all();
    }

    void Join(ThreadIdentifier Id) {
        std::unique_ptr<Endpoint> Target;
        {
            std::lock_guard Lock(Mutex);
            auto Found = Endpoints.find(Id);
            if (Id == MainThreadId || Found == Endpoints.end())
                throw std::invalid_argument("unknown worker ID");
            Target = std::move(Found->second);
            Endpoints.erase(Found);
            Target->Closed = true;
            Target->Retiring = true;
        }
        Changed.notify_all();
        if (Target->Worker.joinable()) Target->Worker.join();
        if (Target->Failure) std::rethrow_exception(Target->Failure);
    }

    void Complete(Endpoint& Target, std::exception_ptr Failure, bool Abort = false) {
        {
            std::lock_guard Lock(Mutex);
            if (Failure && !Target.Failure) Target.Failure = std::move(Failure);
            --Pending;
            if (Abort) Close(Target);
        }
        Changed.notify_all();
    }

    void Close(Endpoint& Target) {
        Target.Closed = true;
        Pending -= Target.Queue.size();
        Target.Queue.clear();
    }

    void RunWorker(Endpoint& Target, std::vector<std::string> ModulePaths) noexcept {
        try {
            Host.RunWorker(std::move(ModulePaths), ThreadsGuid, this,
                [&](NativeModuleHost& WorkerHost) {
                    bool First = true;
                    for (;;) {
                        Message Input;
                        {
                            std::unique_lock Lock(Mutex);
                            Changed.wait(Lock, [&] { return Target.Closed || !Target.Queue.empty(); });
                            if (Target.Queue.empty()) break;
                            Input = std::move(Target.Queue.front());
                            Target.Queue.pop_front();
                        }
                        std::exception_ptr Failure;
                        try { Check(WorkerHost.DispatchScript(Input.Function, Input.Arguments)); }
                        catch (...) { Failure = std::current_exception(); }
                        Complete(Target, Failure, First && static_cast<bool>(Failure));
                        First = false;
                    }
                });
        } catch (...) {
            {
                std::lock_guard Lock(Mutex);
                if (!Target.Failure) Target.Failure = std::current_exception();
                Close(Target);
            }
            Changed.notify_all();
        }
    }

    void DispatchRoot(Endpoint& Root, Message Input, bool DeferFailure) {
        DispatchingRoot = true;
        std::exception_ptr Failure;
        try { Check(Host.DispatchScript(Input.Function, Input.Arguments)); }
        catch (...) { Failure = std::current_exception(); }
        DispatchingRoot = false;
        Complete(Root, DeferFailure ? Failure : nullptr);
        if (Failure && !DeferFailure) std::rethrow_exception(Failure);
    }

    std::uint64_t Pump(std::uint64_t Limit) {
        if (DispatchingRoot) throw std::logic_error("threads.Pump cannot dispatch recursively");
        std::uint64_t Count = 0;
        while (Count < Limit) {
            Endpoint* Root;
            Message Input;
            {
                std::lock_guard Lock(Mutex);
                Root = Endpoints.at(MainThreadId).get();
                if (Root->Queue.empty()) break;
                Input = std::move(Root->Queue.front());
                Root->Queue.pop_front();
            }
            DispatchRoot(*Root, std::move(Input), false);
            ++Count;
        }
        return Count;
    }

    void Finish() {
        Endpoint* Root;
        {
            std::lock_guard Lock(Mutex);
            Root = Endpoints.at(MainThreadId).get();
        }
        for (;;) {
            Message Input;
            {
                std::unique_lock Lock(Mutex);
                Changed.wait(Lock, [&] { return Pending == 0 || !Root->Queue.empty(); });
                if (Pending == 0) break;
                Input = std::move(Root->Queue.front());
                Root->Queue.pop_front();
            }
            DispatchRoot(*Root, std::move(Input), true);
        }
        Stop();
        if (Root->Failure) std::rethrow_exception(Root->Failure);
        for (const auto& [Id, Thread] : Endpoints) {
            if (!Thread->Failure) continue;
            try { std::rethrow_exception(Thread->Failure); }
            catch (const std::exception& Failure) {
                throw std::runtime_error("worker " + std::to_string(Id) + ": " + Failure.what());
            }
        }
    }

    void Stop() noexcept {
        {
            std::lock_guard Lock(Mutex);
            if (Stopped) return;
            Stopped = true;
            for (auto& [_, Thread] : Endpoints) {
                Close(*Thread);
                Thread->Retiring = true;
            }
        }
        Changed.notify_all();
        for (auto& [_, Thread] : Endpoints)
            if (Thread->Worker.joinable()) Thread->Worker.join();
    }

    NativeModuleHost& Host;
    const ThreadIdentifier MainThreadId;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::unordered_map<ThreadIdentifier, std::unique_ptr<Endpoint>> Endpoints;
    std::size_t Pending = 0;
    bool Stopped = false;
    bool DispatchingRoot = false; // Accessed only on the entry thread.
};

Value CreateThreads(const NativeModuleContext& Context) {
    if (!Context.Host) return Value::Error("threads requires native host worker support");
    if (auto* Group = static_cast<ThreadGroup*>(Context.Host->GetWorkerState(ThreadsGuid))) {
        return Value::FromObject(
            std::make_shared<ThreadGroup::Library>(*Group, Context.Machine, NativeThreadId()));
    }
    auto Group = std::make_shared<ThreadGroup>(*Context.Host);
    auto Library = Value::FromObject(
        std::make_shared<ThreadGroup::Library>(*Group, Context.Machine, Group->MainThreadId));
    Context.Host->RegisterShutdown([Group] { Group->Finish(); }, [Group] { Group->Stop(); });
    return Library;
}

constexpr NativeModuleDescriptor Descriptor{ThreadsGuid, "threads", CreateThreads};

} // namespace
} // namespace Feather

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    return &Feather::Descriptor;
}
