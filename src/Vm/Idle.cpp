#include <Feather/Runtime.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace Feather {
IdleCallbackFault::IdleCallbackFault(std::exception_ptr Input) : Cause(std::move(Input)) {
    try { std::rethrow_exception(Cause); }
    catch (const std::exception& Failure) { Message = Failure.what(); }
    catch (...) { Message = "idle callback failed"; }
}
std::shared_ptr<IdleRegistration> Vm::RegisterIdleHook(std::function<Value(double)> Hook) {
    if (!Hook) throw std::invalid_argument("empty idle hook");
    auto Vacant = std::find_if(IdleHooks.begin(), IdleHooks.end(),
        [](const auto& Entry) { return Entry.expired(); });
    if (Vacant == IdleHooks.end() && IdleHooks.size() >= 1024)
        throw std::length_error("idle hook registration limit exceeded");
    auto Registration = std::shared_ptr<IdleRegistration>(new IdleRegistration(std::move(Hook)));
    if (Vacant != IdleHooks.end()) *Vacant = Registration;
    else IdleHooks.push_back(Registration);
    return Registration;
}
Value Vm::Idle(double Seconds, std::function<double()> Monotonic, std::function<void(double)> Wait) {
    if (!std::isfinite(Seconds) || Seconds < 0) throw std::invalid_argument("invalid idle duration");
    if (!Monotonic) Monotonic = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    if (!Wait) Wait = [](double Duration) { std::this_thread::sleep_for(std::chrono::duration<double>(Duration)); };
    auto Start = Monotonic();
    auto Deadline = Start + Seconds;
    if (!std::isfinite(Start) || !std::isfinite(Deadline)) throw std::invalid_argument("invalid idle deadline");
    auto FinishWaiting = [&] {
        for (;;) {
            auto Remaining = Deadline - Monotonic();
            if (Remaining <= 0) break;
            Wait(std::min(Remaining, 86400.0));
        }
    };
    if (IdleDispatching) { FinishWaiting(); return {}; }
    IdleDispatching = true;
    struct Reset { bool& Flag; ~Reset() { Flag = false; } } Guard{IdleDispatching};
    // Rotate before pruning so registration churn does not reset fairness to the first hook.
    if (!IdleHooks.empty()) {
        NextIdleHook %= IdleHooks.size();
        std::rotate(IdleHooks.begin(), IdleHooks.begin() + NextIdleHook, IdleHooks.end());
        std::erase_if(IdleHooks, [](const auto& Entry) { return Entry.expired(); });
    }
    NextIdleHook = 0;
    std::size_t Calls = 0;
    while (!IdleHooks.empty()) {
        auto Count = IdleHooks.size(); // Registrations during a callback join the next round.
        bool Worked = false;
        for (std::size_t I = 0; I < Count; ++I) {
            double Remaining = std::max(0.0, Deadline - Monotonic());
            if (Calls && (Remaining == 0 || Seconds == 0)) return {};
            if (Calls == 256) { FinishWaiting(); return {}; }
            NextIdleHook %= IdleHooks.size();
            auto Hook = IdleHooks[NextIdleHook++].lock();
            if (!Hook) continue;
            ++Calls;
            Value Result;
            try { Result = Hook->Hook(Remaining); }
            catch (const IdleCallbackFault&) { throw; }
            catch (...) { throw IdleCallbackFault(std::current_exception()); }
            if (Result.IsError()) return Result;
            if (Result.GetType() != ValueType::Bool)
                throw IdleCallbackFault(std::make_exception_ptr(
                    std::logic_error("idle hook must return bool or Error")));
            Worked = Worked || Result.AsBool();
        }
        if (Seconds == 0 || Monotonic() >= Deadline) return {};
        if (!Worked) Wait(std::min(0.005, std::max(0.0, Deadline - Monotonic())));
    }
    FinishWaiting();
    return {};
}
} // namespace Feather
