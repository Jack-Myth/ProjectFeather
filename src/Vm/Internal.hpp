#pragma once

#include <Feather/Runtime.hpp>

#include <unordered_set>

namespace Feather {

class FunctionObject : public NativeObject {
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
    std::shared_ptr<const std::vector<std::uint8_t>> Identity;
    std::shared_ptr<Module> Program;
    std::vector<Value> ConstantValues;
    std::unordered_map<std::string, Value> Globals;
    std::unordered_map<std::string, Value>* GlobalScope = nullptr;
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
    ModuleInstance* Instance = nullptr;
    std::size_t Pc = 0;
    std::vector<Value> Arguments;
    std::vector<Value> Locals;
    std::vector<Value> Stack;
};

// Only the active prefix is visible to the debugger and GC. Returned frames
// keep bounded buffer capacity at their depth for the next script call.
class FrameStack {
public:
    FrameStack() = default;
    FrameStack(const FrameStack&) = delete;
    FrameStack& operator=(const FrameStack&) = delete;

    std::size_t size() const { return Depth; }
    bool empty() const { return Depth == 0; }
    Frame& back() { return *Top; }
    const Frame& back() const { return *Top; }
    Frame& operator[](std::size_t Index) { return Slots[Index]; }
    const Frame& operator[](std::size_t Index) const { return Slots[Index]; }
    auto begin() const { return Slots.begin(); }
    auto end() const { return Slots.begin() + Depth; }

    Frame& Enter() {
        if (Depth == Slots.size()) Slots.emplace_back();
        // Growth can relocate every frame, including the former top.
        Top = &Slots[Depth++];
        return *Top;
    }

    void Leave() {
        constexpr std::size_t RetainedDepths = 16;
        constexpr std::size_t MaximumRetainedSlots = 4096;
        if (Depth <= RetainedDepths && Top->Stack.capacity() + Top->Locals.capacity() +
                Top->Arguments.capacity() <= MaximumRetainedSlots) {
            Top->Stack.clear();
            Top->Locals.clear();
            Top->Arguments.clear();
            Top->Function.reset();
        } else {
            *Top = {};
        }
        --Depth;
        Top = Depth ? Top - 1 : nullptr;
    }
private:
    std::vector<Frame> Slots;
    Frame* Top = nullptr;
    std::size_t Depth = 0;
};

struct ExecutionState {
    FrameStack Frames;
};

} // namespace Feather
