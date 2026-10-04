#include <windows.h>
#include <shlobj.h>

#include "fsutil.h"
#include "shellreg.h"

namespace {

const wchar_t *const kRoot = L"Software\\Classes\\";
const wchar_t *const kAddKey = L"AllFilesystemObjects\\shell\\Cabine.Add";
const wchar_t *const kExtractKey = L"SystemFileAssociations\\.cab\\shell\\Cabine.Extract";
const wchar_t *const kOpenKey = L"SystemFileAssociations\\.cab\\shell\\Cabine.Open";

std::wstring quoted(const std::string &path)
{
    return L"\"" + fsu::widen(path) + L"\"";
}

bool setValue(HKEY key, const wchar_t *name, const std::wstring &value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *) value.c_str(),
                          DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool writeVerb(const wchar_t *subKey, const wchar_t *title, const std::wstring &icon,
               const std::wstring &command, bool multiSelect, std::string &err)
{
    std::wstring path = std::wstring(kRoot) + subKey;
    HKEY key, cmd;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
    {
        err = "Не удалось создать раздел реестра HKCU\\" + fsu::narrow(path);
        return false;
    }
    bool ok = setValue(key, L"MUIVerb", title) && setValue(key, L"Icon", icon);
    // Player: пункт виден при выделении до 100 объектов (для Document — до 15).
    // Программа всё равно запускается для каждого объекта отдельно.
    if (ok && multiSelect)
        ok = setValue(key, L"MultiSelectModel", L"Player");
    if (ok)
        ok = RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmd,
                             nullptr) == ERROR_SUCCESS;
    if (ok)
    {
        ok = setValue(cmd, nullptr, command);
        RegCloseKey(cmd);
    }
    RegCloseKey(key);
    if (!ok)
        err = "Не удалось записать раздел реестра HKCU\\" + fsu::narrow(path);
    return ok;
}

bool deleteVerb(const wchar_t *subKey, std::string &err)
{
    std::wstring path = std::wstring(kRoot) + subKey;
    LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (r == ERROR_SUCCESS)
        r = RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
    {
        err = "Не удалось удалить раздел реестра HKCU\\" + fsu::narrow(path);
        return false;
    }
    return true;
}

} // namespace

bool registerShellMenu(const std::string &cabineExe, const std::string &shellExe, std::string &err)
{
    std::wstring icon = quoted(cabineExe) + L",0";
    bool ok =
        writeVerb(kAddKey, L"Добавить в CAB-архив", icon,
                  quoted(shellExe) + L" -a \"%1\"", true, err) &&
        writeVerb(kExtractKey, L"Распаковать здесь", icon,
                  quoted(shellExe) + L" -x \"%1\"", true, err) &&
        writeVerb(kOpenKey, L"Открыть в Cabine", icon,
                  quoted(cabineExe) + L" \"%1\"", false, err);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool unregisterShellMenu(std::string &err)
{
    bool ok = deleteVerb(kAddKey, err) && deleteVerb(kExtractKey, err) && deleteVerb(kOpenKey, err);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}
