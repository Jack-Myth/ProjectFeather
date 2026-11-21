#include <cstdint>
#include <cstring>
#include <thread>

#define EXPORT extern "C" __declspec(dllexport)

EXPORT double Mixed(int8_t A, float B, int16_t C, double D, uint32_t E) {
    return A + B + C + D + E;
}
EXPORT int32_t Update(uint32_t* A, uint32_t* B) {
    *A += 3;
    return A == B ? static_cast<int32_t>(*B) : -1;
}
EXPORT void Fill(uint8_t* A, uint8_t* B) {
    A[0] = 123;
    B[1] = A == B ? 255 : 0;
}
EXPORT int Strings(const char* A, const wchar_t* B) {
    return std::strcmp(A, "\xe9\x9b\xaa\xf0\x9f\x98\x80") == 0 &&
        B[0] == 0x96ea && B[1] == 0xd83d && B[2] == 0xde00 && B[3] == 0;
}

EXPORT int32_t InvokeCallback(int32_t (*Callback)(int32_t), int32_t Worker) {
    int32_t Result = 0;
    if (Worker) {
        std::thread Thread([&] { Result = Callback(4); });
        Thread.join();
    } else Result = Callback(4);
    return Result;
}