#include <Feather/NativeModule.hpp>
#include <SDL3/SDL.h>
#include <opusfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>

namespace {
using namespace Feather;
constexpr NativeModuleGuid Guid{{0x47, 0xf1, 0x5c, 0xe1, 0xa3, 0x72, 0x45, 0x4b,
                                 0xa1, 0xe7, 0xd8, 0x68, 0xd4, 0xfb, 0x93, 0x24}};
constexpr std::size_t MaxAudioBytes = 128 * 1024 * 1024;

class AudioFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
AudioFailure SdlError(const char* Operation) {
    return AudioFailure(std::string(Operation) + ": " + SDL_GetError());
}

struct Clip {
    SDL_AudioSpec Spec{};
    std::vector<std::uint8_t> Samples;
    std::size_t FrameBytes() const { return SDL_AUDIO_FRAMESIZE(Spec); }
    std::size_t Frames() const { return Samples.size() / FrameBytes(); }
    double Duration() const { return static_cast<double>(Frames()) / Spec.freq; }
};

Clip LoadClip(const std::string& Path) {
    if (Path.find('\0') != std::string::npos) throw AudioFailure("invalid audio path");
    const std::filesystem::path FilePath(std::u8string(Path.begin(), Path.end()));
    std::ifstream File(FilePath, std::ios::binary | std::ios::ate);
    const auto Size = static_cast<std::streamoff>(File.tellg());
    if (Size <= 0 || Size > static_cast<std::streamoff>(MaxAudioBytes))
        throw AudioFailure("audio file is unreadable, empty or exceeds 128 MiB");
    std::vector<unsigned char> Bytes(static_cast<std::size_t>(Size));
    File.seekg(0);
    if (!File.read(reinterpret_cast<char*>(Bytes.data()), Size))
        throw AudioFailure("could not read audio file");

    Clip Result;
    if (Bytes.size() >= 4 && std::memcmp(Bytes.data(), "OggS", 4) == 0) {
        int Code = 0;
        std::unique_ptr<OggOpusFile, decltype(&op_free)> Opus(
            op_open_memory(Bytes.data(), Bytes.size(), &Code), op_free);
        if (!Opus) throw AudioFailure("could not open Ogg Opus (" + std::to_string(Code) + ")");
        Result.Spec = {SDL_AUDIO_S16, 2, 48000};
        std::array<opus_int16, 5760 * 2> Buffer;
        for (;;) {
            int Frames = op_read_stereo(Opus.get(), Buffer.data(), static_cast<int>(Buffer.size()));
            if (Frames == 0) break;
            if (Frames < 0) throw AudioFailure("could not decode Ogg Opus (" + std::to_string(Frames) + ")");
            const auto Count = static_cast<std::size_t>(Frames) * Result.FrameBytes();
            if (Count > MaxAudioBytes - Result.Samples.size())
                throw AudioFailure("decoded audio exceeds 128 MiB");
            const auto* Data = reinterpret_cast<const std::uint8_t*>(Buffer.data());
            Result.Samples.insert(Result.Samples.end(), Data, Data + Count);
        }
    } else {
        auto* Input = SDL_IOFromConstMem(Bytes.data(), Bytes.size());
        if (!Input) throw SdlError("could not read WAV");
        Uint8* Data = nullptr;
        Uint32 Length = 0;
        if (!SDL_LoadWAV_IO(Input, true, &Result.Spec, &Data, &Length))
            throw SdlError("could not decode WAV");
        std::unique_ptr<Uint8, decltype(&SDL_free)> Samples(Data, SDL_free);
        if (Length > MaxAudioBytes) throw AudioFailure("decoded audio exceeds 128 MiB");
        if (Length) Result.Samples.assign(Data, Data + Length);
    }
    if (Result.Spec.freq <= 0 || Result.FrameBytes() == 0 || Result.Samples.empty() ||
        Result.Samples.size() % Result.FrameBytes() != 0)
        throw AudioFailure("invalid or empty decoded audio");
    return Result;
}

class Device {
public:
    explicit Device(const SDL_AudioSpec& Spec) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw SdlError("could not initialize audio");
        Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Spec, nullptr, nullptr);
        if (!Stream) {
            auto Failure = SdlError("could not open audio device");
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            throw Failure;
        }
    }
    ~Device() {
        SDL_DestroyAudioStream(Stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    SDL_AudioStream* Stream = nullptr;
};

enum class Event { Play, Paused, Stopped, Finished };
constexpr std::array EventNames{"OnPlay", "OnPaused", "OnStoped", "OnFinished"};
std::optional<Event> FindEvent(const Value& Key) {
    if (Key.GetType() != ValueType::String) return std::nullopt;
    const auto& Name = Key.AsString();
    if (Name == "onPlay") return Event::Play;
    if (Name == "OnStopped") return Event::Stopped;
    for (std::size_t Index = 0; Index < EventNames.size(); ++Index)
        if (Name == EventNames[Index]) return static_cast<Event>(Index);
    return std::nullopt;
}

class Sound final : public NativeObject, public std::enable_shared_from_this<Sound> {
public:
    Sound(NativeObjectType& Type, Clip Input) : NativeObject(Type), Samples(std::move(Input)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (auto Which = FindEvent(Key)) return Callbacks[static_cast<std::size_t>(*Which)];
        if (Key.GetType() == ValueType::String && Key.AsString() == "Duration" && !Closed)
            return Value::Number(Samples.Duration());
        return Value::Error("unknown or unavailable audio member");
    }
    Value SetMember(const Value& Key, const Value& Input) override {
        auto Which = FindEvent(Key);
        if (!Which || Closed) return Value::Error("audio member is not writable");
        if (Input.GetType() != ValueType::Null && !Machine().IsCallable(Input))
            return Value::Error("audio delegate must be callable or null");
        const auto Index = static_cast<std::size_t>(*Which);
        if (Input.GetType() == ValueType::Null) RemoveGcVisibleMember(EventNames[Index]);
        else SetGcVisibleMember(EventNames[Index], Input);
        Callbacks[Index] = Input;
        return Input;
    }

    Value Play() {
        if (Playing) return Value::Bool(true);
        auto Hook = Machine().RegisterIdleHook([Weak = weak_from_this()](double) {
            auto Handle = Weak.lock();
            return Handle ? Handle->Poll() : Value::Bool(false);
        });
        if (!Output) {
            auto New = std::make_unique<Device>(Samples.Spec);
            Queue(New->Stream, Offset);
            Output = std::move(New);
        } else if (Drained) {
            Queue(Output->Stream, Offset);
        }
        if (!SDL_ResumeAudioStreamDevice(Output->Stream)) throw SdlError("could not resume audio");
        Playing = true;
        Drained = false;
        Idle = std::move(Hook);
        return Notify(Event::Play);
    }
    Value Pause() {
        if (!Playing) return Value::Bool(true);
        if (!SDL_PauseAudioStreamDevice(Output->Stream)) throw SdlError("could not pause audio");
        Playing = false;
        Idle.reset();
        return Notify(Event::Paused);
    }
    Value Seek(double Seconds) {
        if (!std::isfinite(Seconds) || Seconds < 0 || Seconds > Samples.Duration())
            return Value::Error("audio seek position is outside the sound");
        const auto Frame = Seconds == Samples.Duration() ? Samples.Frames() :
            static_cast<std::size_t>(Seconds * Samples.Spec.freq);
        if (Output) {
            auto New = std::make_unique<Device>(Samples.Spec);
            Queue(New->Stream, Frame);
            if (Playing) {
                if (!SDL_PauseAudioStreamDevice(Output->Stream)) throw SdlError("could not pause audio for seek");
                if (!SDL_ResumeAudioStreamDevice(New->Stream)) {
                    auto Failure = SdlError("could not resume audio after seek");
                    if (!SDL_ResumeAudioStreamDevice(Output->Stream)) Reset(Offset);
                    throw Failure;
                }
            }
            Output = std::move(New);
            Drained = false;
        }
        Offset = Frame;
        return Value::Bool(true);
    }
    Value Stop() {
        const bool Changed = (Output && !Drained) || Offset != 0;
        Reset(0);
        return Changed ? Notify(Event::Stopped) : Value::Bool(true);
    }
    Value Close() {
        Reset(0);
        for (std::size_t Index = 0; Index < EventNames.size(); ++Index) {
            RemoveGcVisibleMember(EventNames[Index]);
            Callbacks[Index] = {};
        }
        std::vector<std::uint8_t>().swap(Samples.Samples);
        Closed = true;
        return Value::Bool(true);
    }
    bool IsClosed() const { return Closed; }

private:
    Vm& Machine() { return GetNativeObjectType()->GetVm(); }
    void Queue(SDL_AudioStream* Stream, std::size_t Frame) {
        const auto Start = Frame * Samples.FrameBytes();
        const auto Count = static_cast<int>(Samples.Samples.size() - Start);
        if ((Count != 0 && !SDL_PutAudioStreamData(Stream, Samples.Samples.data() + Start, Count)) ||
            !SDL_FlushAudioStream(Stream)) throw SdlError("could not queue audio");
    }
    void Reset(std::size_t Frame) {
        Idle.reset();
        Output.reset();
        Playing = false;
        Drained = false;
        Offset = Frame;
    }
    Value Notify(Event Which) {
        const auto Callback = Callbacks[static_cast<std::size_t>(Which)];
        if (Callback.GetType() != ValueType::Null) {
            auto Result = Machine().Call(Callback, {Value::FromObject(shared_from_this())});
            if (Result.IsError()) return Result;
        }
        return Value::Bool(true);
    }
    Value Poll() {
        if (!Playing) return Value::Bool(false);
        const int Queued = SDL_GetAudioStreamQueued(Output->Stream);
        const int Available = SDL_GetAudioStreamAvailable(Output->Stream);
        if (Queued < 0 || Available < 0) {
            auto Failure = Value::Error(std::string("could not query audio: ") + SDL_GetError());
            Reset(0);
            return Failure;
        }
        if (Queued || Available) return Value::Bool(false);
        // Keep the device open so the last submitted hardware buffer can play.
        Idle.reset();
        Playing = false;
        Drained = true;
        Offset = 0;
        return Notify(Event::Finished);
    }

    Clip Samples;
    std::array<Value, EventNames.size()> Callbacks;
    std::unique_ptr<Device> Output;
    std::shared_ptr<IdleRegistration> Idle;
    std::size_t Offset = 0;
    bool Playing = false;
    bool Drained = false;
    bool Closed = false;
};

class SoundType final : public NativeObjectType {
public:
    SoundType(Vm& Machine, NativeModuleGuid Id, std::string Name)
        : NativeObjectType(Machine, Id, std::move(Name)) {}
    Value Open(const std::string& Path) { return Value::FromObject(CreateObject<Sound>(LoadClip(Path))); }
};
class Function final : public NativeObject {
public:
    using Body = std::function<Value(const std::vector<Value>&)>;
    Function(NativeObjectType& Type, Body Action) : NativeObject(Type), Action(std::move(Action)) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    bool IsCallable() const override { return true; }
    Value Call(const std::vector<Value>& Arguments) override {
        try { return Action(Arguments); }
        catch (const AudioFailure& Failure) { return Value::Error(Failure.what()); }
    }
private:
    Body Action;
};
class Module final : public NativeObject {
public:
    explicit Module(NativeObjectType& Type) : NativeObject(Type) {}
    ObjectType GetObjectType() const override { return ObjectType::Host; }
    Value GetMember(const Value& Key) override {
        if (Key.GetType() == ValueType::String) {
            auto It = Members.find(Key.AsString());
            if (It != Members.end()) return It->second;
        }
        return Value::Error("unknown audio function");
    }
    std::unordered_map<std::string, Value> Members;
};
Sound* GetSound(const Value& Input, const SoundType& Type) {
    if (Input.GetType() != ValueType::Object) return nullptr;
    auto* Host = dynamic_cast<NativeObject*>(Input.AsObject());
    return Host && Host->GetNativeObjectType() == &Type ? static_cast<Sound*>(Host) : nullptr;
}
Value Create(const NativeModuleContext& Context) {
    auto& Vm = Context.Machine;
    auto& Sounds = Vm.CreateNativeObjectType<SoundType>(Guid, "Sound");
    auto& ModuleType = Vm.CreateNativeObjectType<NativeSingletonType>(Guid, "Module");
    auto Library = ModuleType.Create<Module>();
    auto Add = [&](const char* Name, Function::Body Action) {
        auto& Type = Vm.CreateNativeObjectType<NativeSingletonType>(Guid, Name);
        Library->Members.emplace(Name, Value::FromObject(Type.Create<Function>(std::move(Action))));
    };
    Add("Open", [&Sounds](const auto& Args) {
        if (Args.size() != 1 || Args[0].GetType() != ValueType::String)
            return Value::Error("audio.Open expects a path string");
        return Sounds.Open(Args[0].AsString());
    });
    auto Command = [&](const char* Name, Value (Sound::*Action)()) {
        Add(Name, [&Sounds, Action](const auto& Args) {
            if (Args.size() != 1) return Value::Error("audio command expects one sound");
            auto* Handle = GetSound(Args[0], Sounds);
            if (!Handle || Handle->IsClosed()) return Value::Error("invalid or closed audio sound");
            return (Handle->*Action)();
        });
    };
    Command("Play", &Sound::Play);
    Command("Pause", &Sound::Pause);
    Command("Stop", &Sound::Stop);
    Command("Close", &Sound::Close);
    Add("Seek", [&Sounds](const auto& Args) {
        if (Args.size() != 2 || !Args[1].IsNumeric()) return Value::Error("audio.Seek expects a sound and seconds");
        auto* Handle = GetSound(Args[0], Sounds);
        if (!Handle || Handle->IsClosed()) return Value::Error("invalid or closed audio sound");
        return Handle->Seek(Args[1].AsNumber());
    });
    return Value::FromObject(Library);
}
}

extern "C" FEATHER_NATIVE_EXPORT const Feather::NativeModuleDescriptor* FeatherNativeModule() {
    static const Feather::NativeModuleDescriptor Descriptor{Guid, "audio", &Create};
    return &Descriptor;
}
