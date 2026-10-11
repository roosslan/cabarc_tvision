#include <windows.h>

#include "cabfile.h"
#include "dialogs.h"
#include "fsutil.h"
#include "sfx/sfxres.h"
#include "sfxstub.h"

bool sfxStub(std::string &data, std::string &err)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SFX_STUB), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    const void *p = h ? LockResource(h) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!p || size == 0)
    {
        err = "В программе отсутствует распаковщик для самораспаковывающихся архивов";
        return false;
    }
    data.assign(static_cast<const char *>(p), size);
    return true;
}

std::string sfxPath(const std::string &path)
{
    std::string ext = fsu::upper(fsu::extension(path));
    if (ext == "EXE")
        return path;
    if (ext == "CAB")
        return fsu::stripExt(path) + ".exe";
    return path + ".exe";
}

bool prepareSfx(std::string &path, std::string &stub)
{
    std::string exe = sfxPath(path);
    if (fsu::dirExists(exe))
    {
        showError("Существует директория с таким именем\n" + exe);
        return false;
    }
    // Замена файла с исходным именем уже подтверждена раньше.
    if (exe != path && fsu::fileExists(exe) &&
        !confirm("Файл уже существует:\n" + exe + "\n\nЗаменить его новым архивом?"))
        return false;
    std::string err;
    if (!sfxStub(stub, err))
    {
        showError(err);
        return false;
    }
    path = exe;
    return true;
}

bool confirmVolumes(const std::string &firstVolume, uint64_t volumeSize)
{
    std::string second = cabVolumeName(firstVolume, 2);
    if (!volumeSize || !fsu::fileExists(second))
        return true;
    return confirm("Рядом уже есть тома с такими же именами (" + fsu::baseName(second) +
                   " и следующие).\nОни будут заменены. Продолжить?");
}
