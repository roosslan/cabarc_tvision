#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstdio>
#include <memory>

#include "fsutil.h"

namespace fsu {

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string fromCodePage(const std::string &s, unsigned codePage)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(codePage, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(codePage, 0, s.data(), (int) s.size(), &w[0], n);
    return narrow(w);
}

std::string upper(const std::string &s)
{
    std::wstring w = widen(s);
    if (!w.empty())
        CharUpperBuffW(&w[0], (DWORD) w.size());
    return narrow(w);
}

int compareNatural(const std::wstring &a, const std::wstring &b)
{
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                            a.data(), (int) a.size(), b.data(), (int) b.size(),
                            nullptr, nullptr, 0);
    return r == 0 ? a.compare(b) : r - CSTR_EQUAL;
}

bool isAscii(const std::string &s)
{
    for (unsigned char c : s)
        if (c >= 0x80)
            return false;
    return true;
}

bool isValidUtf8(const std::string &s)
{
    return s.empty() ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int) s.size(), nullptr, 0) > 0;
}

static DWORD attributes(const std::string &path)
{
    return GetFileAttributesW(widen(path).c_str());
}

bool fileExists(const std::string &path)
{
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string &path)
{
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::string &path)
{
    if (path.empty() || dirExists(path))
        return true;
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !makeDirs(parent))
        return false;
    return CreateDirectoryW(widen(path).c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeFile(const std::string &path)
{
    std::wstring w = widen(path);
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(w.c_str()) != FALSE;
}

bool removeTree(const std::string &path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(joinPath(path, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..")
                continue;
            std::string child = joinPath(path, narrow(n));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // Точки соединения удаляются как ссылки, без захода внутрь.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(widen(child).c_str());
                else
                    removeTree(child);
            }
            else
                removeFile(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(widen(path).c_str()) != FALSE;
}

bool moveReplace(const std::string &from, const std::string &to)
{
    if (fileExists(to))
        clearReadOnly(to);
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED |
                       MOVEFILE_WRITE_THROUGH) != FALSE;
}

void clearReadOnly(const std::string &path)
{
    std::wstring w = widen(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
}

uint64_t fileSize(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (uint64_t(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

bool readFile(const std::string &path, std::string &data, uint64_t maxSize)
{
    uint64_t size = fileSize(path);
    if (size > maxSize)
        return false;
    FILE *f = _wfopen(widen(path).c_str(), L"rb");
    if (!f)
        return false;
    data.resize((size_t) size);
    size_t n = size ? fread(&data[0], 1, data.size(), f) : 0;
    fclose(f);
    data.resize(n);
    return true;
}

std::string currentDir()
{
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring w(n, L'\0');
    n = GetCurrentDirectoryW(n, &w[0]);
    w.resize(n);
    return narrow(w);
}

bool setCurrentDir(const std::string &path)
{
    return SetCurrentDirectoryW(widen(path).c_str()) != FALSE;
}

std::string fullPath(const std::string &path)
{
    std::wstring w = widen(path);
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0)
        return path;
    std::wstring r(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, &r[0], nullptr);
    r.resize(n);
    return narrow(r);
}

static bool isSep(char c)
{
    return c == '\\' || c == '/';
}

std::string joinPath(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    if (name.empty())
        return dir;
    if (isSep(dir.back()))
        return dir + name;
    return dir + '\\' + name;
}

std::string dirName(const std::string &path)
{
    std::string p = path;
    // Завершающий разделитель не считается отдельным компонентом,
    // кроме корня диска ("C:\").
    while (p.size() > 1 && isSep(p.back()) && !(p.size() == 3 && p[1] == ':'))
        p.pop_back();
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos)
        return (p.size() >= 2 && p[1] == ':') ? p.substr(0, 2) : std::string();
    if (pos == 0)
        return p.substr(0, 1);
    if (pos == 2 && p[1] == ':')
        return p.substr(0, 3);
    return p.substr(0, pos);
}

std::string baseName(const std::string &path)
{
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string extension(const std::string &name)
{
    std::string base = baseName(name);
    size_t pos = base.rfind('.');
    if (pos == std::string::npos || pos == 0)
        return {};
    return base.substr(pos + 1);
}

std::string stripExt(const std::string &name)
{
    size_t sep = name.find_last_of("\\/");
    size_t pos = name.rfind('.');
    if (pos == std::string::npos || (sep != std::string::npos && pos < sep) ||
        pos == (sep == std::string::npos ? 0 : sep + 1))
        return name;
    return name.substr(0, pos);
}

std::string withSlash(const std::string &dir)
{
    if (dir.empty() || isSep(dir.back()))
        return dir;
    return dir + '\\';
}

std::string safeRelPath(const std::string &name)
{
    std::string result, part;
    auto flush = [&]() {
        if (!part.empty() && part != "." && part != "..")
        {
            // Двоеточие (указание диска) и прочие недопустимые символы.
            for (char &c : part)
                if (c == ':' || c == '*' || c == '?' || c == '"' ||
                    c == '<' || c == '>' || c == '|' || (unsigned char) c < 0x20)
                    c = '_';
            result = joinPath(result, part);
        }
        part.clear();
    };
    size_t start = 0;
    if (name.size() >= 2 && name[1] == ':')
        start = 2;
    for (size_t i = start; i < name.size(); ++i)
    {
        if (isSep(name[i]))
            flush();
        else
            part += name[i];
    }
    flush();
    return result.empty() ? std::string("_") : result;
}

std::string makeTempDir()
{
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH)
        return {};
    std::string base = narrow(tmp);
    for (unsigned i = 0; i < 1000; ++i)
    {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "Cabine-%lu-%lu",
                 GetCurrentProcessId(), (unsigned long) (GetTickCount() + i));
        std::string dir = joinPath(base, suffix);
        if (CreateDirectoryW(widen(dir).c_str(), nullptr))
            return dir;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    return {};
}

std::string lastErrorText()
{
    DWORD code = GetLastError();
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR) &buf, 0, nullptr);
    std::string text = buf ? narrow(buf) : "код ошибки " + std::to_string(code);
    if (buf)
        LocalFree(buf);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == '.' || text.back() == ' '))
        text.pop_back();
    return text;
}

static bool globMatch(const wchar_t *p, const wchar_t *s)
{
    const wchar_t *star = nullptr, *afterStar = nullptr;
    while (*s)
    {
        if (*p == L'?' || (*p != L'*' && *p == *s))
        {
            ++p;
            ++s;
        }
        else if (*p == L'*')
        {
            star = p++;
            afterStar = s;
        }
        else if (star)
        {
            p = star + 1;
            s = ++afterStar;
        }
        else
            return false;
    }
    while (*p == L'*')
        ++p;
    return *p == 0;
}

bool wildMatch(const std::string &masks, const std::string &name)
{
    std::wstring n = widen(upper(name));
    std::string mask;
    auto test = [&]() -> bool {
        while (!mask.empty() && mask.back() == ' ')
            mask.pop_back();
        size_t lead = mask.find_first_not_of(' ');
        std::string m = lead == std::string::npos ? std::string() : mask.substr(lead);
        mask.clear();
        if (m.empty())
            return false;
        if (m == "*" || m == "*.*")
            return true;
        std::wstring wm = widen(upper(m));
        // "*." в Windows означает «имя без расширения».
        if (wm.back() == L'.' && n.find(L'.') == std::wstring::npos)
            return globMatch(wm.c_str(), (n + L'.').c_str());
        return globMatch(wm.c_str(), n.c_str());
    };
    for (char c : masks)
    {
        if (c == ';' || c == ',')
        {
            if (test())
                return true;
        }
        else
            mask += c;
    }
    return test();
}

bool listDirectory(const std::string &dir, std::vector<DirItem> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(widen(joinPath(dir, "*")).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;  // пустой корень диска
    const DWORD hiddenSystem = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    do
    {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & hiddenSystem) == hiddenSystem)
            continue;
        DirItem item;
        item.name = narrow(n);
        item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.attribs = (uint16_t) (fd.dwFileAttributes & 0xFFFF);
        FILETIME local;
        WORD date = 0, time = 0;
        if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local) &&
            FileTimeToDosDateTime(&local, &date, &time))
        {
            item.date = date;
            item.time = time;
        }
        out.push_back(std::move(item));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

uint64_t freeSpace(const std::string &dir)
{
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(widen(withSlash(dir)).c_str(), &avail, nullptr, nullptr))
        return 0;
    return avail.QuadPart;
}

std::string existingDir(const std::string &dir)
{
    std::string d = dir.empty() ? currentDir() : fullPath(dir);
    while (!dirExists(d) && !isRootDir(d))
        d = dirName(d);
    return dirExists(d) ? d : currentDir();
}

std::vector<std::string> logicalDrives()
{
    std::vector<std::string> drives;
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(512, buf);
    if (n > 0 && n < 512)
        for (const wchar_t *p = buf; *p; p += wcslen(p) + 1)
            drives.push_back(narrow(p));
    return drives;
}

bool isRootDir(const std::string &dir)
{
    return dirName(dir) == dir || (dir.size() <= 3 && dir.size() >= 2 && dir[1] == ':');
}

bool isAbsolutePath(const std::string &path)
{
    return (path.size() >= 2 && path[1] == ':') || (!path.empty() && isSep(path[0]));
}

static void walk(const std::string &dir, const std::string &prefix, const std::string &mask,
                 bool recurse, bool keepPaths, std::vector<FoundFile> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(widen(joinPath(dir, "*")).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::string> subdirs;
    do
    {
        std::wstring wn = fd.cFileName;
        if (wn == L"." || wn == L"..")
            continue;
        std::string n = narrow(wn);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // Точки соединения пропускаются, чтобы не уйти в цикл.
            if (recurse && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                subdirs.push_back(n);
        }
        else if (wildMatch(mask, n))
            out.push_back({joinPath(dir, n), keepPaths ? joinPath(prefix, n) : n});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::string &s : subdirs)
        walk(joinPath(dir, s), joinPath(prefix, s), mask, recurse, keepPaths, out);
}

bool collectFiles(const std::string &item, bool recurse, bool keepPaths,
                  std::vector<FoundFile> &out, std::string &err)
{
    std::string path = fullPath(item);
    while (path.size() > 3 && isSep(path.back()))
        path.pop_back();
    if (dirExists(path))
    {
        walk(path, keepPaths ? baseName(path) : std::string(), "*", recurse, keepPaths, out);
        return true;
    }
    std::string mask = baseName(path);
    if (mask.find_first_of("*?") != std::string::npos)
    {
        std::string dir = dirName(path);
        if (!dirExists(dir))
        {
            err = "Не найдена директория: " + dir;
            return false;
        }
        walk(dir, std::string(), mask, recurse, keepPaths, out);
        return true;
    }
    if (fileExists(path))
    {
        out.push_back({path, baseName(path)});
        return true;
    }
    err = "Не найден файл: " + path;
    return false;
}

bool shellOpen(const std::string &path, std::string &err)
{
    static const bool comReady = SUCCEEDED(
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    (void) comReady;
    std::wstring w = widen(path), dir = widen(dirName(path));
    INT_PTR r = (INT_PTR) ShellExecuteW(nullptr, L"open", w.c_str(), nullptr,
                                        dir.c_str(), SW_SHOWNORMAL);
    if (r > 32)
        return true;
    if (r == SE_ERR_NOASSOC)
        err = "Нет программы, сопоставленной с этим типом файлов";
    else
        err = "Не удалось открыть файл: " + lastErrorText();
    return false;
}

std::vector<std::string> commandLineArgs()
{
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
            args.push_back(narrow(argv[i]));
        LocalFree(argv);
    }
    return args;
}

std::string exePath()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD) buf.size());
        if (n < buf.size())
        {
            buf.resize(n);
            return narrow(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

void printConsole(const std::string &text)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, n;
    std::wstring w = widen(text);
    if (GetConsoleMode(out, &mode))
        WriteConsoleW(out, w.c_str(), (DWORD) w.size(), &n, nullptr);
    else
        WriteFile(out, text.data(), (DWORD) text.size(), &n, nullptr);
}

bool recycle(const std::vector<std::string> &paths, std::string &err)
{
    if (paths.empty())
        return true;
    // Список путей, разделённых нулями, с двумя нулями в конце.
    std::wstring from;
    for (const std::string &p : paths)
    {
        from += widen(fullPath(p));
        from += L'\0';
    }
    from += L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;
    int r = SHFileOperationW(&op);
    std::string left;
    for (const std::string &p : paths)
        if (fileExists(p) || dirExists(p))
            left += "\n" + p;
    if (r == 0 && !op.fAnyOperationsAborted && left.empty())
        return true;
    err = "Не удалось удалить" + (left.empty() ? std::string(" (код " + std::to_string(r) + ")") : ":" + left);
    return false;
}

static std::wstring settingsFile()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::string dir = joinPath(narrow(buf), "Cabine");
    makeDirs(dir);
    std::string ini = joinPath(dir, "cabine.ini");
    if (!fileExists(ini))
    {
        // Файл в UTF-16 с BOM: тогда Private Profile API хранит строки в Юникоде.
        if (FILE *f = _wfopen(widen(ini).c_str(), L"wb"))
        {
            fputc(0xFF, f);
            fputc(0xFE, f);
            fclose(f);
        }
    }
    return widen(ini);
}

std::string loadSetting(const std::string &key, const std::string &def)
{
    std::wstring ini = settingsFile();
    if (ini.empty())
        return def;
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"Cabine", widen(key).c_str(), widen(def).c_str(),
                             buf, 1024, ini.c_str());
    return narrow(buf);
}

void saveSetting(const std::string &key, const std::string &value)
{
    std::wstring ini = settingsFile();
    if (!ini.empty())
        WritePrivateProfileStringW(L"Cabine", widen(key).c_str(), widen(value).c_str(),
                                   ini.c_str());
}

} // namespace fsu
