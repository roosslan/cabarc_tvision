// Тесты ядра Cabine без интерфейса: работа с CAB (cabfile), файловая
// система (fsutil), форматирование (format). Запускаются через CTest.
// Код возврата 0 — все проверки пройдены.

#include <cstdio>
#include <string>
#include <vector>

#include "../src/cabfile.h"
#include "../src/format.h"
#include "../src/fsutil.h"

#include <windows.h>

namespace {

int failures = 0;
int checks = 0;

void check(bool cond, const std::string &what)
{
    ++checks;
    if (!cond)
    {
        ++failures;
        fsu::printConsole("ОШИБКА: " + what + "\n");
    }
    else
        fsu::printConsole("ok: " + what + "\n");
}

void section(const std::string &name)
{
    fsu::printConsole("\n== " + name + "\n");
}

void writeFile(const std::string &path, const std::string &data)
{
    fsu::makeDirs(fsu::dirName(path));
    FILE *f = _wfopen(fsu::widen(path).c_str(), L"wb");
    if (!f)
        return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

std::string readFile(const std::string &path)
{
    std::string d;
    fsu::readFile(path, d, 1ull << 30);
    return d;
}

bool hasEntry(const CabInfo &info, const std::string &name)
{
    for (const CabEntry &e : info.entries)
        if (e.name == name)
            return true;
    return false;
}

// Запуск программы с ожиданием завершения; возвращает код возврата или -1.
int runProcess(const std::string &cmdLine)
{
    std::wstring cmd = fsu::widen(cmdLine);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = (DWORD) -1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int) code;
}

struct CountingProgress : CabProgress
{
    int files = 0;
    std::vector<std::string> stages;
    void onStage(const std::string &s) override { stages.push_back(s); }
    bool onFile(const std::string &) override { ++files; return true; }
};

struct CancelProgress : CabProgress
{
    int n = 0;
    bool onFile(const std::string &) override { return ++n < 2; }
};

const std::string kBinary("\x00\x01\x02\xff", 4);

void testFormat()
{
    section("Форматирование");
    check(formatNumber(1234567) == "1 234 567", "formatNumber разделяет разряды");
    check(formatSize(512) == "512 Б", "formatSize: байты");
    check(formatSize(1536) == "1,5 Кб", "formatSize: килобайты");
    check(formatSize(20ull * 1024 * 1024) == "20 Мб", "formatSize: мегабайты");
    check(formatSize(3ull * 1024 * 1024 * 1024) == "3,0 Гб", "formatSize: гигабайты");
    check(pluralFiles(1) == "1 файл" && pluralFiles(3) == "3 файла" && pluralFiles(5) == "5 файлов",
          "pluralFiles: 1 файл, 3 файла, 5 файлов");
    check(pluralFiles(11) == "11 файлов" && pluralFiles(21) == "21 файл" && pluralFiles(112) == "112 файлов",
          "pluralFiles: 11 файлов, 21 файл, 112 файлов");
    check(formatRatio(1, 4) == "25,0 %" && formatRatio(1, 0) == "—", "formatRatio");
    check(formatAttr(0x21) == "R--A", "formatAttr");
}

void testFsutil(const std::string &src)
{
    section("Файловая система");
    std::vector<fsu::FoundFile> found;
    std::string err;
    check(fsu::collectFiles(src, true, true, found, err) && found.size() == 5,
          "collectFiles: директория рекурсивно, 5 файлов");
    std::vector<fsu::FoundFile> masked;
    check(fsu::collectFiles(fsu::joinPath(src, "*.txt"), false, false, masked, err) && masked.size() == 2,
          "collectFiles: маска *.txt, 2 файла");
    check(fsu::wildMatch("*.TXT;*.bin", "Документ.txt"), "wildMatch: без учёта регистра, список масок");
    check(!fsu::wildMatch("*.txt", "a.txt.bak"), "wildMatch: *.txt не совпадает с a.txt.bak");
    check(fsu::wildMatch("*.", "README"), "wildMatch: *. совпадает с именем без расширения");
    check(fsu::safeRelPath("..\\..\\C:\\evil\\x.txt") == "C_\\evil\\x.txt", "safeRelPath отсекает .. и диск");
    check(fsu::safeRelPath("C:\\Windows\\a.dll") == "Windows\\a.dll", "safeRelPath отсекает ведущий диск");
    check(fsu::compareNatural(L"file2", L"file10") < 0, "compareNatural: file2 раньше file10");
}

std::vector<CabSource> sourcesOf(const std::string &dir)
{
    std::vector<fsu::FoundFile> found;
    std::string err;
    fsu::collectFiles(dir, true, true, found, err);
    std::vector<CabSource> sources;
    for (const fsu::FoundFile &f : found)
        sources.push_back({f.path, f.name});
    return sources;
}

void checkExtracted(const std::string &out, const std::string &big, const std::string &what)
{
    check(readFile(fsu::joinPath(out, "src\\Документ.txt")) == "Привет, архив!", what + ": кириллический файл");
    check(readFile(fsu::joinPath(out, "src\\big.log")) == big, what + ": большой файл");
    check(readFile(fsu::joinPath(out, "src\\sub\\Вложенный\\data.bin")) == kBinary, what + ": двоичный файл");
    check(fsu::fileExists(fsu::joinPath(out, "src\\empty.dat")), what + ": пустой файл");
}

void testMethods(const std::string &root, const std::vector<CabSource> &sources, const std::string &big)
{
    const char *names[] = {"none.cab", "mszip.cab", "lzx.cab"};
    CabCompression comps[] = {CabCompression::None, CabCompression::MSZIP, CabCompression::LZX};
    const char *methods[] = {"Без сжатия", "MSZIP", "LZX:21"};
    for (int k = 0; k < 3; ++k)
    {
        section(std::string("Создание и распаковка: ") + methods[k]);
        std::string cab = fsu::joinPath(root, names[k]);
        CountingProgress p;
        CompressionSpec spec;
        spec.type = comps[k];
        std::string err;
        check(cabCreate(cab, sources, spec, &p, err), "cabCreate " + err);
        check(p.files == 5, "onFile вызван для каждого файла");

        CabInfo info;
        check(cabRead(cab, info, err) && info.entries.size() == 5, "cabRead: 5 записей");
        check(info.offset == 0, "обычный архив начинается с начала файла");
        check(info.method == methods[k], std::string("метод сжатия: ") + info.method);
        check(hasEntry(info, "src\\Документ.txt"), "кириллическое имя");
        check(hasEntry(info, "src\\sub\\Вложенный\\data.bin"), "вложенный путь");
        check(cabHasSignature(cab), "cabHasSignature");

        CabResult res;
        check(cabTest(cab, nullptr, err, &res) && res.files == 5, "cabTest: 5 файлов");

        std::string out = fsu::joinPath(root, std::string("out_") + names[k]);
        check(cabExtract(cab, out, {}, true, Overwrite::Always, nullptr, err, &res), "cabExtract " + err);
        checkExtracted(out, big, names[k]);
    }
}

void testExtractModes(const std::string &root)
{
    section("Режимы распаковки");
    std::string cab = fsu::joinPath(root, "mszip.cab");
    std::string flat = fsu::joinPath(root, "flat");
    std::string err;
    CabResult res;
    check(cabExtract(cab, flat, {"src\\sub\\Вложенный\\data.bin"}, false, Overwrite::Always, nullptr, err, &res) &&
          res.files == 1 && fsu::fileExists(fsu::joinPath(flat, "data.bin")),
          "выбранный файл без путей");

    writeFile(fsu::joinPath(flat, "data.bin"), "local");
    check(cabExtract(cab, flat, {"src\\sub\\Вложенный\\data.bin"}, false, Overwrite::Never, nullptr, err, &res) &&
          res.skipped == 1 && readFile(fsu::joinPath(flat, "data.bin")) == "local",
          "без перезаписи существующий файл не тронут");

    std::string based = fsu::joinPath(root, "based");
    check(cabExtract(cab, based, {"src\\sub\\Вложенный\\data.bin"}, true, Overwrite::Always, nullptr, err,
                     &res, "src\\sub") &&
          readFile(fsu::joinPath(based, "Вложенный\\data.bin")) == kBinary,
          "пути относительно директории архива");

    CancelProgress cp;
    check(!cabExtract(cab, fsu::joinPath(root, "cancel"), {}, true, Overwrite::Always, &cp, err) &&
          err == kCabCancelled,
          "отмена распаковки");
}

void testUpdate(const std::string &root)
{
    section("Добавление и удаление");
    std::string cab = fsu::joinPath(root, "mszip.cab");
    std::string err;
    writeFile(fsu::joinPath(root, "новый.txt"), "добавленный файл");
    check(cabUpdate(cab, {"src\\big.log"}, {{fsu::joinPath(root, "новый.txt"), "docs\\новый.txt"}},
                    CompressionSpec(), nullptr, err),
          "cabUpdate " + err);
    CabInfo info;
    check(cabRead(cab, info, err) && info.entries.size() == 5 && !hasEntry(info, "src\\big.log") &&
          hasEntry(info, "docs\\новый.txt"),
          "удалён big.log, добавлен docs\\новый.txt");
    check(cabTest(cab, nullptr, err), "архив после пересборки проходит проверку");

    writeFile(fsu::joinPath(root, "repl.txt"), "заменено");
    check(cabUpdate(cab, {}, {{fsu::joinPath(root, "repl.txt"), "SRC\\readme.TXT"}}, CompressionSpec(),
                    nullptr, err) &&
          cabRead(cab, info, err) && info.entries.size() == 5,
          "замена файла с тем же именем без учёта регистра");

    check(!cabRead(fsu::joinPath(root, "src\\readme.txt"), info, err), "не-CAB отвергается");
}

void testSfx(const std::string &root, const std::vector<CabSource> &sources, const std::string &big)
{
    section("Самораспаковывающийся архив");
    std::string stub = readFile(CABINE_SFX_STUB);
    check(stub.size() > 4096 && stub.compare(0, 2, "MZ") == 0, "распаковщик cabine-sfx.exe прочитан");

    std::string err;
    CabInfo info;
    check(!cabRead(CABINE_SFX_STUB, info, err), "распаковщик без архива не считается архивом");

    std::string exe = fsu::joinPath(root, "sfx.exe");
    check(cabCreate(exe, sources, CompressionSpec(), nullptr, err, stub), "cabCreate с распаковщиком " + err);
    check(readFile(exe).compare(0, stub.size(), stub) == 0, "файл начинается с распаковщика");
    check(cabRead(exe, info, err) && info.offset == stub.size() && info.entries.size() == 5,
          "архив найден за образом программы");
    check(!cabHasSignature(exe) && cabHasSignature(exe, true), "cabHasSignature: только с sfx = true");

    CabResult res;
    check(cabTest(exe, nullptr, err, &res) && res.files == 5, "cabTest " + err);
    std::string out = fsu::joinPath(root, "out_sfx");
    check(cabExtract(exe, out, {}, true, Overwrite::Always, nullptr, err), "cabExtract " + err);
    checkExtracted(out, big, "sfx.exe");

    check(cabUpdate(exe, {"src\\empty.dat"}, {}, CompressionSpec(), nullptr, err), "cabUpdate " + err);
    check(readFile(exe).compare(0, stub.size(), stub) == 0, "распаковщик сохранён после пересборки");
    check(cabRead(exe, info, err) && info.offset == stub.size() && info.entries.size() == 4 &&
          !hasEntry(info, "src\\empty.dat"),
          "файл удалён из самораспаковывающегося архива");
    check(cabTest(exe, nullptr, err), "архив после пересборки проходит проверку");
}

void testSystemExpand(const std::string &root)
{
    section("Совместимость с expand.exe");
    std::string dir = fsu::joinPath(root, "expand");
    writeFile(fsu::joinPath(dir, "in\\hello.txt"), "Hello from Cabine\r\n");
    writeFile(fsu::joinPath(dir, "in\\Привет.txt"), "Привет");
    std::string err;
    std::string cab = fsu::joinPath(dir, "sys.cab");
    CompressionSpec lzx;
    lzx.type = CabCompression::LZX;
    check(cabCreate(cab, {{fsu::joinPath(dir, "in\\hello.txt"), "hello.txt"},
                          {fsu::joinPath(dir, "in\\Привет.txt"), "Привет.txt"}},
                    lzx, nullptr, err),
          "cabCreate LZX " + err);
    std::string out = fsu::joinPath(dir, "out");
    fsu::makeDirs(out);
    int code = runProcess("expand.exe \"" + cab + "\" -F:* \"" + out + "\"");
    check(code == 0, "expand.exe завершился успешно (код " + std::to_string(code) + ")");
    check(readFile(fsu::joinPath(out, "hello.txt")) == "Hello from Cabine\r\n", "expand.exe: hello.txt");
    check(readFile(fsu::joinPath(out, "Привет.txt")) == "Привет", "expand.exe: кириллическое имя");
}

} // namespace

int main()
{
    std::string root = fsu::makeTempDir();
    if (root.empty())
    {
        fsu::printConsole("Не удалось создать временную директорию\n");
        return 2;
    }
    fsu::printConsole("Временная директория: " + root + "\n");

    // Исходные файлы: латиница, кириллица, вложенные директории,
    // сжимаемый файл, двоичный и пустой файлы. Текст псевдослучайный:
    // на однотипных строках LZX из cabinet.dll работает очень медленно.
    std::string src = fsu::joinPath(root, "src");
    std::string big;
    const char *const words[] = {"архив", "файл", "сжатие", "cabinet", "директория", "data", "Turbo", "Vision"};
    uint32_t seed = 12345;
    for (int i = 0; i < 20000; ++i)
    {
        for (int w = 0; w < 6; ++w)
        {
            seed = seed * 1103515245u + 12345u;
            big += words[(seed >> 16) % 8];
            big += std::to_string((seed >> 8) % 1000) + ' ';
        }
        big += '\n';
    }
    writeFile(fsu::joinPath(src, "readme.txt"), "Hello, CAB!\r\n");
    writeFile(fsu::joinPath(src, "Документ.txt"), "Привет, архив!");
    writeFile(fsu::joinPath(src, "sub\\Вложенный\\data.bin"), kBinary);
    writeFile(fsu::joinPath(src, "big.log"), big);
    writeFile(fsu::joinPath(src, "empty.dat"), "");

    std::vector<CabSource> sources = sourcesOf(src);

    testFormat();
    testFsutil(src);
    testMethods(root, sources, big);
    testExtractModes(root);
    testUpdate(root);
    testSfx(root, sources, big);
    testSystemExpand(root);

    fsu::printConsole("\nПроверок: " + std::to_string(checks) + ", ошибок: " + std::to_string(failures) + "\n");
    if (failures == 0)
        fsu::removeTree(root);
    return failures ? 1 : 0;
}
