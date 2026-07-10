#pragma once

#include <filesystem>

#include <windows.h>

namespace platform
{
    // Shows the native Windows folder picker. Returns true and fills `out` when
    // the user picks a folder; false if they cancel.
    bool PickFolder(HWND owner, std::filesystem::path& out);
}
