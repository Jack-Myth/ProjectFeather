#pragma once

#include <Feather/Runtime.hpp>

#include <iosfwd>
#include <memory>

namespace Feather {

// Optional standard library. The VM core does not create or own these objects.
// The supplied streams must outlive the library and every VM using its objects.
class StdIoLibrary final {
public:
    StdIoLibrary(std::istream& Input, std::ostream& Output);
    ~StdIoLibrary();
    StdIoLibrary(const StdIoLibrary&) = delete;
    StdIoLibrary& operator=(const StdIoLibrary&) = delete;

    // A read-only module with Console and IO members.
    Value GetModule() const;

private:
    struct State;
    std::shared_ptr<State> Shared;
    std::shared_ptr<NativeObject> ConsoleObject;
    std::shared_ptr<NativeObject> IOObject;
    std::shared_ptr<NativeObject> ModuleObject;
};

} // namespace Feather
