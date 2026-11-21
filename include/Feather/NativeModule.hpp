#pragma once

#include <Feather/Runtime.hpp>

#include <iosfwd>

namespace Feather {

class FEATHER_API NativeModuleBuilder final {
public:
    using Function = std::function<Value(const std::vector<Value>&)>;
    NativeModuleBuilder(Vm& Machine, NativeModuleGuid Id, std::string Name);
    void AddFunction(std::string Name, Function Body);
    void AddValue(std::string Name, Value Member);
    Value Build() const;
private:
    class Namespace;
    Vm& Machine;
    NativeModuleGuid Id;
    std::shared_ptr<Namespace> Module;
};

class NativeModuleHost {
public:
    using WorkerMain = std::function<void(NativeModuleHost&)>;

    virtual ~NativeModuleHost() = default;
    // On the calling OS thread, create an isolated VM and invoke Main.
    // The child can retrieve the caller's module state for the duration of Main.
    virtual void RunWorker(std::vector<std::string> Paths, NativeModuleGuid Owner,
                           void* State, const WorkerMain& Main) const = 0;
    virtual void* GetWorkerState(NativeModuleGuid Owner) const noexcept = 0;
    virtual Value DispatchScript(const ScriptFunctionAddress& Address,
                                 std::span<const std::uint8_t> Arguments) = 0;
    // Stop must not throw. The host calls it before destroying the VM, including on failure.
    virtual void RegisterShutdown(std::function<void()> Finish, std::function<void()> Stop) = 0;
};

struct NativeModuleContext {
    Vm& Machine;
    std::istream* Input = nullptr;
    std::ostream* Output = nullptr;
    NativeModuleHost* Host = nullptr;
};

struct NativeModuleDescriptor {
    NativeModuleGuid Id;
    const char* Name;
    Value (*Create)(const NativeModuleContext&);
};

inline constexpr const char* NativeModuleEntryName = "FeatherNativeModule";
using NativeModuleEntry = const NativeModuleDescriptor* (*)();

} // namespace Feather

// A native module defines this function with FEATHER_NATIVE_EXPORT. Its C linkage
// fixes the symbol name; the descriptor and Feather values still use the C++ ABI.
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule();
