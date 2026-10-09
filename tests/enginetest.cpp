// Тесты ядра Cabine без интерфейса: работа с CAB (cabfile), ZIP и RAR
// (archive), файловая система (fsutil), форматирование (format).
// Запускаются через CTest. Код возврата 0 — все проверки пройдены.

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../src/archive.h"
#include "../src/cabfile.h"
#include "../src/format.h"
#include "../src/fsutil.h"

#include <windows.h>

extern "C" {
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
}

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

// ---------------------------------------------------------------------------
// ZIP и RAR
// ---------------------------------------------------------------------------

struct ZipItem
{
    std::string name;       // имя в архиве (как будет записано)
    std::string data;
    bool utf8 = true;       // флаг UTF-8 у имени
};

// Создание ZIP средствами minizip-ng. password — шифрование PKWARE
// (aes = false) или WinZip AES.
bool makeZip(const std::string &path, const std::vector<ZipItem> &items, const char *password = nullptr,
             bool aes = false, uint16_t method = MZ_COMPRESS_METHOD_DEFLATE)
{
    void *w = mz_zip_writer_create();
    bool ok = mz_zip_writer_open_file(w, path.c_str(), 0, 0) == MZ_OK;
    if (password)
        mz_zip_writer_set_password(w, password);
    mz_zip_writer_set_compress_method(w, method);
    for (const ZipItem &it : items)
    {
        if (!ok)
            break;
        mz_zip_file fi = {};
        fi.version_madeby = (MZ_HOST_SYSTEM_WINDOWS_NTFS << 8) | 45;   // создано в Windows
        fi.filename = it.name.c_str();
        fi.modified_date = 1700000000;   // 14.11.2023
        fi.compression_method = method;
        fi.flag = it.utf8 ? MZ_ZIP_FLAG_UTF8 : 0;
        if (password)
        {
            fi.flag |= MZ_ZIP_FLAG_ENCRYPTED;
            if (aes)
                fi.aes_version = MZ_AES_VERSION;
        }
        ok = mz_zip_writer_add_buffer(w, it.data.data(), (int32_t) it.data.size(), &fi) == MZ_OK;
    }
    ok = mz_zip_writer_close(w) == MZ_OK && ok;
    mz_zip_writer_delete(&w);
    return ok;
}

// Ответы на запросы пароля: по очереди, затем отказ.
struct PasswordScript
{
    std::vector<std::string> answers;
    int asked = 0;
    int retries = 0;
};

PasswordScript g_script;

bool scriptedPassword(const std::string &, bool retry, std::string &password)
{
    if (retry)
        ++g_script.retries;
    if (g_script.asked >= (int) g_script.answers.size())
        return false;
    password = g_script.answers[g_script.asked++];
    return true;
}

void resetPasswords(std::vector<std::string> answers)
{
    g_script = PasswordScript();
    g_script.answers = std::move(answers);
}

void testZip(const std::string &root, const std::string &big)
{
    section("ZIP");
    std::string dir = fsu::joinPath(root, "zip");
    fsu::makeDirs(dir);
    std::vector<ZipItem> items = {
        {"readme.txt", "Hello, ZIP!\r\n"},
        {"Документ.txt", "Привет, ZIP!"},
        {"sub/Вложенный/data.bin", kBinary},
        {"sub/big.log", big},
        {"empty.dat", ""},
    };
    std::string zip = fsu::joinPath(dir, "sample.zip");
    check(makeZip(zip, items), "создан ZIP средствами minizip-ng");
    check(archiveFormat(zip) == ArchiveFormat::Zip, "формат определён по сигнатуре: ZIP");

    std::string err;
    CabInfo info;
    check(archRead(zip, info, err) && info.entries.size() == 5 && info.format == ArchiveFormat::Zip,
          "archRead: 5 файлов " + err);
    check(hasEntry(info, "Документ.txt") && hasEntry(info, "sub\\Вложенный\\data.bin"),
          "кириллические имена и вложенные пути");
    check(info.method == "Deflate", "метод сжатия: " + info.method);
    check(info.totalSize == 13 + std::string("Привет, ZIP!").size() + kBinary.size() + big.size(),
          "суммарный размер");

    CabResult res;
    check(archTest(zip, nullptr, err, &res) && res.files == 5, "archTest " + err);

    std::string out = fsu::joinPath(dir, "out");
    check(archExtract(zip, out, {}, true, Overwrite::Always, nullptr, err, &res) && res.files == 5,
          "archExtract все " + err);
    check(readFile(fsu::joinPath(out, "Документ.txt")) == "Привет, ZIP!", "содержимое кириллического файла");
    check(readFile(fsu::joinPath(out, "sub\\big.log")) == big, "содержимое большого файла");
    check(readFile(fsu::joinPath(out, "sub\\Вложенный\\data.bin")) == kBinary, "двоичный файл");
    check(fsu::fileExists(fsu::joinPath(out, "empty.dat")), "пустой файл");

    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILETIME local;
    SYSTEMTIME st;
    check(GetFileAttributesExW(fsu::widen(fsu::joinPath(out, "readme.txt")).c_str(), GetFileExInfoStandard, &fad) &&
          FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st) &&
          st.wYear == 2023 && st.wMonth == 11,
          "дата изменения восстановлена");

    std::string based = fsu::joinPath(dir, "based");
    check(archExtract(zip, based, {"sub\\Вложенный\\data.bin"}, true, Overwrite::Always, nullptr, err, &res, "sub") &&
          res.files == 1 && readFile(fsu::joinPath(based, "Вложенный\\data.bin")) == kBinary,
          "выбранный файл с путём относительно директории архива");

    writeFile(fsu::joinPath(out, "readme.txt"), "local");
    check(archExtract(zip, out, {"readme.txt"}, true, Overwrite::Never, nullptr, err, &res) &&
          res.skipped == 1 && readFile(fsu::joinPath(out, "readme.txt")) == "local",
          "без перезаписи существующий файл не тронут");

    CancelProgress cp;
    check(!archExtract(zip, fsu::joinPath(dir, "cancel"), {}, true, Overwrite::Always, &cp, err) &&
          err == kCabCancelled,
          "отмена распаковки");

    // Пароли: PKWARE и WinZip AES; сначала неверный пароль, затем верный.
    struct { const char *file; bool aes; } enc[] = {{"pkware.zip", false}, {"aes.zip", true}};
    for (auto &e : enc)
    {
        std::string path = fsu::joinPath(dir, e.file);
        check(makeZip(path, {{"секрет.txt", "тайна"}, {"b.txt", "второй"}}, "Пароль1", e.aes),
              std::string("создан зашифрованный ") + e.file);
        CabInfo ei;
        check(archRead(path, ei, err) && ei.entries.size() == 2, "список зашифрованного архива читается без пароля");
        resetPasswords({"неверный", "Пароль1"});
        std::string eo = fsu::joinPath(dir, std::string("out_") + e.file);
        check(archExtract(path, eo, {}, true, Overwrite::Always, nullptr, err, &res) && res.files == 2,
              std::string(e.file) + ": распаковка после повторного ввода пароля " + err);
        check(g_script.asked == 2 && g_script.retries == 1, "пароль спрошен дважды, второй раз как повтор");
        check(readFile(fsu::joinPath(eo, "секрет.txt")) == "тайна", std::string(e.file) + ": содержимое");
        resetPasswords({});
        check(archTest(path, nullptr, err), "введённый пароль запомнен для архива");
    }
    std::string noPw = fsu::joinPath(dir, "nopw.zip");
    makeZip(noPw, {{"x.txt", "x"}}, "abc");
    resetPasswords({});
    check(!archExtract(noPw, fsu::joinPath(dir, "out_nopw"), {}, true, Overwrite::Always, nullptr, err) &&
          err == kCabCancelled,
          "отказ от ввода пароля прерывает распаковку");

    // Имя без флага UTF-8 — в кодовой странице OEM (так пишет «Сжатая папка» Windows).
    std::wstring wname = L"Отчёт.txt";
    char oem[64];
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_OEMCP, 0, wname.c_str(), -1, oem, sizeof(oem), nullptr, &lossy);
    if (n > 0 && !lossy)
    {
        std::string path = fsu::joinPath(dir, "oem.zip");
        makeZip(path, {{oem, "OEM", false}});
        CabInfo oi;
        check(archRead(path, oi, err) && hasEntry(oi, "Отчёт.txt"), "имя в кодовой странице OEM");
    }
    else
        fsu::printConsole("пропущено: кодовая страница OEM не содержит кириллицы\n");

    std::string bad = fsu::joinPath(dir, "bad.zip");
    writeFile(bad, std::string("PK\x03\x04", 4) + "мусор");
    check(!archRead(bad, info, err), "повреждённый ZIP отвергается: " + err);
}

void testRar(const std::string &root)
{
    section("RAR");
    std::string data = CABINE_TEST_DATA;
    std::string dir = fsu::joinPath(root, "rar");
    std::string err;
    CabResult res;

    std::string rar5 = fsu::joinPath(data, "sample5.rar");
    check(archiveFormat(rar5) == ArchiveFormat::Rar, "формат определён по сигнатуре: RAR");
    CabInfo info;
    check(archRead(rar5, info, err) && info.entries.size() == 3 && info.format == ArchiveFormat::Rar,
          "archRead RAR5 (solid): 3 файла " + err);
    check(hasEntry(info, "Документ.txt") && hasEntry(info, "sub\\Вложенный\\data.bin"),
          "кириллические имена и вложенные пути");
    check(info.method == "RAR5", "метод: " + info.method);
    check(archTest(rar5, nullptr, err, &res) && res.files == 3, "archTest " + err);

    std::string out = fsu::joinPath(dir, "out5");
    check(archExtract(rar5, out, {}, true, Overwrite::Always, nullptr, err, &res) && res.files == 3,
          "archExtract все " + err);
    check(readFile(fsu::joinPath(out, "readme.txt")) == "Hello, RAR!\r\n", "readme.txt");
    check(readFile(fsu::joinPath(out, "Документ.txt")) == "Привет, RAR!", "кириллический файл");
    std::string bin;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 256; ++i)
            bin += (char) i;
    check(readFile(fsu::joinPath(out, "sub\\Вложенный\\data.bin")) == bin, "двоичный файл");

    std::string one = fsu::joinPath(dir, "one");
    check(archExtract(rar5, one, {"sub\\Вложенный\\data.bin"}, false, Overwrite::Always, nullptr, err, &res) &&
          res.files == 1 && readFile(fsu::joinPath(one, "data.bin")) == bin,
          "выбранный файл из solid-архива без путей");

    writeFile(fsu::joinPath(out, "readme.txt"), "local");
    check(archExtract(rar5, out, {"readme.txt"}, true, Overwrite::Never, nullptr, err, &res) &&
          res.skipped == 1 && readFile(fsu::joinPath(out, "readme.txt")) == "local",
          "без перезаписи существующий файл не тронут");

    // Пароль на данные: список читается, распаковка спрашивает пароль.
    std::string secret = fsu::joinPath(data, "secret5.rar");
    check(archRead(secret, info, err) && info.entries.size() == 3, "список RAR с паролем читается без пароля");
    resetPasswords({"Пароль1"});
    std::string so = fsu::joinPath(dir, "secret");
    check(archExtract(secret, so, {}, true, Overwrite::Always, nullptr, err, &res) && res.files == 3,
          "распаковка RAR с паролем " + err);
    check(g_script.asked == 1 && readFile(fsu::joinPath(so, "Документ.txt")) == "Привет, RAR!",
          "пароль спрошен один раз, содержимое верное");

    // Зашифрованные заголовки: пароль нужен уже для списка; неверный — повторный запрос.
    std::string hdr = fsu::joinPath(data, "secrethdr.rar");
    resetPasswords({"неверный", "Пароль1"});
    check(archRead(hdr, info, err) && info.entries.size() == 3, "список RAR с шифрованием заголовков " + err);
    check(g_script.asked == 2 && g_script.retries >= 1, "после неверного пароля спрошен снова");
    resetPasswords({});
    check(archTest(hdr, nullptr, err), "пароль запомнен для архива " + err);

    // Многотомный архив.
    std::string vol = fsu::joinPath(data, "vol.part1.rar");
    check(archRead(vol, info, err) && info.entries.size() == 1 && info.entries[0].size == 60000,
          "многотомный архив: один файл 60000 байт " + err);
    std::string vo = fsu::joinPath(dir, "vol");
    check(archExtract(vol, vo, {}, true, Overwrite::Always, nullptr, err, &res), "распаковка многотомного " + err);
    std::string expect;
    uint32_t seed = 1;
    for (int i = 0; i < 60000; ++i)
    {
        seed = seed * 1103515245u + 12345u;
        expect += (char) ((seed >> 16) & 0xFF);
    }
    check(readFile(fsu::joinPath(vo, "big.bin")) == expect, "содержимое файла из трёх томов");

    std::string lone = fsu::joinPath(dir, "lone");
    fsu::makeDirs(lone);
    CopyFileW(fsu::widen(vol).c_str(), fsu::widen(fsu::joinPath(lone, "vol.part1.rar")).c_str(), FALSE);
    bool extracted = archExtract(fsu::joinPath(lone, "vol.part1.rar"), fsu::joinPath(lone, "out"), {}, true,
                                 Overwrite::Always, nullptr, err);
    check(!extracted && err.find("том") != std::string::npos, "без следующего тома — понятная ошибка: " + err);
}

void testDetection(const std::string &root)
{
    section("Определение формата");
    std::string txt = fsu::joinPath(root, "plain.txt");
    writeFile(txt, "просто текст");
    check(archiveFormat(txt) == ArchiveFormat::Unknown && !isArchive(txt), "текстовый файл — не архив");
    CabInfo info;
    std::string err;
    check(!archRead(txt, info, err), "archRead отвергает не-архив: " + err);
    check(archiveFormat(fsu::joinPath(root, "mszip.cab")) == ArchiveFormat::Cab, "CAB по сигнатуре");
    check(hasArchiveExtension("a.ZIP") && hasArchiveExtension("b.rar") && !hasArchiveExtension("c.7z"),
          "расширения архивов");
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
    setPasswordPrompt(scriptedPassword);
    testZip(root, big);
    testRar(root);
    testDetection(root);

    fsu::printConsole("\nПроверок: " + std::to_string(checks) + ", ошибок: " + std::to_string(failures) + "\n");
    if (failures == 0)
        fsu::removeTree(root);
    return failures ? 1 : 0;
}
