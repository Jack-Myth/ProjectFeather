#pragma once

#include <Feather/Runtime.hpp>

namespace Feather {

class FunctionObject final : public NativeObject {
public:
    FunctionObject(std::shared_ptr<Module> Owner, std::shared_ptr<FunctionPrototype> Body)
        : Owner(std::move(Owner)), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Function; }
    bool IsCallable() const override { return true; }
    const FunctionPrototype& GetBody() const { return *Body; }
    const std::shared_ptr<Module>& GetOwner() const { return Owner; }
private:
    std::shared_ptr<Module> Owner;
    std::shared_ptr<FunctionPrototype> Body;
};

struct Frame {
    std::shared_ptr<FunctionObject> Function;
    std::size_t Pc = 0;
    std::vector<Value> Locals;
    std::vector<Value> Stack;
};

struct ExecutionState {
    std::vector<Frame> Frames;
    bool PendingCallResult = false;
};

} // namespace Feather
