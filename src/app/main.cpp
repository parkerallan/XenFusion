#include "app/EngineApplication.h"

int main(int, char**)
{
    EngineApplication app;
    if (!app.Init())
    {
        app.Shutdown();
        return 1;
    }

    app.RunLoop();
    app.Shutdown();
    return 0;
}
