#pragma once

#include <Feather/Runtime.hpp>

#include <unordered_set>

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

struct ModuleInstance {
    enum class State { Loaded, Initializing, Initialized, Failed };
    std::string Id;
    std::shared_ptr<Module> Source;
    std::vector<std::uint8_t> Identity;
    std::shared_ptr<Module> Program;
    std::vector<std::shared_ptr<Object>> Functions;
    std::unordered_map<std::string, Value> Globals;
    std::unordered_set<std::string> Exports;
    std::shared_ptr<NativeObject> Namespace;
    State Initialization = State::Loaded;
    Value InitializationError;
};

class ModuleNamespace final : public NativeObject {
public:
    explicit ModuleNamespace(std::weak_ptr<ModuleInstance> Owner) : Owner(std::move(Owner)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override;
    Value SetMember(const Value& Key, const Value& Input) override;
    std::string GetModuleId() const;
private:
    std::weak_ptr<ModuleInstance> Owner;
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
