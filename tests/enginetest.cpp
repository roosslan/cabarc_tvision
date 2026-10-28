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
#include "../src/shellreg.h"

#include <windows.h>

extern "C" {
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
}

#include "zlib.h"

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

    uint64_t v = 1;
    check(parseSize("", v) && v == 0 && parseSize("Не делить", v) && v == 0, "parseSize: пусто и «Не делить» — 0");
    check(parseSize("100 Мб", v) && v == 100ull << 20, "parseSize: 100 Мб");
    check(parseSize("700мб", v) && v == 700ull << 20, "parseSize: 700мб без пробела");
    check(parseSize("1,44 Мб", v) && v == 1457664, "parseSize: 1,44 Мб — дискета");
    check(parseSize("2 ГБ", v) && v == 2ull << 30, "parseSize: 2 ГБ");
    check(parseSize("512 kb", v) && v == 512ull << 10, "parseSize: 512 kb (латиница)");
    check(parseSize("1.5 G", v) && v == 3ull << 29, "parseSize: 1.5 G");
    check(parseSize("4096", v) && v == 4096, "parseSize: число без единиц — байты");
    check(!parseSize("много", v) && !parseSize("10 ТБ", v), "parseSize: неверная запись отвергается");
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

void testVolumes(const std::string &root, const std::vector<CabSource> &sources, const std::string &big)
{
    section("Многотомный архив");
    std::string dir = fsu::joinPath(root, "volumes");
    fsu::makeDirs(dir);
    std::string err;
    CompressionSpec none;
    none.type = CabCompression::None;
    const uint64_t volume = 128 * 1024;

    std::string cab = fsu::joinPath(dir, "набор.cab");
    check(cabVolumeName(cab, 2) == fsu::joinPath(dir, "набор2.cab"), "имя второго тома: набор2.cab");
    check(cabCreate(cab, sources, none, nullptr, err, std::string(), volume), "cabCreate по 128 Кб " + err);
    int count = 1;
    while (fsu::fileExists(cabVolumeName(cab, count + 1)))
        ++count;
    check(count >= 5, "создано томов: " + std::to_string(count));
    bool sizesOk = fsu::fileSize(cab) <= volume;
    for (int n = 2; n <= count; ++n)
        sizesOk = sizesOk && fsu::fileSize(cabVolumeName(cab, n)) <= volume;
    check(sizesOk, "каждый том не больше 128 Кб");
    check(!fsu::fileExists(cab + ".tmp~"), "временных файлов не осталось");

    CabInfo info;
    check(cabRead(cab, info, err) && info.volumes == count && info.entries.size() == 5 &&
          info.missingVolume.empty(),
          "cabRead читает все тома: 5 файлов");
    uint64_t total = 0;
    for (const CabSource &s : sources)
        total += fsu::fileSize(s.diskPath);
    check(info.totalSize == total, "суммарный размер по всем томам без повторов");

    CabResult res;
    bool ok = cabTest(cab, nullptr, err, &res);
    check(ok && res.files == 5, "cabTest по всем томам " + err);
    std::string out = fsu::joinPath(dir, "out");
    ok = cabExtract(cab, out, {}, true, Overwrite::Always, nullptr, err, &res);
    check(ok && res.files == 5, "cabExtract по всем томам " + err);
    checkExtracted(out, big, "многотомный");

    // Нет последнего тома: список неполный, распаковка — с ошибкой.
    std::string part = fsu::joinPath(dir, "part");
    fsu::makeDirs(part);
    for (int n = 1; n < count; ++n)
    {
        std::string from = n == 1 ? cab : cabVolumeName(cab, n);
        CopyFileW(fsu::widen(from).c_str(), fsu::widen(fsu::joinPath(part, fsu::baseName(from))).c_str(), FALSE);
    }
    std::string partCab = fsu::joinPath(part, fsu::baseName(cab));
    ok = cabRead(partCab, info, err);
    check(ok && info.missingVolume == fsu::baseName(cabVolumeName(cab, count)),
          "отсутствующий том назван: " + info.missingVolume);
    bool extracted = cabExtract(partCab, fsu::joinPath(part, "out"), {}, true, Overwrite::Always, nullptr, err);
    check(!extracted, "распаковка без последнего тома завершается ошибкой: " + err);

    // Самораспаковывающийся первый том.
    std::string stub = readFile(CABINE_SFX_STUB);
    const uint64_t sfxVolume = stub.size() + 256 * 1024;
    std::string exe = fsu::joinPath(dir, "setup.exe");
    check(cabCreate(exe, sources, none, nullptr, err, stub, sfxVolume), "cabCreate SFX по томам " + err);
    check(fsu::fileSize(exe) <= sfxVolume, "первый том вместе с распаковщиком укладывается в размер тома");
    check(fsu::fileExists(fsu::joinPath(dir, "setup2.cab")), "второй том — setup2.cab");
    ok = cabRead(exe, info, err);
    check(ok && info.offset == stub.size() && info.volumes >= 2 && info.entries.size() == 5,
          "SFX читается со всеми томами: томов " + std::to_string(info.volumes));
    std::string sout = fsu::joinPath(dir, "sfx_out");
    ok = cabExtract(exe, sout, {}, true, Overwrite::Always, nullptr, err, &res);
    check(ok && res.files == 5, "распаковка SFX по всем томам " + err);
    checkExtracted(sout, big, "SFX многотомный");
    check(!cabUpdate(exe, {"src\\empty.dat"}, {}, none, nullptr, err), "изменение многотомного архива запрещено");
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

// ---------------------------------------------------------------------------
// GZ, TAR, TAR.GZ
// ---------------------------------------------------------------------------

struct TarItem
{
    std::string name;       // имя в архиве (UTF-8, разделитель '/')
    std::string data;
    char type = '0';
    bool pax = false;       // имя передаётся расширенным заголовком pax
};

void tarHeader(std::string &out, const std::string &name, uint64_t size, char type)
{
    char h[512] = {};
    memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    snprintf(h + 100, 8, "%07o", 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011llo", (unsigned long long) size);
    snprintf(h + 136, 12, "%011llo", 1700000000ull);   // 14.11.2023
    memset(h + 148, ' ', 8);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (unsigned char c : h)
        sum += c;
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    out.append(h, 512);
}

void tarData(std::string &out, const std::string &data)
{
    out += data;
    out.append((512 - data.size() % 512) % 512, '\0');
}

// tar-архив: длинные имена — записью GNU 'L', отмеченные pax — заголовком 'x'.
std::string makeTar(const std::vector<TarItem> &items)
{
    std::string out;
    for (const TarItem &it : items)
    {
        std::string name = it.name;
        if (it.pax)
        {
            std::string rec = " path=" + name + "\n";
            std::string len = std::to_string(rec.size() + 2);
            if (len.size() + rec.size() != std::stoul(len))
                len = std::to_string(rec.size() + len.size());
            std::string pax = len + rec;
            tarHeader(out, "PaxHeader/file", pax.size(), 'x');
            tarData(out, pax);
            name = "short.txt";
        }
        else if (name.size() > 100)
        {
            tarHeader(out, "././@LongLink", name.size() + 1, 'L');
            tarData(out, name + '\0');
            name = name.substr(0, 100);
        }
        tarHeader(out, name, it.type == '0' ? it.data.size() : 0, it.type);
        if (it.type == '0')
            tarData(out, it.data);
    }
    out.append(1024, '\0');
    return out;
}

bool gzipFile(const std::string &path, const std::string &data)
{
    gzFile gz = gzopen_w(fsu::widen(path).c_str(), "wb9");
    if (!gz)
        return false;
    bool ok = data.empty() || gzwrite(gz, data.data(), (unsigned) data.size()) == (int) data.size();
    return gzclose(gz) == Z_OK && ok;
}

void testTarGz(const std::string &root, const std::string &big)
{
    section("TAR, TAR.GZ, GZ");
    std::string dir = fsu::joinPath(root, "tar");
    fsu::makeDirs(dir);
    std::string err;
    CabResult res;

    std::string longName = "long/" + std::string(110, 'x') + "/файл.txt";
    std::vector<TarItem> items = {
        {"readme.txt", "Hello, TAR!\n"},
        {"Документ.txt", "Привет, TAR!"},
        {"sub/", "", '5'},
        {"sub/Вложенный/data.bin", kBinary},
        {"sub/big.log", big},
        {"empty.dat", ""},
        {longName, "длинное имя"},
        {"pax/имя через pax.txt", "pax", '0', true},
    };
    std::string tarData = makeTar(items);
    std::string tar = fsu::joinPath(dir, "sample.tar");
    writeFile(tar, tarData);

    check(archiveFormat(tar) == ArchiveFormat::Tar, "формат определён: TAR");
    CabInfo info;
    check(archRead(tar, info, err) && info.entries.size() == 7 && info.format == ArchiveFormat::Tar,
          "archRead TAR: 7 файлов " + err);
    check(hasEntry(info, "Документ.txt") && hasEntry(info, "sub\\Вложенный\\data.bin"), "кириллица и вложенные пути");
    std::string longWin = longName;
    for (char &c : longWin)
        if (c == '/')
            c = '\\';
    check(hasEntry(info, longWin), "длинное имя GNU (запись L)");
    check(hasEntry(info, "pax\\имя через pax.txt"), "имя из заголовка pax");

    std::string out = fsu::joinPath(dir, "out_tar");
    bool ok = archExtract(tar, out, {}, true, Overwrite::Always, nullptr, err, &res);
    check(ok && res.files == 7, "archExtract TAR " + err);
    check(readFile(fsu::joinPath(out, "Документ.txt")) == "Привет, TAR!" &&
          readFile(fsu::joinPath(out, "sub\\big.log")) == big &&
          readFile(fsu::joinPath(out, "sub\\Вложенный\\data.bin")) == kBinary &&
          fsu::fileExists(fsu::joinPath(out, "empty.dat")) &&
          readFile(fsu::joinPath(out, longWin)) == "длинное имя" &&
          readFile(fsu::joinPath(out, "pax\\имя через pax.txt")) == "pax",
          "содержимое файлов TAR");

    std::string based = fsu::joinPath(dir, "based");
    ok = archExtract(tar, based, {"sub\\Вложенный\\data.bin"}, true, Overwrite::Always, nullptr, err, &res, "sub");
    check(ok && res.files == 1 && readFile(fsu::joinPath(based, "Вложенный\\data.bin")) == kBinary,
          "выбранный файл с путём относительно директории архива");

    // tar.gz и tgz.
    for (const char *name : {"sample.tar.gz", "sample.tgz"})
    {
        std::string tgz = fsu::joinPath(dir, name);
        check(gzipFile(tgz, tarData), std::string("создан ") + name);
        check(archiveFormat(tgz) == ArchiveFormat::TarGz, std::string(name) + ": формат TAR.GZ");
        ok = archRead(tgz, info, err);
        check(ok && info.entries.size() == 7 && info.method == "gzip", std::string(name) + ": 7 файлов " + err);
        ok = archTest(tgz, nullptr, err, &res);
        check(ok && res.files == 7, std::string(name) + ": проверка " + err);
        std::string o = fsu::joinPath(dir, std::string("out_") + name);
        ok = archExtract(tgz, o, {}, true, Overwrite::Always, nullptr, err, &res);
        check(ok && readFile(fsu::joinPath(o, "sub\\big.log")) == big, std::string(name) + ": распаковка " + err);
    }

    // Отдельный файл в gzip.
    std::string gz = fsu::joinPath(dir, "big.log.gz");
    check(gzipFile(gz, big), "создан big.log.gz");
    check(archiveFormat(gz) == ArchiveFormat::Gz, "формат GZIP");
    ok = archRead(gz, info, err);
    check(ok && info.entries.size() == 1 && info.entries[0].name == "big.log" && info.entries[0].size == big.size(),
          "в GZIP один файл big.log с размером из заголовка");
    std::string go = fsu::joinPath(dir, "out_gz");
    ok = archExtract(gz, go, {}, true, Overwrite::Always, nullptr, err, &res);
    check(ok && res.files == 1 && readFile(fsu::joinPath(go, "big.log")) == big, "распаковка GZIP " + err);

    // Повреждённые данные gzip.
    std::string data = readFile(gz);
    std::string broken = fsu::joinPath(dir, "broken.log.gz");
    for (size_t i = data.size() / 2; i < data.size() / 2 + 64; ++i)
        data[i] = (char) ~data[i];
    writeFile(broken, data);
    ok = archTest(broken, nullptr, err);
    check(!ok, "повреждённый GZIP не проходит проверку: " + err);

    // Отмена.
    CancelProgress cp;
    ok = archExtract(tar, fsu::joinPath(dir, "cancel"), {}, true, Overwrite::Always, &cp, err);
    check(!ok && err == kCabCancelled, "отмена распаковки TAR");

    check(archiveStem("D:\\x\\src.tar.gz") == "src" && archiveStem("a.tgz") == "a" &&
          archiveStem("b.gz") == "b" && archiveStem("Отчёт.zip") == "Отчёт",
          "archiveStem: .tar.gz снимается целиком");

    // Архив, созданный системным tar.exe (bsdtar, есть в Windows 10 1803 и новее).
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::string tarExe = fsu::joinPath(fsu::narrow(sys), "tar.exe");
    if (fsu::fileExists(tarExe))
    {
        std::string src = fsu::joinPath(dir, "systar_src");
        writeFile(fsu::joinPath(src, "Отчёт.txt"), "из tar.exe");
        writeFile(fsu::joinPath(src, "inner\\b.txt"), "b");
        std::string sysTgz = fsu::joinPath(dir, "system.tgz");
        int code = runProcess("\"" + tarExe + "\" -czf \"" + sysTgz + "\" -C \"" + src + "\" .");
        check(code == 0, "tar.exe создал архив (код " + std::to_string(code) + ")");
        std::string so = fsu::joinPath(dir, "out_system");
        ok = archExtract(sysTgz, so, {}, true, Overwrite::Always, nullptr, err, &res);
        check(ok && readFile(fsu::joinPath(so, "inner\\b.txt")) == "b", "распаковка архива tar.exe " + err);
        check(readFile(fsu::joinPath(so, "Отчёт.txt")) == "из tar.exe", "кириллическое имя из tar.exe");
    }
    else
        fsu::printConsole("пропущено: нет tar.exe\n");
}

void test7z(const std::string &root)
{
    section("7z");
    std::string data = CABINE_TEST_DATA;
    std::string dir = fsu::joinPath(root, "7z");
    std::string err;
    CabResult res;
    std::string bin;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 256; ++i)
            bin += (char) i;

    // Образцы созданы 7zr.exe из исходников 7-Zip (тот же набор файлов, что у RAR).
    struct { const char *file; const char *what; } samples[] = {
        {"sample.7z", "solid LZMA2"},
        {"bcj.7z", "BCJ + LZMA"},
        {"delta.7z", "Delta + LZMA2"},
    };
    for (auto &s : samples)
    {
        std::string path = fsu::joinPath(data, s.file);
        std::string what = std::string(s.file) + " (" + s.what + ")";
        check(archiveFormat(path) == ArchiveFormat::SevenZip, what + ": формат 7Z");
        CabInfo info;
        bool ok = archRead(path, info, err);
        check(ok && info.entries.size() == 3 && info.format == ArchiveFormat::SevenZip, what + ": 3 файла " + err);
        check(hasEntry(info, "Документ.txt") && hasEntry(info, "sub\\Вложенный\\data.bin"), what + ": имена");
        ok = archTest(path, nullptr, err, &res);
        check(ok && res.files == 3, what + ": проверка " + err);
        std::string out = fsu::joinPath(dir, s.file);
        ok = archExtract(path, out, {}, true, Overwrite::Always, nullptr, err, &res);
        check(ok && res.files == 3 && readFile(fsu::joinPath(out, "readme.txt")) == "Hello, RAR!\r\n" &&
              readFile(fsu::joinPath(out, "Документ.txt")) == "Привет, RAR!" &&
              readFile(fsu::joinPath(out, "sub\\Вложенный\\data.bin")) == bin,
              what + ": содержимое " + err);
    }

    std::string sample = fsu::joinPath(data, "sample.7z");
    std::string based = fsu::joinPath(dir, "based");
    bool ok = archExtract(sample, based, {"sub\\Вложенный\\data.bin"}, true, Overwrite::Always, nullptr, err,
                          &res, "sub");
    check(ok && res.files == 1 && readFile(fsu::joinPath(based, "Вложенный\\data.bin")) == bin,
          "выбранный файл из solid-блока с путём относительно директории архива");

    std::string out = fsu::joinPath(dir, "sample.7z");
    writeFile(fsu::joinPath(out, "readme.txt"), "local");
    ok = archExtract(sample, out, {"readme.txt"}, true, Overwrite::Never, nullptr, err, &res);
    check(ok && res.skipped == 1 && readFile(fsu::joinPath(out, "readme.txt")) == "local",
          "без перезаписи существующий файл не тронут");

    // Пароль на данные (7zAES): список читается без пароля, распаковка спрашивает его;
    // неверный пароль — повторный запрос.
    CabInfo info;
    std::string secret = fsu::joinPath(data, "secret.7z");
    resetPasswords({});
    ok = archRead(secret, info, err);
    check(ok && info.entries.size() == 3 && info.method == "7z, пароль" && g_script.asked == 0,
          "список 7z с паролем на данные читается без пароля " + err);
    std::string so = fsu::joinPath(dir, "secret");
    ok = archExtract(secret, so, {}, true, Overwrite::Always, nullptr, err, &res);
    check(!ok && g_script.asked == 0, "без пароля 7z не распаковывается");
    resetPasswords({"неверный", "Пароль1"});
    ok = archExtract(secret, so, {}, true, Overwrite::Always, nullptr, err, &res);
    check(ok && res.files == 3 && g_script.asked == 2 && g_script.retries >= 1,
          "распаковка 7z с паролем, после неверного спрошен снова " + err);
    check(readFile(fsu::joinPath(so, "readme.txt")) == "Hello, RAR!\r\n" &&
              readFile(fsu::joinPath(so, "sub\\Вложенный\\data.bin")) == bin,
          "содержимое расшифровано верно");

    // Зашифрованные заголовки: пароль нужен уже для списка.
    std::string hdr = fsu::joinPath(data, "secrethdr.7z");
    resetPasswords({"неверный", "Пароль1"});
    ok = archRead(hdr, info, err);
    check(ok && info.entries.size() == 3 && g_script.asked == 2, "список 7z с шифрованием заголовков " + err);
    resetPasswords({});
    check(archTest(hdr, nullptr, err, &res) && res.files == 3, "пароль 7z запомнен для архива " + err);
    resetPasswords({});
    std::string fresh = fsu::joinPath(dir, "hdr-copy.7z");
    CopyFileW(fsu::widen(hdr).c_str(), fsu::widen(fresh).c_str(), FALSE);
    ok = archRead(fresh, info, err);
    check(!ok && err.find("пароль") != std::string::npos, "без пароля — сообщение: " + err);

    check(hasArchiveExtension("a.7Z") && archiveStem("архив.7z") == "архив", "расширение .7z");
}

// Самораспаковывающиеся RAR, 7z и ZIP: программа, после которой дописан архив.
void testForeignSfx(const std::string &root)
{
    section("Самораспаковывающиеся RAR, 7z, ZIP");
    std::string data = CABINE_TEST_DATA;
    std::string dir = fsu::joinPath(root, "sfx-foreign");
    fsu::makeDirs(dir);
    std::string stub = readFile(CABINE_SFX_STUB);
    std::string zip = fsu::joinPath(dir, "plain.zip");
    makeZip(zip, {{"readme.txt", "Hello, ZIP!\r\n"}, {"sub/Документ.txt", "Привет, ZIP!"}});

    struct { const char *name; std::string archive; ArchiveFormat format; const char *readme; } cases[] = {
        {"rar-sfx.exe", fsu::joinPath(data, "sample5.rar"), ArchiveFormat::Rar, "Hello, RAR!\r\n"},
        {"7z-sfx.exe", fsu::joinPath(data, "sample.7z"), ArchiveFormat::SevenZip, "Hello, RAR!\r\n"},
        {"zip-sfx.exe", zip, ArchiveFormat::Zip, "Hello, ZIP!\r\n"},
    };
    std::string err;
    for (auto &c : cases)
    {
        std::string exe = fsu::joinPath(dir, c.name);
        writeFile(exe, stub + readFile(c.archive));
        check(archiveFormat(exe, true) == c.format, std::string(c.name) + ": формат " + formatName(c.format));
        check(archiveFormat(exe) == ArchiveFormat::Unknown, std::string(c.name) + ": без sfx — программа");
        CabInfo info;
        bool ok = archRead(exe, info, err);
        check(ok && hasEntry(info, "readme.txt"), std::string(c.name) + ": список " + err);
        std::string out = fsu::joinPath(dir, std::string(c.name) + ".out");
        ok = archExtract(exe, out, {}, true, Overwrite::Always, nullptr, err);
        check(ok && readFile(fsu::joinPath(out, "readme.txt")) == c.readme, std::string(c.name) + ": распаковка " + err);
    }
    // Обычные программы с теми же байтами в данных (код UnRAR и LZMA SDK) — не архивы.
    check(archiveFormat(CABINE_SFX_STUB, true) == ArchiveFormat::Unknown, "распаковщик Cabine — не архив");
    check(archiveFormat(fsu::exePath(), true) == ArchiveFormat::Unknown,
          "программа с кодом UnRAR и LZMA SDK — не архив");
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
    check(hasArchiveExtension("a.ZIP") && hasArchiveExtension("b.rar") && !hasArchiveExtension("c.arj"),
          "расширения архивов");
}

// Path пользователя: HKEY_CURRENT_USER подменяется временным разделом,
// настоящая переменная Path не затрагивается.
void testUserPath()
{
    section("Path пользователя");
    std::wstring tmpKey = L"Software\\CabineTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY fake;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, tmpKey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &fake,
                        nullptr) != ERROR_SUCCESS)
    {
        check(false, "временный раздел реестра");
        return;
    }
    RegOverridePredefKey(HKEY_CURRENT_USER, fake);

    auto readPath = [](std::wstring &value, DWORD &type) {
        wchar_t buf[1024] = {};
        DWORD size = sizeof(buf);
        bool ok = RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path", RRF_RT_ANY | RRF_NOEXPAND, &type,
                               buf, &size) == ERROR_SUCCESS;
        value = buf;
        return ok;
    };
    std::string err;
    const std::string dir = "D:\\Prog\\Cabine";
    check(!userPathContains(dir), "в пустом Path директории нет");
    check(addToUserPath(dir, err), "добавление в отсутствующий Path: " + err);
    std::wstring value;
    DWORD type = 0;
    check(readPath(value, type) && value == L"D:\\Prog\\Cabine" && type == REG_EXPAND_SZ,
          "Path создан как REG_EXPAND_SZ");
    check(removeFromUserPath(dir, err) && !readPath(value, type), "удаление единственного элемента убирает Path");

    HKEY env;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_WRITE, nullptr, &env, nullptr);
    const std::wstring initial = L"%USERPROFILE%\\bin;C:\\Tools;";
    RegSetValueExW(env, L"Path", 0, REG_EXPAND_SZ, (const BYTE *) initial.c_str(),
                   DWORD((initial.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(env);
    check(addToUserPath(dir, err) && readPath(value, type) &&
              value == L"%USERPROFILE%\\bin;C:\\Tools;D:\\Prog\\Cabine",
          "добавление в конец без удвоения «;», %переменные% не раскрываются");
    check(addToUserPath("d:\\prog\\cabine\\", err) && readPath(value, type) &&
              value == L"%USERPROFILE%\\bin;C:\\Tools;D:\\Prog\\Cabine",
          "повторное добавление (другой регистр, «\\» в конце) ничего не меняет");
    check(userPathContains("D:\\PROG\\CABINE"), "userPathContains без учёта регистра");
    check(removeFromUserPath(dir, err) && readPath(value, type) && value == L"%USERPROFILE%\\bin;C:\\Tools" &&
              type == REG_EXPAND_SZ,
          "удаление оставляет прочие элементы");

    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(fake);
    RegDeleteTreeW(HKEY_CURRENT_USER, tmpKey.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, tmpKey.c_str());

    check(!fsu::findEditor("notepad").empty(), "findEditor находит notepad.exe");
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
    testVolumes(root, sources, big);
    testSystemExpand(root);
    setPasswordPrompt(scriptedPassword);
    testZip(root, big);
    testRar(root);
    testTarGz(root, big);
    test7z(root);
    testForeignSfx(root);
    testDetection(root);
    testUserPath();

    fsu::printConsole("\nПроверок: " + std::to_string(checks) + ", ошибок: " + std::to_string(failures) + "\n");
    if (failures == 0)
        fsu::removeTree(root);
    return failures ? 1 : 0;
}
