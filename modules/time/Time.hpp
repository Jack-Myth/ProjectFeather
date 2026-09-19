#pragma once

#include <Feather/Runtime.hpp>

#include <memory>

namespace Feather {

inline constexpr NativeModuleGuid TimeModuleGuid{{
    0x71, 0x9d, 0x3a, 0xe4, 0x68, 0xb2, 0x44, 0x19,
    0xa6, 0x5f, 0x8c, 0x20, 0xd7, 0x33, 0x91, 0x4b}};

class TimeHost {
public:
    virtual ~TimeHost() = default;
    virtual double Monotonic() = 0;
    virtual double UnixTime() = 0;
    virtual void Sleep(double Seconds) = 0;
    virtual void SleepUntil(double Deadline) = 0;
};

std::shared_ptr<TimeHost> CreateSystemTimeHost();

class TimeLibrary final {
public:
    explicit TimeLibrary(Vm& Machine);
    TimeLibrary(Vm& Machine, std::shared_ptr<TimeHost> Host);
    ~TimeLibrary();
    TimeLibrary(const TimeLibrary&) = delete;
    TimeLibrary& operator=(const TimeLibrary&) = delete;

    Value GetModule() const;

private:
    std::shared_ptr<TimeHost> Host;
    std::shared_ptr<NativeObject> ModuleObject;
};

} // namespace Feather
