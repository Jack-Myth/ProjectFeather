#include <Feather/BinaryArchive.hpp>
#include <Feather/ByteBuffer.hpp>

namespace Feather {
namespace {

constexpr std::uint32_t MessageFormat = 0x314d5446; // FTM1

enum class Tag : std::uint8_t {
    Null = 0, Bool = 1, Byte = 2, Number = 3, String = 4,
    Reference = 5, RootMeta = 6, Object = 7, Array = 8, Native = 9,
};

} // namespace

class MessageValueArchive final {
public:
    explicit MessageValueArchive(BinaryArchive& Archive) : Archive(Archive) {}

    void Write(const Vm& Source, std::span<const Value> Arguments) {
        for (const auto& Argument : Arguments) Track(Source, Argument);
        for (std::size_t I = 0; I < Objects.size(); ++I) {
            if (!Objects[I].IsScriptObject()) continue;
            const auto* Object = Objects[I].AsScriptObject();
            if (Object->MetaObject) Track(Source, Value::FromScript(Object->MetaObject));
            for (const auto& [_, Data] : Object->Members) Track(Source, Data);
            for (const auto& Element : Object->GetArrayElements()) Track(Source, Element);
        }

        auto Format = MessageFormat;
        Archive.Access(Format);
        WriteCount(Arguments.size());
        WriteCount(Objects.size());
        for (const auto& Object : Objects) WriteDeclaration(Object);
        for (const auto& Argument : Arguments) WriteValue(Source, Argument);
        for (const auto& Item : Objects) {
            if (!Item.IsScriptObject()) continue;
            const auto* Object = Item.AsScriptObject();
            WriteValue(Source, Object->MetaObject ? Value::FromScript(Object->MetaObject) : Value{});
            if (Object->IsArray()) {
                const auto Elements = Object->GetArrayElements();
                WriteCount(Elements.size());
                for (const auto& Element : Elements) WriteValue(Source, Element);
            } else {
                WriteCount(Object->Members.size());
                for (const auto& [Key, Data] : Object->Members) {
                    WriteValue(Source, Key.Type == ValueType::String ? Value::String(Key.Text)
                                                                   : Value::Number(Key.Number));
                    WriteValue(Source, Data);
                }
            }
        }
    }

    std::vector<Value> Read(Vm& Target) {
        std::uint32_t Format = 0;
        Archive.Access(Format);
        if (Format != MessageFormat) throw std::invalid_argument("invalid message archive format");
        const auto ArgumentCount = ReadCount();
        const auto ObjectCount = ReadCount();

        // Create every shell before resolving references. GC may run while allocating.
        std::vector<RootHandle> Roots;
        Objects.reserve(ObjectCount);
        for (std::uint32_t I = 0; I < ObjectCount; ++I) {
            auto Object = ReadDeclaration(Target);
            if (Object.IsScriptObject()) Roots.push_back(Target.AddToRoot(Object));
            Objects.push_back(std::move(Object));
        }
        if (ArgumentCount > Archive.Remaining())
            throw std::invalid_argument("invalid message argument count");
        std::vector<Value> Arguments;
        Arguments.reserve(ArgumentCount);
        for (std::uint32_t I = 0; I < ArgumentCount; ++I) Arguments.push_back(ReadValue(Target));
        for (const auto& Item : Objects) {
            if (!Item.IsScriptObject()) continue;
            auto* Object = Item.AsScriptObject();
            auto Meta = ReadValue(Target);
            if (Meta.IsScriptObject()) Object->MetaObject = Meta.AsScriptObject();
            else if (Meta.GetType() == ValueType::Null) Object->MetaObject = nullptr;
            else throw std::invalid_argument("invalid message MetaObject");
            const auto Count = ReadCount();
            if (Object->IsArray()) {
                for (std::uint32_t I = 0; I < Count; ++I) Object->AppendArray(ReadValue(Target));
            } else {
                for (std::uint32_t I = 0; I < Count; ++I) {
                    auto Key = ScriptObject::ConvertKey(ReadValue(Target));
                    if (!Key || Object->Members.contains(*Key))
                        throw std::invalid_argument("invalid or duplicate message object key");
                    Object->Members.emplace(std::move(*Key), ReadValue(Target));
                }
            }
        }
        if (Archive.Remaining() != 0) throw std::invalid_argument("trailing message archive data");
        return Arguments;
    }

private:
    void Track(const Vm& Source, const Value& Input) {
        if (Input.GetType() != ValueType::Object) return;
        Source.ValidateOwnedValue(Input);
        if (!Input.IsScriptObject()) {
            const auto* Native = dynamic_cast<const NativeObject*>(Input.AsObject());
            const auto* Type = Native ? Native->GetNativeObjectType() : nullptr;
            if (Type && &Type->GetVm() != &Source)
                throw std::invalid_argument("message native object belongs to another VM");
        }
        if (Input.AsObject() == Source.GetRootMetaObject() || Ids.contains(Input.AsObject())) return;
        Ids.emplace(Input.AsObject(), CheckedCount(Objects.size()));
        Objects.push_back(Input);
    }

    void WriteDeclaration(const Value& Input) {
        if (Input.IsScriptObject()) {
            WriteTag(Input.AsScriptObject()->IsArray() ? Tag::Array : Tag::Object);
            return;
        }
        const auto* Buffer = ByteBufferData(Input);
        const auto* Native = dynamic_cast<const NativeObject*>(Input.AsObject());
        if (!Buffer || !Native || !Native->GcVisibleMembers.empty())
            throw std::invalid_argument("object type is not supported by message archives");
        const auto& Id = Native->GetNativeObjectType()->GetId();
        WriteTag(Tag::Native);
        for (auto Byte : Id.Module.Bytes) Archive.Access(Byte);
        Archive.WriteBytes(Id.Name);
        Archive.WriteBytes(*Buffer);
    }

    Value ReadDeclaration(Vm& Target) {
        switch (ReadTag()) {
        case Tag::Object: return Value::FromScript(Target.CreateScriptObject());
        case Tag::Array: return Value::FromScript(Target.CreateArray());
        case Tag::Native: {
            NativeTypeId Id;
            for (auto& Byte : Id.Module.Bytes) Archive.Access(Byte);
            Archive.Access(Id.Name);
            if (Id != ByteBufferLibrary::GetTypeId())
                throw std::invalid_argument("unknown native type in message archive");
            std::string Bytes;
            Archive.Access(Bytes);
            return ByteBufferLibrary(Target).Create(std::move(Bytes));
        }
        default: throw std::invalid_argument("invalid message object declaration");
        }
    }

    void WriteValue(const Vm& Source, const Value& Input) {
        switch (Input.GetType()) {
        case ValueType::Null: WriteTag(Tag::Null); break;
        case ValueType::Bool: {
            WriteTag(Tag::Bool);
            auto Data = Input.AsBool(); Archive.Access(Data); break;
        }
        case ValueType::Byte: {
            WriteTag(Tag::Byte);
            auto Data = Input.AsByte(); Archive.Access(Data); break;
        }
        case ValueType::Number: {
            WriteTag(Tag::Number);
            auto Data = Input.AsNumber(); Archive.Access(Data); break;
        }
        case ValueType::String:
            WriteTag(Tag::String); Archive.WriteBytes(Input.AsString()); break;
        case ValueType::Object:
            if (Input.AsObject() == Source.GetRootMetaObject()) {
                WriteTag(Tag::RootMeta);
            } else {
                WriteTag(Tag::Reference);
                auto Id = Ids.at(Input.AsObject()); Archive.Access(Id);
            }
            break;
        }
    }

    Value ReadValue(Vm& Target) {
        switch (ReadTag()) {
        case Tag::Null: return {};
        case Tag::Bool: {
            bool Data = false; Archive.Access(Data); return Value::Bool(Data);
        }
        case Tag::Byte: {
            std::uint8_t Data = 0; Archive.Access(Data); return Value::Byte(Data);
        }
        case Tag::Number: {
            double Data = 0; Archive.Access(Data); return Value::Number(Data);
        }
        case Tag::String: {
            std::string Data; Archive.Access(Data); return Value::String(std::move(Data));
        }
        case Tag::RootMeta: return Value::FromScript(Target.GetRootMetaObject());
        case Tag::Reference: {
            std::uint32_t Id = 0;
            Archive.Access(Id);
            if (Id >= Objects.size()) throw std::invalid_argument("invalid message object reference");
            return Objects[Id];
        }
        default: throw std::invalid_argument("unknown message value type");
        }
    }

    void WriteTag(Tag Type) {
        auto Byte = static_cast<std::uint8_t>(Type);
        Archive.Access(Byte);
    }
    Tag ReadTag() {
        std::uint8_t Byte = 0;
        Archive.Access(Byte);
        return static_cast<Tag>(Byte);
    }
    static std::uint32_t CheckedCount(std::size_t Count) {
        if (Count > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("message archive has too many entries");
        return static_cast<std::uint32_t>(Count);
    }
    void WriteCount(std::size_t Count) {
        auto Field = CheckedCount(Count);
        Archive.Access(Field);
    }
    std::uint32_t ReadCount() {
        std::uint32_t Count = 0;
        Archive.Access(Count);
        if (Count > Archive.Remaining()) throw std::invalid_argument("invalid message entry count");
        return Count;
    }

    BinaryArchive& Archive;
    std::vector<Value> Objects;
    std::unordered_map<const Object*, std::uint32_t> Ids;
};

std::vector<std::uint8_t> Vm::SerializeArguments(std::span<const Value> Arguments) const {
    BinaryArchive Archive;
    MessageValueArchive(Archive).Write(*this, Arguments);
    return std::move(Archive).Finish();
}

std::vector<Value> Vm::DeserializeArguments(std::span<const std::uint8_t> Bytes) {
    BinaryArchive Archive(Bytes);
    return MessageValueArchive(Archive).Read(*this);
}

} // namespace Feather
