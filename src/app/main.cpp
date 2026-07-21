#include "app/EngineApplication.h"

// Debug links as a console app (log/stdout visible); Release links as a GUI
// app with /ENTRY:mainCRTStartup so this same main() serves both.
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
