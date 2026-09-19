#pragma once

#include <Feather/Runtime.hpp>

#include <iosfwd>
#include <memory>

namespace Feather {

inline constexpr NativeModuleGuid StdIoModuleGuid{{
    0x2d, 0x33, 0xd7, 0x69, 0x4a, 0x83, 0x46, 0xc0,
    0xb5, 0x87, 0x65, 0x72, 0x47, 0x4f, 0x1f, 0xd2}};

// Optional standard library. The VM core does not create or own these objects.
// The supplied streams must outlive the library and every VM using its objects.
class StdIoLibrary final {
public:
    StdIoLibrary(std::istream& Input, std::ostream& Output);
    StdIoLibrary(Vm& Machine, std::istream& Input, std::ostream& Output);
    ~StdIoLibrary();
    StdIoLibrary(const StdIoLibrary&) = delete;
    StdIoLibrary& operator=(const StdIoLibrary&) = delete;

    // A read-only module with Console and IO members.
    Value GetModule() const;

private:
    StdIoLibrary(Vm* Machine, std::istream& Input, std::ostream& Output);
    struct State;
    std::shared_ptr<State> Shared;
    std::shared_ptr<NativeObject> ConsoleObject;
    std::shared_ptr<NativeObject> IOObject;
    std::shared_ptr<NativeObject> ModuleObject;
};

} // namespace Feather
