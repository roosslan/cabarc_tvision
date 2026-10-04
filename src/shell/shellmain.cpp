// cabine-shell.exe — посредник между контекстным меню Проводника и cabine.exe.
//
// Проводник запускает команду пункта меню отдельно для каждого выделенного
// объекта. Первый запущенный экземпляр становится сборщиком: принимает пути
// от остальных через именованный канал, дожидается паузы в поступлении и один
// раз запускает cabine.exe со списком всех путей. Программа не имеет окна,
// поэтому консоль появляется только у cabine.exe.
//
// cabine-shell.exe -a <путь>   добавить в архив
// cabine-shell.exe -x <путь>   распаковать архив

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../fsutil.h"

namespace {

const DWORD kQuietMs = 700;         // пауза, после которой список считается полным
const DWORD kMaxCollectMs = 15000;
const DWORD kConnectMs = 3000;

std::wstring channelName(const std::string &mode)
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return std::wstring(L"Cabine.Shell") + (mode == "-a" ? L".Add." : L".Extract.") +
           std::to_wstring(session);
}

std::string joinLines(const std::vector<std::string> &paths)
{
    std::string data;
    for (const std::string &p : paths)
        data += p + "\n";
    return data;
}

void splitLines(const std::string &data, std::vector<std::string> &out)
{
    size_t start = 0;
    while (start < data.size())
    {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos)
            nl = data.size();
        if (nl > start)
            out.push_back(data.substr(start, nl - start));
        start = nl + 1;
    }
}

bool sendToCollector(const std::wstring &pipe, const std::vector<std::string> &paths)
{
    std::string data = joinLines(paths);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < kConnectMs)
    {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            bool ok = WriteFile(h, data.data(), (DWORD) data.size(), &written, nullptr) &&
                      written == data.size();
            CloseHandle(h);
            return ok;
        }
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeW(pipe.c_str(), 500);
        else
            Sleep(30);   // сборщик ещё не создал канал
    }
    return false;
}

class Collector
{
public:
    Collector(const std::wstring &pipe, std::vector<std::string> initial) :
        pipe(pipe),
        paths(std::move(initial)),
        last(GetTickCount())
    {
        server = std::thread([this] { serve(); });
    }

    // Ожидание паузы в поступлении путей.
    void waitQuiet(DWORD quietMs, DWORD maxMs)
    {
        DWORD start = GetTickCount();
        while (GetTickCount() - last < quietMs && GetTickCount() - start < maxMs)
            Sleep(50);
    }

    std::vector<std::string> finish()
    {
        stop = true;
        // Подключение к самому себе выводит поток из ConnectNamedPipe.
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        server.join();
        std::lock_guard<std::mutex> lock(mtx);
        return paths;
    }

private:
    void serve()
    {
        while (!stop)
        {
            HANDLE p = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_INBOUND,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                        PIPE_REJECT_REMOTE_CLIENTS,
                                        PIPE_UNLIMITED_INSTANCES, 0, 65536, 0, nullptr);
            if (p == INVALID_HANDLE_VALUE)
                return;
            bool connected = ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (connected && !stop)
            {
                std::string data;
                char buf[4096];
                DWORD n;
                while (ReadFile(p, buf, sizeof(buf), &n, nullptr) && n > 0)
                    data.append(buf, n);
                std::lock_guard<std::mutex> lock(mtx);
                splitLines(data, paths);
                last = GetTickCount();
            }
            DisconnectNamedPipe(p);
            CloseHandle(p);
        }
    }

    std::wstring pipe;
    std::mutex mtx;
    std::vector<std::string> paths;
    std::atomic<DWORD> last;
    std::atomic<bool> stop{false};
    std::thread server;
};

bool launchCabine(const std::string &mode, const std::vector<std::string> &paths)
{
    // Список передаётся файлом: командная строка ограничена 32767 символами.
    wchar_t dir[MAX_PATH + 1], file[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH + 1, dir) || !GetTempFileNameW(dir, L"cab", 0, file))
        return false;
    FILE *f = _wfopen(file, L"wb");
    if (!f)
        return false;
    std::string data = joinLines(paths);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);

    std::string exe = fsu::joinPath(fsu::dirName(fsu::exePath()), "cabine.exe");
    std::wstring cmd = L"\"" + fsu::widen(exe) + L"\" " + fsu::widen(mode) + L" \"@" + file + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        nullptr, &si, &pi))
    {
        DeleteFileW(file);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    std::vector<std::string> args = fsu::commandLineArgs();
    if (args.size() < 2 || (args[0] != "-a" && args[0] != "-x"))
    {
        MessageBoxW(nullptr,
                    L"cabine-shell.exe вызывается из контекстного меню Проводника:\n"
                    L"cabine-shell.exe -a <путь>  — добавить в CAB-архив\n"
                    L"cabine-shell.exe -x <путь>  — распаковать CAB-архив",
                    L"Cabine", MB_ICONINFORMATION);
        return 2;
    }
    std::string mode = args[0];
    std::vector<std::string> paths(args.begin() + 1, args.end());
    std::wstring name = channelName(mode);
    std::wstring pipe = L"\\\\.\\pipe\\" + name;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\" + name).c_str());
    bool collector = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!collector)
    {
        if (sendToCollector(pipe, paths))
            return 0;
        // Сборщик уже завершил приём — пути обрабатываются самостоятельно.
        if (mutex)
            CloseHandle(mutex);
        mutex = nullptr;
    }

    Collector c(pipe, paths);
    c.waitQuiet(kQuietMs, kMaxCollectMs);
    // Новые запуски начинают новый сбор; уже подключающиеся ещё принимаются.
    if (mutex)
        CloseHandle(mutex);
    c.waitQuiet(200, 2000);
    std::vector<std::string> all = c.finish();

    if (!launchCabine(mode, all))
    {
        MessageBoxW(nullptr, L"Не удалось запустить cabine.exe", L"Cabine", MB_ICONERROR);
        return 1;
    }
    return 0;
}
