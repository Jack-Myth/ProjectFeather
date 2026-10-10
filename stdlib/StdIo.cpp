#include <Feather/StdIo.hpp>
#include <Feather/ByteBuffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Feather {
namespace {

Value Error(std::string Message) {
    return Value::FromObject(std::make_shared<ErrorObject>(std::move(Message)));
}

using Method = std::function<Value(const std::vector<Value>&)>;

class Function final : public NativeObject {
public:
    Function(NativeObjectType& Type, Method Body)
        : NativeObject(Type), Body(std::move(Body)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        try { return Body(Arguments); }
        catch (const std::exception& Failure) { return Error(Failure.what()); }
    }
private:
    Method Body;
};

class Namespace final : public NativeObject {
public:
    explicit Namespace(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    void AddValue(std::string Name, Value Input) {
        Members.emplace(std::move(Name), std::move(Input));
    }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String) return Error("member name must be a string");
        auto Found = Members.find(Key.AsString());
        return Found == Members.end() ? Error("unknown standard IO member") : Found->second;
    }
private:
    std::unordered_map<std::string, Value> Members;
};

bool Integer(const Value& Input, std::uint64_t& Result, std::uint64_t Limit) {
    if (!Input.IsNumeric()) return false;
    double Number = Input.AsNumber();
    if (!std::isfinite(Number) || Number < 0 || Number > static_cast<double>(Limit) ||
        std::trunc(Number) != Number) return false;
    Result = static_cast<std::uint64_t>(Number);
    return true;
}

bool Offset(const Value& Input, std::int64_t& Result) {
    if (!Input.IsNumeric()) return false;
    double Number = Input.AsNumber();
    constexpr double MaxExact = 9007199254740991.0;
    if (!std::isfinite(Number) || Number < -MaxExact || Number > MaxExact ||
        std::trunc(Number) != Number) return false;
    Result = static_cast<std::int64_t>(Number);
    return true;
}

class FileHandle final : public NativeObject {
public:
    FileHandle(NativeObjectType& Type, std::FILE* File, std::shared_ptr<void> Owner,
               std::string Path, std::string Mode, bool Readable, bool Writable)
        : NativeObject(Type), File(File), Owner(std::move(Owner)), Path(std::move(Path)),
          Mode(std::move(Mode)), Readable(Readable), Writable(Writable) {}
    ~FileHandle() override { if (File) std::fclose(File); }
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool OwnedBy(const std::shared_ptr<void>& Input) const { return Owner == Input; }
    std::FILE* Get() const { return File; }
    const std::string& GetPath() const { return Path; }
    const std::string& GetMode() const { return Mode; }
    bool CanRead() const { return Readable; }
    bool CanWrite() const { return Writable; }
    int Close() {
        auto* Closing = std::exchange(File, nullptr);
        return Closing ? std::fclose(Closing) : EOF;
    }
private:
    std::FILE* File;
    std::shared_ptr<void> Owner;
    std::string Path;
    std::string Mode;
    bool Readable;
    bool Writable;
};

FileHandle* Handle(const std::vector<Value>& A, const std::shared_ptr<void>& Owner) {
    if (A.empty() || A[0].GetType() != ValueType::Object || A[0].IsScriptObject()) return nullptr;
    auto* File = dynamic_cast<FileHandle*>(A[0].AsObject());
    return File && File->OwnedBy(Owner) && File->Get() ? File : nullptr;
}

std::FILE* Open(const std::string& Path, const std::string& Mode) {
    std::u8string Utf8Path;
    Utf8Path.reserve(Path.size());
    for (unsigned char Byte : Path) Utf8Path.push_back(static_cast<char8_t>(Byte));
    auto Name = std::filesystem::path(Utf8Path);
#ifdef _WIN32
    std::wstring WideMode(Mode.begin(), Mode.end());
    std::FILE* File = nullptr;
    _wfopen_s(&File, Name.c_str(), WideMode.c_str());
    return File;
#else
    return std::fopen(Name.c_str(), Mode.c_str());
#endif
}

std::string AbsolutePath(const std::string& Path) {
    std::u8string Utf8Path;
    Utf8Path.reserve(Path.size());
    for (unsigned char Byte : Path) Utf8Path.push_back(static_cast<char8_t>(Byte));
    auto Absolute = std::filesystem::absolute(std::filesystem::path(Utf8Path)).lexically_normal();
    auto Bytes = Absolute.u8string();
    std::string Result;
    Result.reserve(Bytes.size());
    for (char8_t Byte : Bytes) Result.push_back(static_cast<char>(Byte));
    return Result;
}

int SeekFile(std::FILE* File, std::int64_t Offset, int Origin) {
#ifdef _WIN32
    return _fseeki64(File, Offset, Origin);
#else
    return fseeko(File, static_cast<off_t>(Offset), Origin);
#endif
}

std::int64_t TellFile(std::FILE* File) {
#ifdef _WIN32
    return _ftelli64(File);
#else
    return ftello(File);
#endif
}

void AppendU32(std::vector<std::uint8_t>& Output, std::uint32_t Value) {
    for (unsigned I = 0; I < 4; ++I)
        Output.push_back(static_cast<std::uint8_t>(Value >> (I * 8)));
}

void AppendU64(std::vector<std::uint8_t>& Output, std::uint64_t Value) {
    for (unsigned I = 0; I < 8; ++I)
        Output.push_back(static_cast<std::uint8_t>(Value >> (I * 8)));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> Input, std::size_t& At) {
    if (At > Input.size() || Input.size() - At < 4)
        throw std::invalid_argument("truncated stdio payload");
    std::uint32_t Result = 0;
    for (unsigned I = 0; I < 4; ++I) Result |= std::uint32_t(Input[At++]) << (I * 8);
    return Result;
}

std::uint64_t ReadU64(std::span<const std::uint8_t> Input, std::size_t& At) {
    if (At > Input.size() || Input.size() - At < 8)
        throw std::invalid_argument("truncated stdio payload");
    std::uint64_t Result = 0;
    for (unsigned I = 0; I < 8; ++I) Result |= std::uint64_t(Input[At++]) << (I * 8);
    return Result;
}

void AppendString(std::vector<std::uint8_t>& Output, const std::string& Value) {
    if (Value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("stdio snapshot string is too large");
    AppendU32(Output, static_cast<std::uint32_t>(Value.size()));
    Output.insert(Output.end(), Value.begin(), Value.end());
}

std::string ReadString(std::span<const std::uint8_t> Input, std::size_t& At) {
    auto Size = ReadU32(Input, At);
    if (At > Input.size() || Input.size() - At < Size)
        throw std::invalid_argument("truncated stdio string");
    std::string Result(reinterpret_cast<const char*>(Input.data() + At), Size);
    At += Size;
    (void)Value::String(Result);
    return Result;
}

std::pair<bool, bool> ModeAccess(std::string_view Mode) {
    bool Readable = Mode == "r" || Mode == "r+" || Mode == "w+" || Mode == "a+";
    bool Writable = Mode == "w" || Mode == "a" || Mode == "r+" || Mode == "w+" || Mode == "a+";
    return {Readable, Writable};
}

std::string RestoreMode(std::string_view Mode) {
    if (Mode == "r") return "rb";
    if (Mode == "r+" || Mode == "w" || Mode == "w+") return "r+b";
    if (Mode == "a") return "ab";
    if (Mode == "a+") return "a+b";
    throw std::invalid_argument("invalid saved file mode");
}

class FileHandleType final : public NativeObjectType {
public:
    FileHandleType(Vm& Machine, std::shared_ptr<void> Owner)
        : NativeObjectType(Machine, StdIoModuleGuid, "FileHandle"), Owner(std::move(Owner)) {}
    std::shared_ptr<FileHandle> Create(std::FILE* File, std::string Path, std::string Mode,
                                       bool Readable, bool Writable) {
        return CreateObject<FileHandle>(File, Owner, std::move(Path), std::move(Mode),
                                        Readable, Writable);
    }
    std::vector<std::uint8_t> Serialize(const NativeObject& Input) const override {
        auto* Handle = dynamic_cast<const FileHandle*>(&Input);
        if (!Handle || Handle->GetNativeObjectType() != this)
            throw std::invalid_argument("invalid stdio file handle");
        if (!Handle->Get()) return {0};
        if (Handle->CanWrite() && std::fflush(Handle->Get()) != 0)
            throw std::runtime_error("cannot flush file for snapshot");
        auto Position = TellFile(Handle->Get());
        if (Position < 0) throw std::runtime_error("cannot save file position");
        std::vector<std::uint8_t> Result{1};
        AppendString(Result, Handle->GetPath());
        AppendString(Result, Handle->GetMode());
        AppendU64(Result, static_cast<std::uint64_t>(Position));
        return Result;
    }
    std::shared_ptr<NativeObject> Deserialize(
        std::uint32_t Version, std::span<const std::uint8_t> Payload) override {
        if (Version != 1 || Payload.empty() || Payload[0] > 1)
            throw std::invalid_argument("invalid stdio file handle payload");
        if (Payload[0] == 0) {
            if (Payload.size() != 1)
                throw std::invalid_argument("invalid closed file handle payload");
            return Create(nullptr, {}, {}, false, false);
        }
        std::size_t At = 1;
        auto Path = ReadString(Payload, At);
        auto Mode = ReadString(Payload, At);
        auto Position = ReadU64(Payload, At);
        if (At != Payload.size() || Position > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("invalid stdio file handle payload");
        auto [Readable, Writable] = ModeAccess(Mode);
        if (!Readable && !Writable) throw std::invalid_argument("invalid saved file mode");
        auto* File = Open(Path, RestoreMode(Mode));
        if (!File) throw std::runtime_error("cannot reopen saved file");
        if (SeekFile(File, static_cast<std::int64_t>(Position), SEEK_SET) != 0) {
            std::fclose(File);
            throw std::runtime_error("cannot restore saved file position");
        }
        return Create(File, std::move(Path), std::move(Mode), Readable, Writable);
    }
private:
    std::shared_ptr<void> Owner;
};

std::string Display(const Value& Input) {
    switch (Input.GetType()) {
    case ValueType::Null: return "null";
    case ValueType::Bool: return Input.AsBool() ? "true" : "false";
    case ValueType::String: return Input.AsString();
    case ValueType::Byte: return std::to_string(Input.AsByte());
    case ValueType::Number: {
        std::ostringstream Text;
        Text.imbue(std::locale::classic());
        Text << std::setprecision(std::numeric_limits<double>::max_digits10) << Input.AsNumber();
        return Text.str();
    }
    case ValueType::Object: throw std::invalid_argument("Console cannot print an object");
    }
    throw std::invalid_argument("unknown value type");
}

} // namespace

struct StdIoLibrary::State {
    State(std::istream& Input, std::ostream& Output) : Input(Input), Output(Output) {}
    std::istream& Input;
    std::ostream& Output;
};

StdIoLibrary::StdIoLibrary(Vm& Machine, std::istream& Input, std::ostream& Output)
    : Shared(std::make_shared<State>(Input, Output)) {
    auto MakeNamespace = [&](std::string Name) {
        auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
            StdIoModuleGuid, std::move(Name));
        return Type.Create<Namespace>();
    };
    auto AddMethod = [&](const std::shared_ptr<Namespace>& Target, std::string TypeName,
                         std::string Name, Method Body) {
        auto& Type = Machine.CreateNativeObjectType<NativeSingletonType>(
            StdIoModuleGuid, std::move(TypeName));
        Target->AddValue(std::move(Name), Value::FromObject(Type.Create<Function>(std::move(Body))));
    };
    ByteBufferLibrary Buffers(Machine);
    auto* Files = &Machine.CreateNativeObjectType<FileHandleType>(
        std::static_pointer_cast<void>(Shared));

    auto ConsoleNamespace = MakeNamespace("Console");
    AddMethod(ConsoleNamespace, "Console.Print", "Print", [State = Shared](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Console.Print expects one value");
        State->Output << Display(A[0]);
        State->Output.flush();
        return State->Output ? Value{} : Error("console output failed");
    });
    AddMethod(ConsoleNamespace, "Console.PrintLine", "PrintLine", [State = Shared](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Console.PrintLine expects one value");
        State->Output << Display(A[0]) << '\n';
        State->Output.flush();
        return State->Output ? Value{} : Error("console output failed");
    });
    AddMethod(ConsoleNamespace, "Console.InputLine", "InputLine", [State = Shared](const std::vector<Value>& A) {
        if (!A.empty()) return Error("Console.InputLine expects no arguments");
        std::string Line;
        if (!std::getline(State->Input, Line))
            return State->Input.eof() ? Value{} : Error("console input failed");
        try { return Value::String(std::move(Line)); }
        catch (const std::invalid_argument&) { return Error("console input is not valid UTF-8"); }
    });
    ConsoleObject = ConsoleNamespace;

    auto IONamespace = MakeNamespace("IO");
    auto Owner = std::static_pointer_cast<void>(Shared);
    AddMethod(IONamespace, "IO.OpenFile", "OpenFile", [Owner, Files](const std::vector<Value>& A) {
        if (A.size() != 2 || A[0].GetType() != ValueType::String || A[1].GetType() != ValueType::String)
            return Error("IO.OpenFile expects path and mode strings");
        const auto& Mode = A[1].AsString();
        auto [Readable, Writable] = ModeAccess(Mode);
        if (!Readable && !Writable) return Error("invalid file mode");
        auto Path = AbsolutePath(A[0].AsString());
        std::FILE* File = Open(Path, Mode + "b");
        if (!File) return Error("cannot open file");
        return Value::FromObject(Files->Create(
            File, std::move(Path), Mode, Readable, Writable));
    });
    AddMethod(IONamespace, "IO.CloseFile", "CloseFile", [Owner](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("IO.CloseFile expects one handle");
        auto* File = Handle(A, Owner);
        if (!File) return Error("invalid or closed file handle");
        return File->Close() == 0 ? Value::Bool(true) : Error("file close failed");
    });
    AddMethod(IONamespace, "IO.Tell", "Tell", [Owner](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("IO.Tell expects one handle");
        auto* File = Handle(A, Owner);
        if (!File) return Error("invalid or closed file handle");
        auto Position = TellFile(File->Get());
        return Position < 0 || Position > 9007199254740991LL ? Error("file position unavailable")
            : Value::Number(static_cast<double>(Position));
    });
    AddMethod(IONamespace, "IO.Seek", "Seek", [Owner](const std::vector<Value>& A) {
        std::int64_t Distance = 0;
        if (A.size() != 3 || !Offset(A[1], Distance) || A[2].GetType() != ValueType::String)
            return Error("IO.Seek expects handle, integer offset, and origin");
        auto* File = Handle(A, Owner);
        if (!File) return Error("invalid or closed file handle");
        const auto& Origin = A[2].AsString();
        int Base = Origin == "start" ? SEEK_SET : Origin == "current" ? SEEK_CUR :
                   Origin == "end" ? SEEK_END : -1;
        if (Base < 0) return Error("invalid seek origin");
        if (SeekFile(File->Get(), Distance, Base) != 0) return Error("file seek failed");
        auto Position = TellFile(File->Get());
        return Position < 0 || Position > 9007199254740991LL ? Error("file position unavailable")
            : Value::Number(static_cast<double>(Position));
    });
    AddMethod(IONamespace, "IO.Read", "Read", [Owner, Buffers](const std::vector<Value>& A) {
        constexpr std::uint64_t MaxRead = 16 * 1024 * 1024;
        std::uint64_t Count = 0;
        if (A.size() != 2 || !Integer(A[1], Count, MaxRead))
            return Error("IO.Read expects handle and byte count (at most 16 MiB)");
        auto* File = Handle(A, Owner);
        if (!File || !File->CanRead()) return Error("invalid, closed, or unreadable file handle");
        std::string Bytes(static_cast<std::size_t>(Count), '\0');
        auto ReadCount = std::fread(Bytes.data(), 1, Bytes.size(), File->Get());
        if (std::ferror(File->Get())) return Error("file read failed");
        Bytes.resize(ReadCount);
        return Buffers.Create(std::move(Bytes));
    });
    AddMethod(IONamespace, "IO.Write", "Write", [Owner](const std::vector<Value>& A) {
        if (A.size() != 2) return Error("IO.Write expects handle and byte, string or ByteBuffer");
        auto* File = Handle(A, Owner);
        if (!File || !File->CanWrite()) return Error("invalid, closed, or unwritable file handle");
        std::string SingleByte;
        const std::string* Bytes = nullptr;
        if (A[1].GetType() == ValueType::Byte) {
            SingleByte.assign(1, static_cast<char>(A[1].AsByte()));
            Bytes = &SingleByte;
        }
        if (A[1].GetType() == ValueType::String) Bytes = &A[1].AsString();
        else if (A[1].GetType() == ValueType::Object && !A[1].IsScriptObject()) {
            Bytes = ByteBufferData(A[1]);
        }
        if (!Bytes) return Error("IO.Write expects a byte, string or ByteBuffer");
        auto Written = std::fwrite(Bytes->data(), 1, Bytes->size(), File->Get());
        if (Written != Bytes->size()) return Error("file write failed");
        return Value::Number(static_cast<double>(Written));
    });
    IOObject = IONamespace;
    auto ModuleNamespace = MakeNamespace("Module");
    ModuleNamespace->AddValue("Console", Value::FromObject(ConsoleObject));
    ModuleNamespace->AddValue("IO", Value::FromObject(IOObject));
    ModuleObject = std::move(ModuleNamespace);
}

StdIoLibrary::~StdIoLibrary() = default;
Value StdIoLibrary::GetModule() const { return Value::FromObject(ModuleObject); }

} // namespace Feather
