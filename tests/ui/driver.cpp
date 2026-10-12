// Драйвер тестов интерфейса: запускает программу в псевдоконсоли (ConPTY),
// разбирает её VT-вывод в модель экрана и подаёт нажатия клавиш по сценарию.
//
// cabine-uidriver <сценарий>  — журнал пишется в <сценарий>.log, снимки экрана —
// рядом со сценарием. Команды сценария (строки с # пропускаются):
//   size <колонки> <строки>     размер псевдоконсоли (до run)
//   run <командная строка>      запуск программы
//   key <клавиша>               Enter, Esc, Tab, Down, F5, Alt+X, Ctrl+N...
//   type <текст>                ввод текста (UTF-8)
//   wait <мс>                   пауза
//   waitfor <мс> <текст>        ожидание текста на экране
//   dump <имя> / attr <имя>     снимок экрана (attr — с цветами)
//   exit <мс> [код]             ожидание завершения программы с кодом (по умолчанию 0)
// Код возврата: 0 — успешно, 3 — текст не найден, 4 — программа не завершилась,
// 5 — неверный код завершения программы.
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>

static std::string dir;
static FILE *logFile;
static HANDLE hProcess, inWrite, outRead;
static HPCON hpc;
static std::mutex mtx;
static int cols = 100, rows = 30;
static std::atomic<bool> win32Input{false};

struct Cell { wchar_t ch = L' '; unsigned char attr = 0x07; };
static std::vector<Cell> grid;
static int cx = 0, cy = 0;
static bool pendingWrap = false;
static unsigned char curFg = 7, curBg = 0;
static bool reverseVideo = false;

static std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- VT-разбор

static unsigned char ansiToBios(int c)
{
    static const unsigned char map[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return map[c & 7] | (c & 8);
}

static unsigned char xtermToBios(int n)
{
    if (n < 16)
        return ansiToBios(n);
    if (n >= 232)
        return n < 244 ? 8 : 7;
    n -= 16;
    int r = n / 36, g = (n / 6) % 6, b = n % 6;
    int bios = (r >= 3 ? 4 : 0) | (g >= 3 ? 2 : 0) | (b >= 3 ? 1 : 0);
    if (r >= 4 || g >= 4 || b >= 4)
        bios |= 8;
    return (unsigned char) bios;
}

static unsigned char rgbToBios(int r, int g, int b)
{
    int bios = (r >= 128 ? 4 : 0) | (g >= 128 ? 2 : 0) | (b >= 128 ? 1 : 0);
    if (r >= 192 || g >= 192 || b >= 192)
        bios |= 8;
    if (bios == 0 && (r > 64 || g > 64 || b > 64))
        bios = 8;
    return (unsigned char) bios;
}

static unsigned char curAttr()
{
    unsigned char fg = curFg, bg = curBg;
    if (reverseVideo)
        std::swap(fg, bg);
    return (unsigned char) ((bg << 4) | fg);
}

static Cell &at(int x, int y) { return grid[y * cols + x]; }

static void clearCells(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1 && y < rows; ++y)
        for (int x = (y == y0 ? x0 : 0); x < cols && (y < y1 || x <= x1); ++x)
        {
            at(x, y).ch = L' ';
            at(x, y).attr = curAttr();
        }
}

static void putChar(wchar_t ch)
{
    if (pendingWrap)
    {
        cx = 0;
        if (++cy >= rows)
            cy = rows - 1;
        pendingWrap = false;
    }
    if (cx < cols && cy < rows)
    {
        at(cx, cy).ch = ch;
        at(cx, cy).attr = curAttr();
    }
    if (++cx >= cols)
    {
        cx = cols - 1;
        pendingWrap = true;
    }
}

static void sgr(const std::vector<int> &p)
{
    if (p.empty())
    {
        curFg = 7; curBg = 0; reverseVideo = false;
        return;
    }
    for (size_t i = 0; i < p.size(); ++i)
    {
        int v = p[i];
        if (v == 0) { curFg = 7; curBg = 0; reverseVideo = false; }
        else if (v == 7) reverseVideo = true;
        else if (v == 27) reverseVideo = false;
        else if (v >= 30 && v <= 37) curFg = ansiToBios(v - 30);
        else if (v == 39) curFg = 7;
        else if (v >= 40 && v <= 47) curBg = ansiToBios(v - 40);
        else if (v == 49) curBg = 0;
        else if (v >= 90 && v <= 97) curFg = ansiToBios(v - 90 + 8);
        else if (v >= 100 && v <= 107) curBg = ansiToBios(v - 100 + 8);
        else if ((v == 38 || v == 48) && i + 1 < p.size())
        {
            unsigned char c = 0;
            if (p[i + 1] == 5 && i + 2 < p.size()) { c = xtermToBios(p[i + 2]); i += 2; }
            else if (p[i + 1] == 2 && i + 4 < p.size()) { c = rgbToBios(p[i + 2], p[i + 3], p[i + 4]); i += 4; }
            if (v == 38) curFg = c; else curBg = c;
        }
    }
}

static void csi(char final, const std::string &raw)
{
    bool priv = !raw.empty() && raw[0] == '?';
    std::vector<int> p;
    std::string num;
    for (size_t i = priv ? 1 : 0; i <= raw.size(); ++i)
    {
        if (i == raw.size() || raw[i] == ';')
        {
            p.push_back(num.empty() ? -1 : atoi(num.c_str()));
            num.clear();
        }
        else if (isdigit((unsigned char) raw[i]))
            num += raw[i];
    }
    auto arg = [&](size_t i, int def) { return i < p.size() && p[i] > 0 ? p[i] : def; };
    if (priv)
    {
        if ((final == 'h' || final == 'l') && !p.empty() && p[0] == 9001)
        {
            win32Input = final == 'h';
            fprintf(logFile, "  win32-input-mode: %d\n", final == 'h');
        }
        return;
    }
    pendingWrap = false;
    switch (final)
    {
        case 'H': case 'f':
            cy = std::min(rows - 1, arg(0, 1) - 1);
            cx = std::min(cols - 1, arg(1, 1) - 1);
            break;
        case 'A': cy = std::max(0, cy - arg(0, 1)); break;
        case 'B': cy = std::min(rows - 1, cy + arg(0, 1)); break;
        case 'C': cx = std::min(cols - 1, cx + arg(0, 1)); break;
        case 'D': cx = std::max(0, cx - arg(0, 1)); break;
        case 'G': cx = std::min(cols - 1, arg(0, 1) - 1); break;
        case 'd': cy = std::min(rows - 1, arg(0, 1) - 1); break;
        case 'J':
        {
            int m = p.empty() || p[0] < 0 ? 0 : p[0];
            if (m == 0) clearCells(cx, cy, cols - 1, rows - 1);
            else if (m == 1) clearCells(0, 0, cx, cy);
            else clearCells(0, 0, cols - 1, rows - 1);
            break;
        }
        case 'K':
        {
            int m = p.empty() || p[0] < 0 ? 0 : p[0];
            if (m == 0) clearCells(cx, cy, cols - 1, cy);
            else if (m == 1) clearCells(0, cy, cx, cy);
            else clearCells(0, cy, cols - 1, cy);
            break;
        }
        case 'X':
        {
            int n = arg(0, 1);
            for (int x = cx; x < cols && x < cx + n; ++x)
            {
                at(x, cy).ch = L' ';
                at(x, cy).attr = curAttr();
            }
            break;
        }
        case 'm':
        {
            std::vector<int> q;
            for (int v : p) q.push_back(v < 0 ? 0 : v);
            if (q.size() == 1 && p[0] < 0) q.clear();
            sgr(q);
            break;
        }
        default:
            break;
    }
}

static void feed(const char *data, size_t n)
{
    static int state = 0;   // 0 текст, 1 ESC, 2 CSI, 3 OSC, 4 OSC после ESC, 5 пропуск 1 символа
    static std::string params, utf8;
    std::lock_guard<std::mutex> lock(mtx);
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char) data[i];
        switch (state)
        {
            case 0:
                if (c == 0x1B) { state = 1; utf8.clear(); }
                else if (c == '\r') { cx = 0; pendingWrap = false; }
                else if (c == '\n') { if (++cy >= rows) cy = rows - 1; pendingWrap = false; }
                else if (c == '\b') { if (cx > 0) --cx; pendingWrap = false; }
                else if (c == 0x07 || c < 0x20) {}
                else
                {
                    utf8 += (char) c;
                    int need = (unsigned char) utf8[0] < 0x80 ? 1 : (unsigned char) utf8[0] < 0xE0 ? 2
                             : (unsigned char) utf8[0] < 0xF0 ? 3 : 4;
                    if ((int) utf8.size() >= need)
                    {
                        std::wstring w = widen(utf8);
                        for (wchar_t ch : w)
                            putChar(ch);
                        utf8.clear();
                    }
                }
                break;
            case 1:
                if (c == '[') { state = 2; params.clear(); }
                else if (c == ']') state = 3;
                else if (c == '(' || c == ')') state = 5;
                else state = 0;
                break;
            case 2:
                if (c >= 0x40 && c <= 0x7E) { csi((char) c, params); state = 0; }
                else params += (char) c;
                break;
            case 3:
                if (c == 0x07) state = 0;
                else if (c == 0x1B) state = 4;
                break;
            case 4:
                state = c == '\\' ? 0 : 3;
                break;
            case 5:
                state = 0;
                break;
        }
    }
}

static FILE *rawFile;
static std::atomic<long> rawBytes{0};

static void readerThread()
{
    char buf[8192];
    DWORD n;
    while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0)
    {
        if (rawFile)
        {
            fwrite(buf, 1, n, rawFile);
            fflush(rawFile);
        }
        rawBytes += n;
        feed(buf, n);
    }
}

// ---------------------------------------------------------------- ввод

static void writeInput(const std::string &s)
{
    DWORD n;
    WriteFile(inWrite, s.data(), (DWORD) s.size(), &n, nullptr);
}

static void sendWin32Key(WORD vk, wchar_t ch, DWORD state)
{
    WORD sc = vk ? (WORD) MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) : 0;
    char buf[128];
    for (int down = 1; down >= 0; --down)
    {
        snprintf(buf, sizeof(buf), "\x1b[%u;%u;%u;%d;%lu;1_", vk, sc, (unsigned) ch, down, state);
        writeInput(buf);
    }
}

struct KeyDef { const char *name; WORD vk; const char *vt; wchar_t ch; bool enh; };
static const KeyDef keys[] = {
    {"Enter", VK_RETURN, "\r", L'\r', false}, {"Esc", VK_ESCAPE, "\x1b", 0x1B, false},
    {"Tab", VK_TAB, "\t", L'\t', false}, {"Space", VK_SPACE, " ", L' ', false},
    {"Back", VK_BACK, "\x7f", 8, false},
    {"Up", VK_UP, "\x1b[A", 0, true}, {"Down", VK_DOWN, "\x1b[B", 0, true},
    {"Right", VK_RIGHT, "\x1b[C", 0, true}, {"Left", VK_LEFT, "\x1b[D", 0, true},
    {"Home", VK_HOME, "\x1b[H", 0, true}, {"End", VK_END, "\x1b[F", 0, true},
    {"Ins", VK_INSERT, "\x1b[2~", 0, true}, {"Del", VK_DELETE, "\x1b[3~", 0, true},
    {"PgUp", VK_PRIOR, "\x1b[5~", 0, true}, {"PgDn", VK_NEXT, "\x1b[6~", 0, true},
    {"Plus", VK_ADD, "+", L'+', false}, {"Minus", VK_SUBTRACT, "-", L'-', false},
    {"Star", VK_MULTIPLY, "*", L'*', false},
    {"F1", VK_F1, "\x1bOP", 0, false}, {"F2", VK_F2, "\x1bOQ", 0, false},
    {"F3", VK_F3, "\x1bOR", 0, false}, {"F4", VK_F4, "\x1bOS", 0, false},
    {"F5", VK_F5, "\x1b[15~", 0, false}, {"F6", VK_F6, "\x1b[17~", 0, false},
    {"F7", VK_F7, "\x1b[18~", 0, false}, {"F8", VK_F8, "\x1b[19~", 0, false},
    {"F9", VK_F9, "\x1b[20~", 0, false}, {"F10", VK_F10, "\x1b[21~", 0, false},
    {"F11", VK_F11, "\x1b[23~", 0, false}, {"F12", VK_F12, "\x1b[24~", 0, false},
};

static bool sendKey(std::string spec)
{
    DWORD state = 0;
    bool alt = false, ctrl = false, shift = false;
    for (;;)
    {
        if (spec.rfind("Alt+", 0) == 0) { alt = true; state |= LEFT_ALT_PRESSED; spec = spec.substr(4); }
        else if (spec.rfind("Ctrl+", 0) == 0) { ctrl = true; state |= LEFT_CTRL_PRESSED; spec = spec.substr(5); }
        else if (spec.rfind("Shift+", 0) == 0) { shift = true; state |= SHIFT_PRESSED; spec = spec.substr(6); }
        else break;
    }
    const KeyDef *k = nullptr;
    for (auto &d : keys)
        if (spec == d.name)
            k = &d;
    WORD vk = 0;
    wchar_t ch = 0;
    bool enh = false;
    std::string vt;
    if (k)
    {
        vk = k->vk; ch = k->ch; enh = k->enh; vt = k->vt;
    }
    else if (spec.size() == 1 && isalnum((unsigned char) spec[0]))
    {
        char c = (char) toupper((unsigned char) spec[0]);
        vk = (WORD) c;
        ch = (wchar_t) (shift ? c : tolower(c));
        if (ctrl && isalpha((unsigned char) c))
            ch = (wchar_t) (c - 'A' + 1);
        vt = std::string(1, (char) ch);
    }
    else
        return false;

    if (win32Input)
    {
        sendWin32Key(vk, ch, state | (enh ? ENHANCED_KEY : 0));
        return true;
    }
    // Запасной путь: VT-последовательности.
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    if (mod > 1 && vt.size() > 2 && vt[0] == 0x1b && (vt[1] == '[' || vt[1] == 'O'))
    {
        char last = vt.back();
        std::string body = vt.substr(2, vt.size() - 3);
        if (last == '~')
            vt = "\x1b[" + body + ";" + std::to_string(mod) + "~";
        else
            vt = "\x1b[1;" + std::to_string(mod) + last;
    }
    else if (alt)
        vt = "\x1b" + vt;
    writeInput(vt);
    return true;
}

static void typeText(const std::string &text)
{
    if (win32Input)
    {
        for (wchar_t c : widen(text))
        {
            sendWin32Key(0, c, 0);
            Sleep(20);
        }
    }
    else
        writeInput(text);
}

// ---------------------------------------------------------------- экран

static std::vector<std::wstring> screenLines(std::vector<std::string> *attrs)
{
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::wstring> lines;
    for (int y = 0; y < rows; ++y)
    {
        std::wstring l;
        std::string a;
        char hex[4];
        for (int x = 0; x < cols; ++x)
        {
            l += at(x, y).ch;
            snprintf(hex, sizeof(hex), "%02X", at(x, y).attr);
            a += hex;
        }
        lines.push_back(l);
        if (attrs)
            attrs->push_back(a);
    }
    return lines;
}

static void dump(const std::string &name, bool withAttr)
{
    std::vector<std::string> attrs;
    auto lines = screenLines(withAttr ? &attrs : nullptr);
    std::string path = dir + "\\" + name + (withAttr ? ".attr.txt" : ".txt");
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
    for (size_t i = 0; i < lines.size(); ++i)
    {
        fprintf(f, "%s\n", narrow(lines[i]).c_str());
        if (withAttr)
            fprintf(f, "%s\n", attrs[i].c_str());
    }
    fclose(f);
    fprintf(logFile, "dump %s (получено байт: %ld)\n", name.c_str(), (long) rawBytes);
}

static bool screenHas(const std::string &text)
{
    std::wstring w = widen(text);
    for (auto &l : screenLines(nullptr))
        if (l.find(w) != std::wstring::npos)
            return true;
    return false;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return 2;
    std::string script = narrow(argv[1]);
    dir = script.substr(0, script.find_last_of("\\/"));
    logFile = _wfopen(widen(script + ".log").c_str(), L"wb");
    std::ifstream in(widen(script));
    std::string line;
    int rc = 0;
    std::thread reader;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::string cmd = line.substr(0, line.find(' '));
        std::string arg = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
        fprintf(logFile, "> %s\n", line.c_str());
        fflush(logFile);
        if (cmd == "size")
            sscanf(arg.c_str(), "%d %d", &cols, &rows);
        else if (cmd == "run")
        {
            grid.assign(cols * rows, Cell());
            HANDLE inRead, outWrite;
            CreatePipe(&inRead, &inWrite, nullptr, 0);
            CreatePipe(&outRead, &outWrite, nullptr, 0);
            COORD size = {(SHORT) cols, (SHORT) rows};
            HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &hpc);
            fprintf(logFile, "CreatePseudoConsole: 0x%08lx\n", hr);
            CloseHandle(inRead);
            CloseHandle(outWrite);
            rawFile = _wfopen(widen(script + ".raw").c_str(), L"wb");
            reader = std::thread(readerThread);

            STARTUPINFOEXW si = {};
            si.StartupInfo.cb = sizeof(si);
            // Пустые стандартные дескрипторы заменяются дескрипторами псевдоконсоли;
            // иначе ребёнок получил бы перенаправленные дескрипторы драйвера.
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST) malloc(attrSize);
            InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                      hpc, sizeof(hpc), nullptr, nullptr);
            PROCESS_INFORMATION pi;
            std::wstring cl = widen(arg);
            if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                &si.StartupInfo, &pi))
            {
                fprintf(logFile, "CreateProcess: ошибка %lu\n", GetLastError());
                return 1;
            }
            hProcess = pi.hProcess;
            CloseHandle(pi.hThread);
        }
        else if (cmd == "wait")
            Sleep(atoi(arg.c_str()));
        else if (cmd == "key")
        {
            if (!sendKey(arg))
                fprintf(logFile, "неизвестная клавиша %s\n", arg.c_str());
            Sleep(150);
        }
        else if (cmd == "type")
        {
            typeText(arg);
            Sleep(150);
        }
        else if (cmd == "dump")
            dump(arg, false);
        else if (cmd == "attr")
            dump(arg, true);
        else if (cmd == "waitfor")
        {
            int ms = atoi(arg.c_str());
            std::string text = arg.substr(arg.find(' ') + 1);
            bool found = false;
            DWORD t0 = GetTickCount();
            while (!found && GetTickCount() - t0 < (DWORD) ms)
            {
                found = screenHas(text);
                if (!found)
                    Sleep(50);
            }
            DWORD elapsed = GetTickCount() - t0;
            Sleep(150);
            fprintf(logFile, "waitfor '%s': %s за %lu мс\n", text.c_str(), found ? "найдено" : "НЕ НАЙДЕНО", elapsed);
            if (!found)
                rc = 3;
        }
        else if (cmd == "exit")
        {
            int ms = 0, expected = 0;
            sscanf(arg.c_str(), "%d %d", &ms, &expected);
            DWORD r = WaitForSingleObject(hProcess, ms);
            DWORD code = 0;
            GetExitCodeProcess(hProcess, &code);
            fprintf(logFile, "exit: %s, код %lu\n", r == WAIT_OBJECT_0 ? "завершился" : "НЕ завершился", code);
            if (r != WAIT_OBJECT_0)
                rc = 4;
            else if ((int) code != expected)
            {
                fprintf(logFile, "exit: ожидался код %d\n", expected);
                rc = 5;
            }
        }
        fflush(logFile);
    }
    if (hProcess && WaitForSingleObject(hProcess, 0) != WAIT_OBJECT_0)
    {
        TerminateProcess(hProcess, 99);
        fprintf(logFile, "процесс принудительно завершён\n");
    }
    if (hpc)
        ClosePseudoConsole(hpc);
    if (reader.joinable())
        reader.detach();
    fclose(logFile);
    return rc;
}
