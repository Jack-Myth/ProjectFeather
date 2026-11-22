#include <Feather/NativeModule.hpp>
#include <ffi.h>
#include <Feather/ByteBuffer.hpp>
#include <array>
#include <atomic>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

namespace {
using namespace Feather;
constexpr NativeModuleGuid ModuleId{{0xb1,0x02,0x76,0x90,0x49,0x22,0x42,0xcd,
                              0xbd,0x43,0x93,0x47,0x74,0x9d,0xf1,0x60}};

void Require(bool Condition, const char* Message) {
    if (!Condition) throw std::invalid_argument(Message);
}

class Host : public NativeObject {
public:
    explicit Host(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
};

class Factory final : public NativeObjectType {
public:
    Factory(Vm& Machine) : NativeObjectType(Machine, ModuleId, "ForeignObject") {}
    template<class T, class... Args> Value Make(Args&&... Input) {
        return Value::FromObject(CreateObject<T>(std::forward<Args>(Input)...));
    }
};

template<class T> T* As(const Value& V) {
    return V.GetType() == ValueType::Object ? dynamic_cast<T*>(V.AsObject()) : nullptr;
}

enum class Kind { Void, I8, U8, I16, U16, I32, U32, F32, F64, Word, Ptr, Utf8, Utf16, Buffer };
struct Spec {
    Kind Base;
    bool ByRef = false;
    size_t Size = 0;
    ffi_type* Abi() const {
        if (ByRef) return &ffi_type_pointer;
        switch (Base) {
        case Kind::Void: return &ffi_type_void;
        case Kind::I8: return &ffi_type_sint8;
        case Kind::U8: return &ffi_type_uint8;
        case Kind::I16: return &ffi_type_sint16;
        case Kind::U16: return &ffi_type_uint16;
        case Kind::I32: return &ffi_type_sint32;
        case Kind::U32: return &ffi_type_uint32;
        case Kind::F32: return &ffi_type_float;
        case Kind::F64: return &ffi_type_double;
        case Kind::Word: return &ffi_type_sint64;
        default: return &ffi_type_pointer;
        }
    }
};
struct Descriptor final : Host {
    Descriptor(NativeObjectType& Type, Spec S) : Host(Type), Signature(S) {}
    Spec Signature;
};
struct Strings final : Host {
    Strings(NativeObjectType& Type, Factory& F) : Host(Type),
        Utf8(F.Make<Descriptor>(Spec{Kind::Utf8})), Utf16(F.Make<Descriptor>(Spec{Kind::Utf16})) {}
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String) {
            if (Key.AsString() == "utf8") return Utf8;
            if (Key.AsString() == "utf16") return Utf16;
        }
        return Value::Error("unknown ffi.str member");
    }
    Value Utf8, Utf16;
};
struct Pointer : Host {
    Pointer(NativeObjectType& Type, void* Address, std::shared_ptr<void> Library)
        : Host(Type), Address(Address), Library(std::move(Library)) {}
    Value Owner;
    void* Address;
    std::shared_ptr<void> Library;
};
struct Ref final : Host {
    Ref(NativeObjectType& Type, Value Input) : Host(Type) { Assign(Input); }
    void Assign(const Value& V) {
        Require(V.IsNumeric() || V.GetType() == ValueType::Null || As<Pointer>(V),
                "ffi.Ref expects a number, pointer or null");
        Val = V;
    }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "Val") return Val;
        return NativeObject::GetMember(Key);
    }
    Value SetMember(const Value& Key, const Value& Input) override {
        if (Key.GetType() != ValueType::String || Key.AsString() != "Val")
            return NativeObject::SetMember(Key, Input);
        try { Assign(Input); return Val; }
        catch (const std::invalid_argument& E) { return Value::Error(E.what()); }
    }
    Value Val;
};

std::wstring Wide(const std::string& Text) {
    if (Text.empty()) return {};
    Require(Text.size() <= INT_MAX, "FFI string too long");
    int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), nullptr, 0);
    Require(Length > 0, "invalid UTF-8 string");
    std::wstring Result(Length, L'\0');
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()),
                                Result.data(), Length) == Length, "invalid UTF-8 string");
    return Result;
}
const std::string& TextOf(const Value& V) {
    Require(V.GetType() == ValueType::String, "FFI expected a string");
    const auto& S = V.AsString();
    Require(S.find('\0') == std::string::npos, "embedded NUL in FFI string");
    return S;
}
// The Windows x64 ABI uses little-endian, word-sized storage for narrow integer results.
struct alignas(8) Scalar {
    unsigned char Data[8]{};
    template<class T> void Put(T V) { std::memcpy(Data, &V, sizeof V); }
    template<class T> T Get() const { T V; std::memcpy(&V, Data, sizeof V); return V; }
};
template<class T> T Integer(double N) {
    Require(std::trunc(N) == N && N >= std::numeric_limits<T>::lowest() &&
            N <= std::numeric_limits<T>::max(), "FFI integer outside declared range");
    return static_cast<T>(N);
}
void* PointerAddress(const Value& V) {
    auto* P = As<Pointer>(V);
    Require(P || V.GetType() == ValueType::Null, "FFI expected a pointer or null");
    return P ? P->Address : nullptr;
}
void WriteScalar(Scalar& S, Kind K, const Value& V) {
    if (K == Kind::Ptr) {
        S.Put(PointerAddress(V));
        return;
    }
    Require(V.IsNumeric(), "FFI expected a number");
    double N = V.AsNumber();
    switch (K) {
    case Kind::I8: S.Put(Integer<int8_t>(N)); break;
    case Kind::U8: S.Put(Integer<uint8_t>(N)); break;
    case Kind::I16: S.Put(Integer<int16_t>(N)); break;
    case Kind::U16: S.Put(Integer<uint16_t>(N)); break;
    case Kind::I32: S.Put(Integer<int32_t>(N)); break;
    case Kind::U32: S.Put(Integer<uint32_t>(N)); break;
    case Kind::Word:
        Require(std::trunc(N) == N && std::abs(N) <= 9007199254740991.0, "IntPtr outside safe integer range");
        S.Put(static_cast<intptr_t>(N)); break;
    case Kind::F32:
        Require(!std::isfinite(N) || std::abs(N) <= std::numeric_limits<float>::max(), "Float32 overflow");
        S.Put(static_cast<float>(N)); break;
    case Kind::F64: S.Put(N); break;
    default: throw std::logic_error("invalid FFI scalar");
    }
}
Value ReadScalar(const Scalar& S, Kind K, Factory& F, const std::shared_ptr<void>& Library) {
    switch (K) {
    case Kind::Void: return {};
    case Kind::I8: return Value::Number(S.Get<int8_t>());
    case Kind::U8: return Value::Byte(S.Get<uint8_t>());
    case Kind::I16: return Value::Number(S.Get<int16_t>());
    case Kind::U16: return Value::Number(S.Get<uint16_t>());
    case Kind::I32: return Value::Number(S.Get<int32_t>());
    case Kind::U32: return Value::Number(S.Get<uint32_t>());
    case Kind::F32: return Value::Number(S.Get<float>());
    case Kind::Word: {
        auto N = S.Get<intptr_t>();
        Require(N >= -9007199254740991LL && N <= 9007199254740991LL, "IntPtr outside safe integer range");
        return Value::Number(static_cast<double>(N));
    }
    case Kind::F64: return Value::Number(S.Get<double>());
    case Kind::Ptr: {
        auto P = S.Get<void*>();
        return P ? F.Make<Pointer>(P, Library) : Value{};
    }
    default: throw std::logic_error("invalid FFI return type");
    }
}

class Memory final : public ByteBufferView {
public:
    Memory(NativeObjectType& T, size_t Size) : ByteBufferView(T), Bytes(Size, '\0') {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    const std::string& Data() const override { return Bytes; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "Length")
            return Value::Number(static_cast<double>(Bytes.size()));
        return NativeObject::GetMember(Key);
    }
    std::string Bytes;
    std::unordered_map<size_t, Value> Fields;
    // Native objects are reference counted. Reject ownership cycles before storing a pointer.
    bool CanHold(const Pointer& P) const {
        std::vector<const Memory*> Pending;
        std::unordered_set<const Memory*> Seen;
        if (auto* M = As<Memory>(P.Owner)) Pending.push_back(M);
        while (!Pending.empty()) {
            auto* M = Pending.back();
            Pending.pop_back();
            if (M == this) return false;
            if (!Seen.insert(M).second) continue;
            for (const auto& [_, V] : M->Fields)
                if (auto* Next = As<Memory>(As<Pointer>(V)->Owner)) Pending.push_back(Next);
        }
        return true;
    }
};

Value Field(Factory& F, Kind K, bool Write, const std::vector<Value>& A) {
    Require(A.size() == (Write ? 3 : 2) && A[1].IsNumeric(), "invalid Struct arguments");
    const auto* Bytes = ByteBufferData(A[0]);
    auto* M = As<Memory>(A[0]);
    Require(Bytes && (!Write || M), "Struct write needs ffi.Buffer; read needs a ByteBuffer");
    const auto Offset = Integer<uint32_t>(A[1].AsNumber());
    size_t Width = Spec{K}.Abi()->size;
    Require(Offset <= Bytes->size() && Width <= Bytes->size()-Offset, "Struct field out of bounds");
    Scalar S;
    if (!Write) {
        std::memcpy(S.Data, Bytes->data()+Offset, Width);
        if (M && K == Kind::Ptr) {
            auto It = M->Fields.find(Offset);
            if (It != M->Fields.end() && As<Pointer>(It->second)->Address == S.Get<void*>())
                return It->second;
        }
        return ReadScalar(S, K, F, {});
    }
    WriteScalar(S, K, A[2]);
    if (K == Kind::Ptr && As<Pointer>(A[2])) {
        Require(M->CanHold(*As<Pointer>(A[2])), "cyclic buffer pointer ownership");
        M->Fields.insert_or_assign(Offset, A[2]);
    }
    std::erase_if(M->Fields, [&](const auto& Entry) {
        if (Entry.first == Offset && K == Kind::Ptr && As<Pointer>(A[2])) return false;
        return Entry.first < size_t(Offset)+Width && Offset < Entry.first+sizeof(void*);
    });
    std::memcpy(M->Bytes.data()+Offset, S.Data, Width);
    return {};
}

// Libffi invokes this entry synchronously. Exceptions must never cross the native ABI.
class Callback final : public Pointer {
public:
    Callback(NativeObjectType& T, Factory& F, const std::vector<Value>& Args)
        : Pointer(T, nullptr, {}), Objects(F), Thread(GetCurrentThreadId()) {
        Require(Args.size() >= 2 && Args[0].GetType() == ValueType::Object &&
                Args[0].AsObject()->HasFlag(Object::Flag::ScriptFunction), "Callback expects a Feather function");
        Target = Args[0];
        SetGcVisibleMember("target", Target);
        for (size_t I = 1; I < Args.size(); ++I) {
            auto* D = As<Descriptor>(Args[I]);
            Require(D && !D->Signature.ByRef && D->Signature.Base != Kind::Utf8 &&
                    D->Signature.Base != Kind::Utf16 && D->Signature.Base != Kind::Buffer,
                    "callback expects scalar or pointer descriptors");
            if (I == 1) Result = D->Signature;
            else {
                Require(D->Signature.Base != Kind::Void, "void callback argument");
                Params.push_back(D->Signature);
                Types.push_back(D->Signature.Abi());
            }
        }
        Require(ffi_prep_cif(&Cif, FFI_DEFAULT_ABI, static_cast<unsigned>(Types.size()),
                             Result.Abi(), Types.data()) == FFI_OK, "invalid callback signature");
        Closure.reset(static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &Address)));
        Require(Closure && ffi_prep_closure_loc(Closure.get(), &Cif, Enter, this, Address) == FFI_OK,
                "cannot prepare native callback");
    }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String && Key.AsString() == "LastError") {
            if (WrongThread.exchange(false)) return Value::Error("callback called from another thread");
            return std::exchange(Failure, Value{});
        }
        return NativeObject::GetMember(Key);
    }
private:
    static void Enter(ffi_cif*, void* Out, void** Raw, void* Self) noexcept {
        auto& C = *static_cast<Callback*>(Self);
        Scalar Return;
        if (GetCurrentThreadId() != C.Thread) C.WrongThread = true;
        else try {
            std::vector<Value> Arguments;
            for (size_t I = 0; I < C.Params.size(); ++I) {
                Scalar S;
                std::memcpy(S.Data, Raw[I], C.Types[I]->size);
                Arguments.push_back(ReadScalar(S, C.Params[I].Base, C.Objects, {}));
            }
            auto V = C.Objects.GetVm().Call(C.Target, std::move(Arguments));
            if (V.IsError()) C.Failure = V;
            else if (C.Result.Base != Kind::Void) WriteScalar(Return, C.Result.Base, V);
        } catch (const std::exception& E) {
            try { C.Failure = Value::Error(E.what()); } catch (...) {}
        } catch (...) {}
        if (C.Result.Base != Kind::Void) {
            auto Width = C.Result.Abi()->size;
            // Integral returns narrower than ffi_arg use a full signed/unsigned word.
            switch (C.Result.Base) {
            case Kind::I8: Return.Put(ffi_sarg(Return.Get<int8_t>())); break;
            case Kind::I16: Return.Put(ffi_sarg(Return.Get<int16_t>())); break;
            case Kind::I32: Return.Put(ffi_sarg(Return.Get<int32_t>())); break;
            default: break;
            }
            if (C.Result.Base != Kind::F32) Width = sizeof(ffi_arg);
            std::memcpy(Out, Return.Data, Width);
        }
    }
    Factory& Objects;
    DWORD Thread;
    std::atomic<bool> WrongThread{false};
    Value Target, Failure;
    Spec Result{Kind::Void};
    std::vector<Spec> Params;
    std::vector<ffi_type*> Types;
    ffi_cif Cif{};
    std::unique_ptr<ffi_closure, decltype(&ffi_closure_free)> Closure{nullptr, ffi_closure_free};
};
struct Slot {
    Scalar Argument, Pointee;
    std::wstring WideText;
    std::vector<uint8_t> Bytes;
};
class Bound final : public Host {
public:
    Bound(NativeObjectType& Type, Factory& F, const std::vector<Value>& Args)
        : Host(Type), Objects(F) {
        Require(Args.size() >= 3, "ffi.bind expects library, symbol and return type");
        const auto Path = std::filesystem::path(Wide(TextOf(Args[0])));
        Name = TextOf(Args[1]);
        Require(!Path.empty() && !Name.empty(), "empty FFI library or symbol name");
        for (size_t I = 2; I < Args.size(); ++I) {
            auto* D = As<Descriptor>(Args[I]);
            Require(D, "ffi.bind expected an FFI type descriptor");
            auto S = D->Signature;
            if (I == 2) {
                Require(!S.ByRef && S.Base != Kind::Utf8 && S.Base != Kind::Utf16 &&
                        S.Base != Kind::Buffer, "unsupported FFI return type");
                Result = S;
            } else {
                Require(S.Base != Kind::Void, "void parameter in FFI signature");
                Params.push_back(S);
                Types.push_back(S.Abi());
                NeedsAuxiliary |= S.ByRef || S.Base == Kind::Buffer ||
                                  S.Base == Kind::Utf8 || S.Base == Kind::Utf16;
            }
        }
        Require(ffi_prep_cif(&Cif, FFI_DEFAULT_ABI, static_cast<unsigned>(Types.size()),
                             Result.Abi(), Types.data()) == FFI_OK, "invalid FFI signature");
        auto File = Path.has_parent_path() ? std::filesystem::absolute(Path) : Path;
        DWORD Flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        if (File.is_absolute()) Flags |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
        HMODULE H = LoadLibraryExW(File.c_str(), nullptr, Flags);
        if (!H) throw std::runtime_error("cannot load " + Args[0].AsString() +
                                         " (Windows error " + std::to_string(GetLastError()) + ")");
        Library = {H, [](void* P) { FreeLibrary(static_cast<HMODULE>(P)); }};
        Entry = GetProcAddress(H, Name.c_str());
        if (!Entry) throw std::runtime_error("missing export " + Name + " in " + Args[0].AsString());
    }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Args) override {
        try { return Invoke(Args); }
        catch (const std::exception& E) { return Value::Error(Name + ": " + E.what()); }
    }
private:
    Value Invoke(const std::vector<Value>& Args) {
        Require(Args.size() == Params.size(), "wrong argument count");
        constexpr size_t InlineArgs = 8;
        if (!NeedsAuxiliary && !Args.empty() && Args.size() <= InlineArgs) {
            // Invocation-local: a native callback may reenter this same Bound.
            std::array<Scalar, InlineArgs> Slots;
            std::array<void*, InlineArgs> Addresses;
            for (size_t I = 0; I < Args.size(); ++I) {
                if (Args[I].IsError()) return Args[I];
                WriteScalar(Slots[I], Params[I].Base, Args[I]);
                Addresses[I] = Slots[I].Data;
            }
            Scalar Output;
            ffi_call(&Cif, FFI_FN(Entry), Output.Data, Addresses.data());
            return ReadScalar(Output, Result.Base, Objects, Library);
        }
        std::vector<Slot> Slots(Args.size());
        std::vector<void*> Addresses(Args.size());
        std::unordered_map<Ref*, size_t> Refs;
        std::unordered_map<ScriptObject*, size_t> Buffers;
        for (size_t I = 0; I < Args.size(); ++I) {
            auto& Slot = Slots[I];
            auto S = Params[I];
            const auto& V = Args[I];
            if (V.IsError()) return V;
            Addresses[I] = Slot.Argument.Data;
            if (S.ByRef) {
                auto* R = As<Ref>(V);
                Require(R, "ByRef parameter requires ffi.Ref");
                auto [It, Fresh] = Refs.emplace(R, I);
                Require(Params[It->second].Base == S.Base, "aliased Ref has conflicting types");
                if (Fresh) WriteScalar(Slot.Pointee, S.Base, R->Val);
                Slot.Argument.Put(&Slots[It->second].Pointee.Data[0]);
            } else if (S.Base == Kind::Buffer) {
                Require(V.IsScriptObject() && V.AsScriptObject()->IsArray(), "Param requires a byte array");
                auto* Array = V.AsScriptObject();
                Require(Array->GetArrayLength() == S.Size, "wrong Param byte count");
                auto [It, Fresh] = Buffers.emplace(Array, I);
                if (Fresh) {
                    Slot.Bytes.reserve(S.Size);
                    for (auto& Element : Array->GetArrayElements()) {
                        Require(Element.GetType() == ValueType::Byte, "Param array contains a non-byte");
                        Slot.Bytes.push_back(Element.AsByte());
                    }
                }
                Slot.Argument.Put(Slots[It->second].Bytes.data());
            } else if (S.Base == Kind::Utf8 || S.Base == Kind::Utf16) {
                if (V.GetType() == ValueType::Null) Slot.Argument.Put(static_cast<void*>(nullptr));
                else if (S.Base == Kind::Utf8) Slot.Argument.Put(TextOf(V).c_str());
                else {
                    Slot.WideText = Wide(TextOf(V));
                    Slot.Argument.Put(Slot.WideText.c_str());
                }
            } else WriteScalar(Slot.Argument, S.Base, V);
        }
        Scalar Output;
        ffi_call(&Cif, FFI_FN(Entry), Output.Data, Addresses.data());
        for (auto [R, Index] : Refs) {
            const auto& Data = Slots[Index].Pointee;
            if (Params[Index].Base == Kind::Ptr && As<Pointer>(R->Val) &&
                As<Pointer>(R->Val)->Address == Data.Get<void*>()) continue;
            R->Val = ReadScalar(Data, Params[Index].Base, Objects, Library);
        }
        for (auto [Array, Index] : Buffers) {
            const auto& Bytes = Slots[Index].Bytes;
            for (size_t J = 0; J < Bytes.size(); ++J)
                Array->SetRaw(Value::Number(static_cast<double>(J)), Value::Byte(Bytes[J]));
        }
        return ReadScalar(Output, Result.Base, Objects, Library);
    }
    Factory& Objects;
    std::string Name;
    std::shared_ptr<void> Library;
    FARPROC Entry{};
    Spec Result{Kind::Void};
    bool NeedsAuxiliary = false;
    std::vector<Spec> Params;
    std::vector<ffi_type*> Types;
    ffi_cif Cif{};
};

Value Create(const NativeModuleContext& Context) {
    NativeModuleBuilder Module(Context.Machine, ModuleId, "ffi");
    auto& F = Context.Machine.CreateNativeObjectType<Factory>();
    const std::pair<const char*, Kind> Scalars[] = {
        {"Int8",Kind::I8}, {"UInt8",Kind::U8}, {"Int16",Kind::I16}, {"UInt16",Kind::U16},
        {"Int32",Kind::I32}, {"UInt32",Kind::U32}, {"Float32",Kind::F32},
        {"Float64",Kind::F64}, {"Pointer",Kind::Ptr}, {"IntPtr",Kind::Word}
    };
    for (auto [Name, K] : Scalars) {
        Module.AddValue(std::string(Name) + "ByVal", F.Make<Descriptor>(Spec{K}));
        Module.AddValue(std::string(Name) + "ByRef", F.Make<Descriptor>(Spec{K, true}));
    }
    Module.AddFunction("Buffer", [&F](const auto& A) {
        Require(A.size() == 1 && A[0].IsNumeric(), "Buffer expects a byte count");
        return F.Make<Memory>(Integer<uint32_t>(A[0].AsNumber()));
    });
    Module.AddFunction("Ptr", [&F](const auto& A) {
        Require(A.size() == 1 || A.size() == 2, "Ptr expects a ByteBuffer and optional byte offset, or a callback");
        if (As<Callback>(A[0])) {
            Require(A.size() == 1, "Ptr does not accept an offset for a callback");
            return A[0];
        }
        auto* Data = ByteBufferData(A[0]);
        Require(Data, "Ptr expects a ByteBuffer or callback");
        size_t Offset = 0;
        if (A.size() == 2) {
            Require(A[1].IsNumeric(), "Ptr expects an integer byte offset");
            double N = A[1].AsNumber();
            Require(std::trunc(N) == N && N >= 0 && N <= Data->size(), "Ptr byte offset outside buffer range");
            Offset = static_cast<size_t>(N);
        }
        auto V = F.Make<Pointer>(const_cast<char*>(Data->data()) + Offset, std::shared_ptr<void>{});
        auto* P = As<Pointer>(V);
        P->Owner = A[0];
        return V;
    });
    Module.AddFunction("SamePtr", [](const auto& A) {
        Require(A.size() == 2, "SamePtr expects two pointers or nulls");
        return Value::Bool(PointerAddress(A[0]) == PointerAddress(A[1]));
    });
    Module.AddFunction("Callback", [&F](const auto& A) { return F.Make<Callback>(F, A); });
    auto StructId = ModuleId;
    StructId.Bytes.back() ^= 1;
    NativeModuleBuilder Struct(Context.Machine, StructId, "ffi.Struct");
    for (auto [Name, K] : Scalars) {
        std::string FieldName(Name);
        if (FieldName.starts_with("UInt")) FieldName[1] = 'i';
        for (bool Write : {false, true})
            Struct.AddFunction((Write ? "Set" : "Get") + FieldName, [&F, K, Write](const auto& A) {
                return Field(F, K, Write, A);
            });
    }
    Module.AddValue("Struct", Struct.Build());
    Module.AddValue("Void", F.Make<Descriptor>(Spec{Kind::Void}));
    Module.AddValue("str", F.Make<Strings>(F));
    Module.AddFunction("bind", [&F](const auto& Args) {
        for (const auto& Arg : Args) if (Arg.IsError()) return Arg;
        return F.Make<Bound>(F, Args);
    });
    Module.AddFunction("Ref", [&F](const auto& Args) {
        Require(Args.size() == 1, "ffi.Ref expects one initial value");
        return F.Make<Ref>(Args[0]);
    });
    Module.AddFunction("Param", [&F](const auto& Args) {
        Require(Args.size() == 1 && Args[0].IsNumeric(), "ffi.Param expects a byte count");
        auto Size = Integer<uint32_t>(Args[0].AsNumber());
        Require(Size > 0, "ffi.Param requires a positive byte count");
        return F.Make<Descriptor>(Spec{Kind::Buffer, false, Size});
    });
    return Module.Build();
}
}
extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{ModuleId, "ffi", &Create};
    return &Descriptor;
}
