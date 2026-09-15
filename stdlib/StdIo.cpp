#include <Feather/StdIo.hpp>

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
    explicit Function(Method Body) : Body(std::move(Body)) {}
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
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    void Add(std::string Name, Method Body) {
        Members.emplace(std::move(Name), Value::FromObject(std::make_shared<Function>(std::move(Body))));
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
    if (Input.GetType() != ValueType::Number) return false;
    double Number = Input.AsNumber();
    if (!std::isfinite(Number) || Number < 0 || Number > static_cast<double>(Limit) ||
        std::trunc(Number) != Number) return false;
    Result = static_cast<std::uint64_t>(Number);
    return true;
}

bool Offset(const Value& Input, std::int64_t& Result) {
    if (Input.GetType() != ValueType::Number) return false;
    double Number = Input.AsNumber();
    constexpr double MaxExact = 9007199254740991.0;
    if (!std::isfinite(Number) || Number < -MaxExact || Number > MaxExact ||
        std::trunc(Number) != Number) return false;
    Result = static_cast<std::int64_t>(Number);
    return true;
}

class ByteBuffer final : public NativeObject {
public:
    explicit ByteBuffer(std::string Bytes)
        : Bytes(std::make_shared<const std::string>(std::move(Bytes))) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    const std::string& Data() const { return *Bytes; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() != ValueType::String) return Error("member name must be a string");
        if (Key.AsString() == "Length") return Value::Number(static_cast<double>(Bytes->size()));
        if (Key.AsString() == "ToString") {
            auto Content = Bytes;
            return Value::FromObject(std::make_shared<Function>([Content = std::move(Content)](const std::vector<Value>& A) {
                if (!A.empty()) return Error("ByteBuffer.ToString expects no arguments");
                try { return Value::String(*Content); }
                catch (const std::invalid_argument&) { return Error("buffer is not valid UTF-8"); }
            }));
        }
        if (Key.AsString() == "Get") {
            auto Content = Bytes;
            return Value::FromObject(std::make_shared<Function>([Content = std::move(Content)](const std::vector<Value>& A) {
                std::uint64_t Index = 0;
                if (A.size() != 1 || !Integer(A[0], Index, Content->size()) || Index == Content->size())
                    return Error("ByteBuffer.Get expects an in-range byte index");
                return Value::Number(static_cast<unsigned char>((*Content)[static_cast<std::size_t>(Index)]));
            }));
        }
        return Error("unknown ByteBuffer member");
    }
private:
    std::shared_ptr<const std::string> Bytes;
};

class FileHandle final : public NativeObject {
public:
    FileHandle(std::FILE* File, std::shared_ptr<void> Owner, bool Readable, bool Writable)
        : File(File), Owner(std::move(Owner)), Readable(Readable), Writable(Writable) {}
    ~FileHandle() override { if (File) std::fclose(File); }
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool OwnedBy(const std::shared_ptr<void>& Input) const { return Owner == Input; }
    std::FILE* Get() const { return File; }
    bool CanRead() const { return Readable; }
    bool CanWrite() const { return Writable; }
    int Close() {
        auto* Closing = std::exchange(File, nullptr);
        return Closing ? std::fclose(Closing) : EOF;
    }
private:
    std::FILE* File;
    std::shared_ptr<void> Owner;
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

std::string Display(const Value& Input) {
    switch (Input.GetType()) {
    case ValueType::Null: return "null";
    case ValueType::Bool: return Input.AsBool() ? "true" : "false";
    case ValueType::String: return Input.AsString();
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

StdIoLibrary::StdIoLibrary(std::istream& Input, std::ostream& Output)
    : Shared(std::make_shared<State>(Input, Output)) {
    auto ConsoleNamespace = std::make_shared<Namespace>();
    ConsoleNamespace->Add("Print", [State = Shared](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Console.Print expects one value");
        State->Output << Display(A[0]);
        State->Output.flush();
        return State->Output ? Value{} : Error("console output failed");
    });
    ConsoleNamespace->Add("PrintLine", [State = Shared](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("Console.PrintLine expects one value");
        State->Output << Display(A[0]) << '\n';
        State->Output.flush();
        return State->Output ? Value{} : Error("console output failed");
    });
    ConsoleNamespace->Add("InputLine", [State = Shared](const std::vector<Value>& A) {
        if (!A.empty()) return Error("Console.InputLine expects no arguments");
        std::string Line;
        if (!std::getline(State->Input, Line))
            return State->Input.eof() ? Value{} : Error("console input failed");
        try { return Value::String(std::move(Line)); }
        catch (const std::invalid_argument&) { return Error("console input is not valid UTF-8"); }
    });
    ConsoleObject = ConsoleNamespace;

    auto IONamespace = std::make_shared<Namespace>();
    auto Owner = std::static_pointer_cast<void>(Shared);
    IONamespace->Add("OpenFile", [Owner](const std::vector<Value>& A) {
        if (A.size() != 2 || A[0].GetType() != ValueType::String || A[1].GetType() != ValueType::String)
            return Error("IO.OpenFile expects path and mode strings");
        const auto& Mode = A[1].AsString();
        bool Readable = Mode == "r" || Mode == "r+" || Mode == "w+" || Mode == "a+";
        bool Writable = Mode == "w" || Mode == "a" || Mode == "r+" || Mode == "w+" || Mode == "a+";
        if (!Readable && !Writable) return Error("invalid file mode");
        std::FILE* File = Open(A[0].AsString(), Mode + "b");
        if (!File) return Error("cannot open file");
        return Value::FromObject(std::make_shared<FileHandle>(File, Owner, Readable, Writable));
    });
    IONamespace->Add("CloseFile", [Owner](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("IO.CloseFile expects one handle");
        auto* File = Handle(A, Owner);
        if (!File) return Error("invalid or closed file handle");
        return File->Close() == 0 ? Value::Bool(true) : Error("file close failed");
    });
    IONamespace->Add("Tell", [Owner](const std::vector<Value>& A) {
        if (A.size() != 1) return Error("IO.Tell expects one handle");
        auto* File = Handle(A, Owner);
        if (!File) return Error("invalid or closed file handle");
        auto Position = TellFile(File->Get());
        return Position < 0 || Position > 9007199254740991LL ? Error("file position unavailable")
            : Value::Number(static_cast<double>(Position));
    });
    IONamespace->Add("Seek", [Owner](const std::vector<Value>& A) {
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
    IONamespace->Add("Read", [Owner](const std::vector<Value>& A) {
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
        return Value::FromObject(std::make_shared<ByteBuffer>(std::move(Bytes)));
    });
    IONamespace->Add("Write", [Owner](const std::vector<Value>& A) {
        if (A.size() != 2) return Error("IO.Write expects handle and string or ByteBuffer");
        auto* File = Handle(A, Owner);
        if (!File || !File->CanWrite()) return Error("invalid, closed, or unwritable file handle");
        const std::string* Bytes = nullptr;
        if (A[1].GetType() == ValueType::String) Bytes = &A[1].AsString();
        else if (A[1].GetType() == ValueType::Object && !A[1].IsScriptObject()) {
            if (auto* Buffer = dynamic_cast<ByteBuffer*>(A[1].AsObject())) Bytes = &Buffer->Data();
        }
        if (!Bytes) return Error("IO.Write expects a string or ByteBuffer");
        auto Written = std::fwrite(Bytes->data(), 1, Bytes->size(), File->Get());
        if (Written != Bytes->size()) return Error("file write failed");
        return Value::Number(static_cast<double>(Written));
    });
    IOObject = IONamespace;
}

StdIoLibrary::~StdIoLibrary() = default;
Value StdIoLibrary::Resolve(std::string_view Specifier) const {
    if (Specifier == "std:console") return Value::FromObject(ConsoleObject);
    if (Specifier == "std:io") return Value::FromObject(IOObject);
    return {};
}

} // namespace Feather
