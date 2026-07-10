#include "platform/Dialogs.h"

#include <shobjidl.h>

namespace platform
{
    bool PickFolder(HWND owner, std::filesystem::path& out)
    {
        bool picked = false;

        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        IFileDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

            if (SUCCEEDED(dialog->Show(owner)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        out = std::filesystem::path(path);
                        CoTaskMemFree(path);
                        picked = true;
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(init))
            CoUninitialize();
        return picked;
    }
}
