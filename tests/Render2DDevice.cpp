#include "../modules/render2d/Render2D.hpp"
#include <iostream>
#include <bgfx/bgfx.h>
#include <stdexcept>
using namespace Feather;
int main(int Argc, char** Argv) {
    try {
        if (Argc != 3) throw std::runtime_error("texture and font paths required");
        auto Host = CreateBgfxRender2DHost();
        Render2DConfig Config; Config.Title = "Feather resource lifecycle test";
        Config.WindowWidth = 320; Config.WindowHeight = 240; Config.VSync = false;
        auto Context = Host->Create(Config);
        auto Texture = Context->LoadTexture(Argv[1]);
        auto Font = Context->LoadFont(Argv[2]);
        Context->BeginRender({});
        Context->DrawTexture(*Texture, {0,0,100,100}, {});
        Context->DrawText(*Font, "Lifecycle", 0, 100, 24, {});
        Context->DrawLine(0, 40, 200, 40, 4, {1,0,0,0.5f});
        Context->DrawLine(60, 0, 60, 200, 8, {0,1,0,1});
        Context->DrawLine(0, 0, 200, 200, 2, {0,0,1,1});
        Context->DrawLine(1, 1, 1, 1, 1, {});
        Context->EndRender();
        Context->Close(); Context->Close();
        bool ClosedRejected = false;
        try { Context->BeginRender({}); } catch (const std::exception&) { ClosedRejected = true; }
        if (!ClosedRejected) throw std::runtime_error("closed context accepted rendering");
        // A new device may reuse handle indices; old wrappers must not destroy them.
        auto Next = Host->Create(Config);
        for (int I = 0; I < 3; ++I) { Next->BeginRender({}); Next->EndRender(); }
        auto BaselineTextures = bgfx::getStats()->numTextures;
        auto NextTexture = Next->LoadTexture(Argv[1]);
        auto NextFont = Next->LoadFont(Argv[2]);
        Texture.reset(); Font.reset(); Context.reset();
        Next->BeginRender({});
        Next->DrawTexture(*NextTexture, {0,0,100,100}, {});
        Next->DrawText(*NextFont, "Normal", 0, 100, 24, {});
        Next->EndRender();
        // Normal release while the owning device is still alive.
        NextTexture.reset(); NextFont.reset();
        Next->BeginRender({}); Next->EndRender();
        for (int I = 0; I < 3; ++I) { Next->BeginRender({}); Next->EndRender(); }
        if (bgfx::getStats()->numTextures != BaselineTextures)
            throw std::runtime_error("image or font GPU texture survived normal release");
        Next->Close(); Next->Close(); Next.reset();
        std::cout << "Render device lifecycle tests passed\n";
    } catch (const std::exception& E) { std::cerr << E.what() << '\n'; return 1; }
}
