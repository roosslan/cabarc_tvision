#include <windows.h>
#include <shlobj.h>

#include "fsutil.h"
#include "shellreg.h"

namespace {

const wchar_t *const kRoot = L"Software\\Classes\\";

// Подменю «Cabine» для всех файлов и директорий. Порядок пунктов задаётся
// именами разделов; пункты для архива показываются только у файлов .cab.
const wchar_t *const kMenuKey = L"AllFilesystemObjects\\shell\\Cabine";
const wchar_t *const kOpenKey = L"AllFilesystemObjects\\shell\\Cabine\\shell\\01open";
const wchar_t *const kExtractKey = L"AllFilesystemObjects\\shell\\Cabine\\shell\\02extract";
const wchar_t *const kAddKey = L"AllFilesystemObjects\\shell\\Cabine\\shell\\03add";
const wchar_t *const kOnlyCab = L"System.FileExtension:=.cab";

// Отдельные пункты прежних версий: удаляются при регистрации и её отмене.
const wchar_t *const kLegacyKeys[] = {
    L"AllFilesystemObjects\\shell\\Cabine.Add",
    L"SystemFileAssociations\\.cab\\shell\\Cabine.Extract",
    L"SystemFileAssociations\\.cab\\shell\\Cabine.Open",
};

std::wstring quoted(const std::string &path)
{
    return L"\"" + fsu::widen(path) + L"\"";
}

bool setValue(HKEY key, const wchar_t *name, const std::wstring &value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *) value.c_str(),
                          DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

struct Verb
{
    const wchar_t *subKey;
    const wchar_t *title;
    std::wstring icon;
    std::wstring command;           // пустая — подменю (SubCommands)
    const wchar_t *selectModel;     // MultiSelectModel
    const wchar_t *appliesTo;       // условие показа или nullptr
};

bool writeVerb(const Verb &v, std::string &err)
{
    std::wstring path = std::wstring(kRoot) + v.subKey;
    HKEY key, cmd;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
    {
        err = "Не удалось создать раздел реестра HKCU\\" + fsu::narrow(path);
        return false;
    }
    // Player: пункт виден при выделении до 100 объектов (для Document — до 15),
    // Single — только для одного объекта. Программа всё равно запускается для
    // каждого объекта отдельно.
    bool ok = setValue(key, L"MUIVerb", v.title) && setValue(key, L"Icon", v.icon) &&
              setValue(key, L"MultiSelectModel", v.selectModel);
    if (ok && v.appliesTo)
        ok = setValue(key, L"AppliesTo", v.appliesTo);
    if (ok && v.command.empty())
        ok = setValue(key, L"SubCommands", L"");
    else if (ok)
    {
        ok = RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmd,
                             nullptr) == ERROR_SUCCESS;
        if (ok)
        {
            ok = setValue(cmd, nullptr, v.command);
            RegCloseKey(cmd);
        }
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

bool deleteLegacy(std::string &err)
{
    for (const wchar_t *k : kLegacyKeys)
        if (!deleteVerb(k, err))
            return false;
    return true;
}

} // namespace

bool registerShellMenu(const std::string &cabineExe, const std::string &shellExe, std::string &err)
{
    std::wstring icon = quoted(cabineExe) + L",0";
    // Подменю пересоздаётся целиком: в нём не остаётся пунктов прежней регистрации.
    bool ok = deleteLegacy(err) && deleteVerb(kMenuKey, err) &&
        writeVerb({kMenuKey, L"Cabine", icon, L"", L"Player", nullptr}, err) &&
        writeVerb({kOpenKey, L"Открыть в Cabine", icon,
                   quoted(cabineExe) + L" \"%1\"", L"Single", kOnlyCab}, err) &&
        writeVerb({kExtractKey, L"Распаковать здесь", icon,
                   quoted(shellExe) + L" -x \"%1\"", L"Player", kOnlyCab}, err) &&
        writeVerb({kAddKey, L"Добавить в CAB-архив", icon,
                   quoted(shellExe) + L" -a \"%1\"", L"Player", nullptr}, err);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

namespace {

const wchar_t *const kProgId = L"Cabine.cab";
const wchar_t *const kExtKey = L"Software\\Classes\\.cab";
const wchar_t *const kPrevValue = L"Cabine.Previous";

bool readString(HKEY root, const std::wstring &path, const wchar_t *name, std::wstring &value)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf), type;
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
        return false;
    value = buf;
    return true;
}

bool setKeyValue(const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = setValue(key, name, value);
    RegCloseKey(key);
    return ok;
}

} // namespace

bool isCabAssociated()
{
    std::wstring value;
    return readString(HKEY_CURRENT_USER, kExtKey, nullptr, value) && value == kProgId;
}

bool cabUserChoiceOverrides()
{
    std::wstring value;
    return readString(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.cab\\UserChoice",
                      L"ProgId", value) && value != kProgId;
}

bool setCabAssociation(bool enable, const std::string &cabineExe, std::string &err)
{
    std::wstring progKey = std::wstring(kRoot) + kProgId;
    if (enable)
    {
        std::wstring exe = quoted(cabineExe);
        std::wstring previous;
        bool ok =
            setKeyValue(progKey, nullptr, L"CAB-архив") &&
            setKeyValue(progKey + L"\\DefaultIcon", nullptr, exe + L",0") &&
            setKeyValue(progKey + L"\\shell", nullptr, L"open") &&
            setKeyValue(progKey + L"\\shell\\open", L"MUIVerb", L"Открыть в Cabine") &&
            setKeyValue(progKey + L"\\shell\\open\\command", nullptr, exe + L" \"%1\"");
        // Прежняя программа запоминается, чтобы вернуть её при отключении.
        if (ok && readString(HKEY_CURRENT_USER, kExtKey, nullptr, previous) && previous != kProgId)
            ok = setKeyValue(kExtKey, kPrevValue, previous);
        ok = ok && setKeyValue(kExtKey, nullptr, kProgId) &&
             setKeyValue(std::wstring(kExtKey) + L"\\OpenWithProgids", kProgId, L"");
        if (!ok)
            err = "Не удалось записать ассоциацию .cab в реестр (HKCU\\Software\\Classes)";
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return ok;
    }

    HKEY ext;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kExtKey, 0, KEY_READ | KEY_WRITE, &ext) == ERROR_SUCCESS)
    {
        std::wstring current, previous;
        if (readString(HKEY_CURRENT_USER, kExtKey, nullptr, current) && current == kProgId)
        {
            if (readString(HKEY_CURRENT_USER, kExtKey, kPrevValue, previous))
                setValue(ext, nullptr, previous);
            else
                RegDeleteValueW(ext, nullptr);
        }
        RegDeleteValueW(ext, kPrevValue);
        HKEY owp;
        if (RegOpenKeyExW(ext, L"OpenWithProgids", 0, KEY_WRITE, &owp) == ERROR_SUCCESS)
        {
            RegDeleteValueW(owp, kProgId);
            RegCloseKey(owp);
        }
        RegCloseKey(ext);
    }
    LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, progKey.c_str());
    if (r == ERROR_SUCCESS)
        RegDeleteKeyW(HKEY_CURRENT_USER, progKey.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (isCabAssociated())
    {
        err = "Не удалось снять ассоциацию .cab в реестре (HKCU\\Software\\Classes)";
        return false;
    }
    return true;
}

bool unregisterShellMenu(std::string &err)
{
    bool ok = deleteVerb(kMenuKey, err) && deleteLegacy(err);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}
