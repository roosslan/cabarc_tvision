// Стандартные заголовки подключаются до заголовков библиотек архивов.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <unordered_set>

#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "archive.h"
#include "fsutil.h"

#include <windows.h>

extern "C" {
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
}

// UnRAR (https://www.rarlab.com/) собирается как статическая библиотека
// с RARDLL. Лицензия UnRAR — в README, раздел «Сторонние библиотеки».
#include "dll.hpp"

// zlib: распаковка gzip (.gz, .tar.gz, .tgz).
#include "zlib.h"

// LZMA SDK (C/ из репозитория 7-Zip): распаковка 7z; расшифровка — src/7zaes.c.
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "7zaes.h"

namespace {

const uint16_t kAttrMask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                           FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;

// ---------------------------------------------------------------------------
// Общее для ZIP и RAR
// ---------------------------------------------------------------------------

PasswordPrompt g_prompt;
std::map<std::string, std::string> g_passwords;    // архив (в верхнем регистре) -> пароль

// Пароль архива: запомненный либо введённый пользователем.
// retry — запомненный пароль не подошёл, нужно спросить снова.
bool getPassword(const std::string &archive, bool retry, std::string &password)
{
    std::string key = fsu::upper(archive);
    auto it = g_passwords.find(key);
    if (!retry && it != g_passwords.end())
    {
        password = it->second;
        return true;
    }
    if (!g_prompt || !g_prompt(archive, retry, password))
        return false;
    g_passwords[key] = password;
    return true;
}

void forgetPassword(const std::string &archive)
{
    g_passwords.erase(fsu::upper(archive));
}

void toDos(time_t t, uint16_t &date, uint16_t &time)
{
    tm lt = {};
    if (t <= 0 || localtime_s(&lt, &t) != 0 || lt.tm_year < 80)
    {
        date = (1 << 5) | 1;    // 01.01.1980
        time = 0;
        return;
    }
    date = uint16_t(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    time = uint16_t((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}

void applyFileInfo(const std::string &path, uint16_t date, uint16_t time, uint16_t attribs)
{
    std::wstring w = fsu::sysPath(path);
    HANDLE h = CreateFileW(w.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        FILETIME local, utc;
        if (DosDateTimeToFileTime(date, time, &local) && LocalFileTimeToFileTime(&local, &utc))
            SetFileTime(h, nullptr, nullptr, &utc);
        CloseHandle(h);
    }
    DWORD a = attribs & kAttrMask;
    SetFileAttributesW(w.c_str(), a ? a : FILE_ATTRIBUTE_NORMAL);
}

// Общие параметры распаковки.
struct ExtractJob
{
    std::string path;
    std::string destDir;
    std::unordered_set<std::string> wanted;
    bool all = true;
    bool keepPaths = true;
    std::string base;
    bool test = false;
    Overwrite overwrite = Overwrite::Ask;
    CabProgress *progress = nullptr;
    uint64_t done = 0, total = 0;
    bool cancelled = false;
    std::string error;
    CabResult result;

    bool wants(const std::string &name) const { return all || wanted.count(name) != 0; }
};

enum class Target { Write, Skip, Stop };

// Проверка места назначения: директория с тем же именем, существующий файл
// (по режиму перезаписи), создание директорий.
Target prepareTarget(ExtractJob &j, const std::string &out, uint64_t size)
{
    if (fsu::dirExists(out))
    {
        j.error = "Невозможно создать файл: существует директория с тем же именем\n" + out;
        return Target::Stop;
    }
    if (fsu::fileExists(out))
    {
        bool replace = j.overwrite == Overwrite::Always;
        if (j.overwrite == Overwrite::Ask)
        {
            switch (j.progress ? j.progress->askOverwrite(out) : AskResult::Yes)
            {
                case AskResult::Yes:    replace = true; break;
                case AskResult::YesAll: replace = true; j.overwrite = Overwrite::Always; break;
                case AskResult::No:     replace = false; break;
                case AskResult::NoAll:  replace = false; j.overwrite = Overwrite::Never; break;
                case AskResult::Cancel: j.cancelled = true; return Target::Stop;
            }
        }
        if (!replace)
        {
            j.done += size;
            ++j.result.skipped;
            return Target::Skip;
        }
        fsu::clearReadOnly(out);
    }
    std::string dir = fsu::dirName(out);
    if (!fsu::makeDirs(dir))
    {
        j.error = "Не удалось создать директорию " + dir + "\n" + fsu::lastErrorText();
        return Target::Stop;
    }
    return Target::Write;
}

bool finish(ExtractJob &j, std::string &err, CabResult *result)
{
    if (result)
        *result = j.result;
    if (j.cancelled)
        err = kCabCancelled;
    else if (!j.error.empty())
        err = j.error;
    else
        return true;
    return false;
}

std::string methodSummary(const std::vector<std::string> &methods)
{
    if (methods.empty())
        return "—";
    for (const std::string &m : methods)
        if (m != methods[0])
            return "Смешанный";
    return methods[0];
}

// ---------------------------------------------------------------------------
// ZIP (minizip-ng)
// ---------------------------------------------------------------------------

struct ZipReader
{
    void *h = mz_zip_reader_create();
    ~ZipReader() { mz_zip_reader_delete(&h); }
};

// Имена в ZIP хранятся в UTF-8 (флаг 11) либо в кодовой странице OEM.
std::string zipName(const mz_zip_file *fi)
{
    std::string name = fi->filename ? fi->filename : "";
    if (!(fi->flag & MZ_ZIP_FLAG_UTF8) && !fsu::isAscii(name))
        name = fsu::fromCodePage(name, CP_OEMCP);
    for (char &c : name)
        if (c == '/')
            c = '\\';
    while (!name.empty() && name.back() == '\\')
        name.pop_back();
    return name;
}

// Атрибуты DOS берутся только у архивов, созданных в DOS/Windows.
uint16_t zipAttribs(const mz_zip_file *fi)
{
    int host = fi->version_madeby >> 8;
    if (host == 0 || host == 10 || host == 11 || host == 14)
        return uint16_t(fi->external_fa & kAttrMask);
    return 0;
}

std::string zipMethod(const mz_zip_file *fi)
{
    std::string m;
    switch (fi->compression_method)
    {
        case MZ_COMPRESS_METHOD_STORE:   m = "Без сжатия"; break;
        case MZ_COMPRESS_METHOD_DEFLATE: m = "Deflate"; break;
        case 9:                          m = "Deflate64"; break;
        case MZ_COMPRESS_METHOD_BZIP2:   m = "BZip2"; break;
        case MZ_COMPRESS_METHOD_LZMA:    m = "LZMA"; break;
        case MZ_COMPRESS_METHOD_ZSTD:    m = "Zstandard"; break;
        case MZ_COMPRESS_METHOD_XZ:      m = "XZ"; break;
        case MZ_COMPRESS_METHOD_PPMD:    m = "PPMd"; break;
        default:                         m = "Метод " + std::to_string(fi->compression_method); break;
    }
    if (fi->flag & MZ_ZIP_FLAG_ENCRYPTED)
        m += fi->aes_version ? ", AES" : ", пароль";
    return m;
}

std::string zipErrorText(int32_t code, const mz_zip_file *fi)
{
    switch (code)
    {
        case MZ_PASSWORD_ERROR: return "Неверный пароль";
        case MZ_SUPPORT_ERROR:
            return "Неподдерживаемый метод сжатия: " + (fi ? zipMethod(fi) : std::string("?"));
        case MZ_CRC_ERROR:      return "Ошибка контрольной суммы: архив повреждён";
        case MZ_MEM_ERROR:      return "Недостаточно памяти";
        case MZ_FORMAT_ERROR:   return "Архив повреждён: неверная структура ZIP";
        case MZ_OPEN_ERROR:     return "Не удалось открыть архив";
        case MZ_READ_ERROR:     return "Ошибка чтения архива";
        case MZ_WRITE_ERROR:    return "Ошибка записи файла";
        case MZ_DATA_ERROR:     return "Ошибка распаковки данных: архив повреждён";
        default:                return "Ошибка ZIP (код " + std::to_string(code) + ")";
    }
}

bool zipOpen(ZipReader &z, const std::string &path, std::string &err)
{
    int32_t r = mz_zip_reader_open_file(z.h, path.c_str());
    if (r != MZ_OK)
    {
        err = "Не удалось открыть ZIP-архив " + path + "\n" + zipErrorText(r, nullptr);
        return false;
    }
    return true;
}

bool zipRead(const std::string &path, CabInfo &info, std::string &err)
{
    ZipReader z;
    if (!zipOpen(z, path, err))
        return false;
    std::vector<std::string> methods;
    uint32_t index = 0;
    int32_t r = mz_zip_reader_goto_first_entry(z.h);
    for (; r == MZ_OK; r = mz_zip_reader_goto_next_entry(z.h), ++index)
    {
        mz_zip_file *fi = nullptr;
        if (mz_zip_reader_entry_get_info(z.h, &fi) != MZ_OK || !fi)
            break;
        if (mz_zip_reader_entry_is_dir(z.h) == MZ_OK)
            continue;
        CabEntry e;
        e.name = zipName(fi);
        if (e.name.empty())
            continue;
        e.size = (uint64_t) fi->uncompressed_size;
        toDos(fi->modified_date, e.date, e.time);
        e.attribs = zipAttribs(fi);
        e.index = index;
        e.encrypted = (fi->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
        info.totalSize += e.size;
        methods.push_back(zipMethod(fi));
        info.entries.push_back(std::move(e));
    }
    if (r != MZ_OK && r != MZ_END_OF_LIST)
    {
        err = zipErrorText(r, nullptr);
        return false;
    }
    info.method = methodSummary(methods);
    return true;
}

// Приёмник распакованных данных: файл или «пустышка» при проверке.
struct ZipSink
{
    ExtractJob *job = nullptr;
    int fd = -1;
};

int32_t zipWrite(void *stream, const void *buf, int32_t size)
{
    ZipSink &s = *static_cast<ZipSink *>(stream);
    if (s.fd != -1 && _write(s.fd, buf, (unsigned) size) != size)
        return -1;
    s.job->done += (uint32_t) size;
    if (s.job->progress && !s.job->progress->onBytes(s.job->done, s.job->total))
    {
        s.job->cancelled = true;
        return -1;
    }
    return size;
}

bool zipExtract(ExtractJob &j)
{
    ZipReader z;
    if (!zipOpen(z, j.path, j.error))
        return false;
    std::string password;
    bool havePassword = false;

    int32_t r = mz_zip_reader_goto_first_entry(z.h);
    for (; r == MZ_OK; r = mz_zip_reader_goto_next_entry(z.h))
    {
        mz_zip_file *fi = nullptr;
        if (mz_zip_reader_entry_get_info(z.h, &fi) != MZ_OK || !fi)
            break;
        std::string name = zipName(fi);
        if (name.empty())
            continue;
        if (mz_zip_reader_entry_is_dir(z.h) == MZ_OK)
        {
            // Пустые директории воссоздаются при распаковке всего архива.
            if (j.all && j.keepPaths && !j.test)
                fsu::makeDirs(cabTargetPath(j.destDir, name, true, j.base));
            continue;
        }
        if (!j.wants(name))
            continue;
        if (j.progress && !j.progress->onFile(name))
        {
            j.cancelled = true;
            return false;
        }

        uint64_t size = (uint64_t) fi->uncompressed_size;
        ZipSink sink;
        sink.job = &j;
        std::string out;
        if (!j.test)
        {
            out = cabTargetPath(j.destDir, name, j.keepPaths, j.base);
            Target t = prepareTarget(j, out, size);
            if (t == Target::Stop)
                return false;
            if (t == Target::Skip)
                continue;
            sink.fd = _wopen(fsu::sysPath(out).c_str(), _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
                             _S_IREAD | _S_IWRITE);
            if (sink.fd == -1)
            {
                j.error = "Не удалось создать файл " + out + "\n" + fsu::lastErrorText();
                return false;
            }
        }

        // Зашифрованный файл: пароль спрашивается один раз на архив,
        // при неверном пароле — повторно.
        bool encrypted = (fi->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
        bool retry = false;
        uint64_t startDone = j.done;
        int32_t sr;
        for (;;)
        {
            if (encrypted && (!havePassword || retry))
            {
                if (!getPassword(j.path, retry, password))
                {
                    sr = retry ? MZ_PASSWORD_ERROR : MZ_OK;
                    if (!retry)
                        j.cancelled = true;
                    break;
                }
                havePassword = true;
                mz_zip_reader_set_password(z.h, password.c_str());
            }
            sr = mz_zip_reader_entry_save(z.h, &sink, zipWrite);
            mz_zip_reader_entry_close(z.h);
            if (sr != MZ_PASSWORD_ERROR || !encrypted || j.cancelled)
                break;
            forgetPassword(j.path);
            retry = true;
            j.done = startDone;
            if (sink.fd != -1)
            {
                _chsize_s(sink.fd, 0);
                _lseeki64(sink.fd, 0, SEEK_SET);
            }
        }

        if (sink.fd != -1)
            _close(sink.fd);
        if (j.cancelled || sr != MZ_OK)
        {
            if (!out.empty())
                fsu::removeFile(out);
            if (!j.cancelled)
                j.error = name + ": " + zipErrorText(sr, fi);
            return false;
        }
        if (!out.empty())
        {
            uint16_t date, time;
            toDos(fi->modified_date, date, time);
            applyFileInfo(out, date, time, zipAttribs(fi));
        }
        ++j.result.files;
    }
    if (r != MZ_OK && r != MZ_END_OF_LIST)
    {
        j.error = zipErrorText(r, nullptr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// RAR (UnRAR)
// ---------------------------------------------------------------------------

struct RarContext
{
    std::string path;
    ExtractJob *job = nullptr;      // nullptr при чтении списка
    bool passwordAsked = false;
    bool passwordRefused = false;
    bool retry = false;             // предыдущая попытка не удалась из-за пароля
    std::string missingVolume;
};

int CALLBACK rarCallback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2)
{
    RarContext &c = *reinterpret_cast<RarContext *>(user);
    switch (msg)
    {
        case UCM_PROCESSDATA:
            if (c.job)
            {
                c.job->done += (uint64_t) p2;
                if (c.job->progress && !c.job->progress->onBytes(c.job->done, c.job->total))
                {
                    c.job->cancelled = true;
                    return -1;
                }
            }
            return 1;
        case UCM_NEEDPASSWORDW:
        {
            // Повторный запрос в той же операции означает неверный пароль.
            std::string pw;
            if (!getPassword(c.path, c.passwordAsked || c.retry, pw))
            {
                c.passwordRefused = true;
                return -1;
            }
            c.passwordAsked = true;
            std::wstring w = fsu::widen(pw);
            wchar_t *buf = reinterpret_cast<wchar_t *>(p1);
            size_t n = std::min(w.size(), (size_t) p2 - 1);
            wmemcpy(buf, w.c_str(), n);
            buf[n] = 0;
            return 1;
        }
        case UCM_NEEDPASSWORD:
            return -1;
        case UCM_CHANGEVOLUMEW:
            if (p2 == RAR_VOL_ASK)
            {
                c.missingVolume = fsu::narrow(reinterpret_cast<wchar_t *>(p1));
                return -1;
            }
            return 1;
        case UCM_LARGEDICT:
            return 1;
        default:
            return 0;
    }
}

std::string rarErrorText(int code, const RarContext &c)
{
    if (!c.missingVolume.empty())
        return "Не найден том архива: " + c.missingVolume;
    if (c.passwordRefused)
        return "Для распаковки нужен пароль";
    switch (code)
    {
        case ERAR_NO_MEMORY:        return "Недостаточно памяти";
        case ERAR_BAD_DATA:         return "Ошибка контрольной суммы: архив повреждён или пароль неверный";
        case ERAR_BAD_ARCHIVE:      return "Архив повреждён";
        case ERAR_UNKNOWN_FORMAT:   return "Неизвестный формат архива";
        case ERAR_EOPEN:            return "Не удалось открыть архив";
        case ERAR_ECREATE:          return "Не удалось создать файл";
        case ERAR_ECLOSE:           return "Ошибка закрытия файла";
        case ERAR_EREAD:            return "Ошибка чтения архива";
        case ERAR_EWRITE:           return "Ошибка записи файла";
        case ERAR_MISSING_PASSWORD: return "Для распаковки нужен пароль";
        case ERAR_BAD_PASSWORD:     return "Неверный пароль";
        case ERAR_LARGE_DICT:       return "Слишком большой словарь сжатия";
        case ERAR_EREFERENCE:       return "Не удалось восстановить ссылку на файл";
        default:                    return "Ошибка RAR (код " + std::to_string(code) + ")";
    }
}

struct RarArchive
{
    HANDLE h = nullptr;
    ~RarArchive()
    {
        if (h)
            RARCloseArchive(h);
    }
};

bool rarOpen(RarArchive &a, RarContext &c, unsigned mode, std::string &err, int *code = nullptr)
{
    std::wstring w = fsu::widen(c.path);
    RAROpenArchiveDataEx od = {};
    od.ArcNameW = &w[0];
    od.OpenMode = mode;
    od.Callback = rarCallback;
    od.UserData = reinterpret_cast<LPARAM>(&c);
    a.h = RAROpenArchiveEx(&od);
    if (!a.h || od.OpenResult != ERAR_SUCCESS)
    {
        int r = od.OpenResult ? (int) od.OpenResult : ERAR_EOPEN;
        if (code)
            *code = r;
        err = "Не удалось открыть RAR-архив " + c.path + "\n" + rarErrorText(r, c);
        return false;
    }
    return true;
}

std::string rarName(const RARHeaderDataEx &hd)
{
    std::string name = fsu::narrow(hd.FileNameW);
    for (char &c : name)
        if (c == '/')
            c = '\\';
    while (!name.empty() && name.back() == '\\')
        name.pop_back();
    return name;
}

// Атрибуты Windows берутся только у архивов, созданных в DOS/Windows.
uint16_t rarAttribs(const RARHeaderDataEx &hd)
{
    return hd.HostOS <= 2 ? uint16_t(hd.FileAttr & kAttrMask) : 0;
}

bool rarReadOnce(const std::string &path, CabInfo &info, std::string &err, bool &badPassword,
                 bool retry)
{
    RarContext c;
    c.path = path;
    c.retry = retry;
    RarArchive a;
    // Неверный пароль к зашифрованным заголовкам обнаруживается уже при открытии.
    auto wrongPassword = [&](int r) {
        return c.passwordAsked && !c.passwordRefused &&
               (r == ERAR_BAD_PASSWORD || r == ERAR_BAD_DATA || r == ERAR_MISSING_PASSWORD);
    };
    int code = 0;
    if (!rarOpen(a, c, RAR_OM_LIST, err, &code))
    {
        badPassword = wrongPassword(code);
        return false;
    }
    auto hd = std::make_unique<RARHeaderDataEx>();
    std::vector<std::string> methods;
    int r;
    uint32_t index = 0;
    while ((r = RARReadHeaderEx(a.h, hd.get())) == ERAR_SUCCESS)
    {
        if (!(hd->Flags & RHDF_DIRECTORY))
        {
            CabEntry e;
            e.name = rarName(*hd);
            e.size = hd->UnpSize | (uint64_t(hd->UnpSizeHigh) << 32);
            e.date = uint16_t(hd->FileTime >> 16);
            e.time = uint16_t(hd->FileTime & 0xFFFF);
            e.attribs = rarAttribs(*hd);
            e.index = index;
            e.encrypted = (hd->Flags & RHDF_ENCRYPTED) != 0;
            info.totalSize += e.size;
            std::string m = hd->UnpVer >= 50 ? "RAR5" : "RAR4";
            if (hd->Flags & RHDF_ENCRYPTED)
                m += ", пароль";
            methods.push_back(m);
            if (!e.name.empty())
                info.entries.push_back(std::move(e));
        }
        ++index;
        int pr = RARProcessFileW(a.h, RAR_SKIP, nullptr, nullptr);
        if (pr != ERAR_SUCCESS)
        {
            r = pr;
            break;
        }
    }
    if (r != ERAR_END_ARCHIVE)
    {
        badPassword = wrongPassword(r);
        err = rarErrorText(r, c);
        return false;
    }
    info.method = methodSummary(methods);
    return true;
}

bool rarRead(const std::string &path, CabInfo &info, std::string &err)
{
    // Архив с зашифрованными заголовками: при неверном пароле список
    // нельзя прочитать, пароль спрашивается заново.
    for (bool retry = false;; retry = true)
    {
        CabInfo attempt = info;
        bool badPassword = false;
        if (rarReadOnce(path, attempt, err, badPassword, retry))
        {
            info = std::move(attempt);
            return true;
        }
        if (!badPassword)
            return false;
        forgetPassword(path);
    }
}

bool rarExtract(ExtractJob &j)
{
    RarContext c;
    c.path = j.path;
    c.job = &j;
    RarArchive a;
    if (!rarOpen(a, c, RAR_OM_EXTRACT, j.error))
        return false;
    auto hd = std::make_unique<RARHeaderDataEx>();
    int r;
    while ((r = RARReadHeaderEx(a.h, hd.get())) == ERAR_SUCCESS)
    {
        std::string name = rarName(*hd);
        int op = RAR_SKIP;
        std::wstring out;
        std::string outPath;
        if (hd->Flags & RHDF_DIRECTORY)
        {
            if (j.all && j.keepPaths && !j.test && !name.empty())
                fsu::makeDirs(cabTargetPath(j.destDir, name, true, j.base));
        }
        else if (!name.empty() && j.wants(name))
        {
            if (j.progress && !j.progress->onFile(name))
            {
                j.cancelled = true;
                return false;
            }
            uint64_t size = hd->UnpSize | (uint64_t(hd->UnpSizeHigh) << 32);
            if (j.test)
                op = RAR_TEST;
            else
            {
                outPath = cabTargetPath(j.destDir, name, j.keepPaths, j.base);
                Target t = prepareTarget(j, outPath, size);
                if (t == Target::Stop)
                    return false;
                if (t == Target::Write)
                {
                    op = RAR_EXTRACT;
                    out = fsu::widen(outPath);
                }
            }
        }
        int pr = RARProcessFileW(a.h, op, nullptr, out.empty() ? nullptr : &out[0]);
        if (pr != ERAR_SUCCESS)
        {
            if (!outPath.empty() && op == RAR_EXTRACT)
                fsu::removeFile(outPath);
            if (pr == ERAR_BAD_PASSWORD || (pr == ERAR_BAD_DATA && c.passwordAsked))
                forgetPassword(j.path);
            if (!j.cancelled)
                j.error = name + ": " + rarErrorText(pr, c);
            return false;
        }
        if (op != RAR_SKIP)
            ++j.result.files;
    }
    if (r != ERAR_END_ARCHIVE)
    {
        if (!j.cancelled)
            j.error = rarErrorText(r, c);
        return false;
    }
    return true;
}

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// ---------------------------------------------------------------------------
// GZ, TAR, TAR.GZ (zlib и собственный разбор tar)
// ---------------------------------------------------------------------------

// Последовательное чтение: обычный файл (tar) или распаковка gzip (gz, tar.gz).
class Source
{
public:
    Source(const std::string &path, bool gzip) : gzip(gzip)
    {
        std::wstring w = fsu::sysPath(path);
        if (gzip)
            gz = gzopen_w(w.c_str(), "rb");
        else
            file = _wfopen(w.c_str(), L"rb");
    }

    ~Source()
    {
        if (gz)
            gzclose(gz);
        if (file)
            fclose(file);
    }

    bool isOpen() const { return gz || file; }

    // Читает до n байт; меньше — только в конце данных. -1 — ошибка.
    int64_t read(void *buf, size_t n)
    {
        size_t total = 0;
        while (total < n)
        {
            unsigned chunk = (unsigned) std::min<size_t>(n - total, 1 << 20);
            int got;
            if (gzip)
            {
                got = gzread(gz, static_cast<char *>(buf) + total, chunk);
                if (got < 0)
                    return -1;
            }
            else
            {
                got = (int) fread(static_cast<char *>(buf) + total, 1, chunk, file);
                if (got == 0 && ferror(file))
                    return -1;
            }
            if (got == 0)
                break;
            total += (size_t) got;
        }
        return (int64_t) total;
    }

    // Текст ошибки распаковки gzip (повреждённые данные, неверная контрольная сумма).
    std::string error()
    {
        int code = Z_OK;
        const char *msg = gz ? gzerror(gz, &code) : nullptr;
        if (gz && code != Z_OK && code != Z_BUF_ERROR)
            return std::string("Ошибка распаковки gzip: архив повреждён (") + (msg ? msg : "") + ")";
        return "Ошибка чтения архива";
    }

private:
    bool gzip;
    gzFile gz = nullptr;
    FILE *file = nullptr;
};

const size_t kTarBlock = 512;

uint64_t tarNumber(const uint8_t *p, size_t n)
{
    // Большие значения GNU tar хранит в двоичном виде (старший бит первого байта).
    if (p[0] & 0x80)
    {
        uint64_t v = p[0] & 0x7F;
        for (size_t i = 1; i < n; ++i)
            v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i]; ++i)
        if (p[i] >= '0' && p[i] <= '7')
            v = v * 8 + (p[i] - '0');
    return v;
}

std::string tarString(const uint8_t *p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len])
        ++len;
    return std::string(reinterpret_cast<const char *>(p), len);
}

// Заголовок tar: контрольная сумма считается с полем суммы, заполненным пробелами.
bool tarChecksumOk(const uint8_t *h)
{
    uint64_t stored = tarNumber(h + 148, 8);
    uint64_t sum = 0;
    for (size_t i = 0; i < kTarBlock; ++i)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return sum == stored;
}

bool tarZeroBlock(const uint8_t *h)
{
    for (size_t i = 0; i < kTarBlock; ++i)
        if (h[i])
            return false;
    return true;
}

// Имена в tar — обычно UTF-8; иначе считаются в кодировке ANSI системы.
std::string tarName(std::string raw)
{
    if (!fsu::isAscii(raw) && !fsu::isValidUtf8(raw))
        raw = fsu::fromCodePage(raw, CP_ACP);
    for (char &c : raw)
        if (c == '/')
            c = '\\';
    while (raw.compare(0, 2, ".\\") == 0)
        raw.erase(0, 2);
    while (!raw.empty() && raw.back() == '\\')
        raw.pop_back();
    return raw;
}

// Записи pax: "<длина> <ключ>=<значение>\n".
void parsePax(const std::string &data, std::string &path, uint64_t &size, bool &hasSize, time_t &mtime)
{
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t sp = data.find(' ', pos);
        if (sp == std::string::npos)
            break;
        size_t len = (size_t) strtoull(data.c_str() + pos, nullptr, 10);
        if (len == 0 || pos + len > data.size())
            break;
        std::string rec = data.substr(sp + 1, pos + len - sp - 1);
        if (!rec.empty() && rec.back() == '\n')
            rec.pop_back();
        size_t eq = rec.find('=');
        if (eq != std::string::npos)
        {
            std::string key = rec.substr(0, eq), value = rec.substr(eq + 1);
            if (key == "path")
                path = value;
            else if (key == "size")
            {
                size = strtoull(value.c_str(), nullptr, 10);
                hasSize = true;
            }
            else if (key == "mtime")
                mtime = (time_t) strtoll(value.c_str(), nullptr, 10);
        }
        pos += len;
    }
}

struct TarEntry
{
    std::string name;
    uint64_t size = 0;      // размер данных в архиве
    time_t mtime = 0;
    char type = '0';
    bool isFile() const { return type == '0' || type == '\0' || type == '7'; }
    bool isDir() const { return type == '5'; }
};

// Обход tar: для каждой записи вызывается onEntry, который может прочитать
// данные (readData) — иначе они пропускаются. Возвращает false при ошибке.
class TarWalker
{
public:
    explicit TarWalker(Source &src) : src(src) {}

    template <class F>
    bool walk(F onEntry, std::string &err)
    {
        std::vector<uint8_t> h(kTarBlock);
        std::string longName, paxPath;
        uint64_t paxSize = 0;
        bool paxHasSize = false;
        time_t paxTime = 0;
        for (;;)
        {
            int64_t got = src.read(h.data(), kTarBlock);
            if (got < 0)
            {
                err = src.error();
                return false;
            }
            if (got == 0 || tarZeroBlock(h.data()))
                return true;    // конец архива
            if (got < (int64_t) kTarBlock || !tarChecksumOk(h.data()))
            {
                err = "Архив повреждён: неверный заголовок tar";
                return false;
            }
            TarEntry e;
            e.type = (char) h[156];
            e.size = tarNumber(h.data() + 124, 12);
            e.mtime = (time_t) tarNumber(h.data() + 136, 12);
            std::string name = tarString(h.data(), 100);
            if (memcmp(h.data() + 257, "ustar", 5) == 0)
            {
                std::string prefix = tarString(h.data() + 345, 155);
                if (!prefix.empty())
                    name = prefix + "/" + name;
            }

            // Служебные записи: длинное имя GNU и расширенный заголовок pax.
            if (e.type == 'L' || e.type == 'x' || e.type == 'g' || e.type == 'K')
            {
                std::string data;
                if (!readAll(e.size, data, err))
                    return false;
                if (e.type == 'L')
                    longName = tarString(reinterpret_cast<const uint8_t *>(data.data()), data.size());
                else if (e.type == 'x')
                    parsePax(data, paxPath, paxSize, paxHasSize, paxTime);
                continue;
            }
            if (!longName.empty())
                name = longName;
            if (!paxPath.empty())
                name = paxPath;
            if (paxHasSize)
                e.size = paxSize;
            if (paxTime)
                e.mtime = paxTime;
            longName.clear();
            paxPath.clear();
            paxHasSize = false;
            paxTime = 0;

            e.name = tarName(name);
            remaining = e.isFile() ? e.size : 0;
            uint64_t padded = (e.size + kTarBlock - 1) / kTarBlock * kTarBlock;
            uint64_t dataBlocks = (e.isFile() || e.type == '1' || e.type == '2') ? 0 : padded;
            if (!e.isFile())
                remaining = 0;
            int r = onEntry(e, *this);
            if (r < 0)
                return false;
            // Непрочитанные данные и выравнивание до блока пропускаются.
            uint64_t skip = remaining + (e.isFile() ? padded - e.size : dataBlocks);
            if (!skipBytes(skip, err))
                return false;
        }
    }

    // Чтение данных текущей записи; 0 — данные закончились, -1 — ошибка.
    int64_t readData(void *buf, size_t n)
    {
        size_t want = (size_t) std::min<uint64_t>(n, remaining);
        if (want == 0)
            return 0;
        int64_t got = src.read(buf, want);
        if (got <= 0)
            return -1;
        remaining -= (uint64_t) got;
        return got;
    }

    std::string error() { return src.error(); }

private:
    bool readAll(uint64_t size, std::string &data, std::string &err)
    {
        if (size > 64 * 1024 * 1024)
        {
            err = "Архив повреждён: слишком длинный служебный заголовок tar";
            return false;
        }
        uint64_t padded = (size + kTarBlock - 1) / kTarBlock * kTarBlock;
        data.resize((size_t) padded);
        if (padded && src.read(&data[0], (size_t) padded) != (int64_t) padded)
        {
            err = "Архив повреждён: обрыв данных tar";
            return false;
        }
        data.resize((size_t) size);
        return true;
    }

    bool skipBytes(uint64_t n, std::string &err)
    {
        std::vector<char> buf(64 * 1024);
        while (n > 0)
        {
            size_t chunk = (size_t) std::min<uint64_t>(n, buf.size());
            int64_t got = src.read(buf.data(), chunk);
            if (got != (int64_t) chunk)
            {
                err = got < 0 ? src.error() : "Архив повреждён: обрыв данных tar";
                return false;
            }
            n -= chunk;
        }
        return true;
    }

    Source &src;
    uint64_t remaining = 0;
};

// Сведения из заголовка gzip: исходное имя (FNAME), время и размер (ISIZE).
struct GzHeader
{
    std::string name;
    time_t mtime = 0;
    uint64_t size = 0;
};

bool readGzHeader(const std::string &path, GzHeader &g)
{
    FILE *f = _wfopen(fsu::sysPath(path).c_str(), L"rb");
    if (!f)
        return false;
    uint8_t h[10];
    bool ok = fread(h, 1, 10, f) == 10 && h[0] == 0x1F && h[1] == 0x8B;
    if (ok)
    {
        uint8_t flags = h[3];
        g.mtime = (time_t) rd32(h + 4);
        if (flags & 0x04)  // FEXTRA
        {
            uint8_t x[2];
            if (fread(x, 1, 2, f) == 2)
                _fseeki64(f, rd16(x), SEEK_CUR);
        }
        if (flags & 0x08)  // FNAME (ISO-8859-1)
        {
            int c;
            while ((c = fgetc(f)) != EOF && c != 0)
                g.name.push_back((char) c);
        }
        uint8_t t[4];
        if (_fseeki64(f, -4, SEEK_END) == 0 && fread(t, 1, 4, f) == 4)
            g.size = rd32(t);
    }
    fclose(f);
    return ok;
}

// Имя файла внутри .gz: из заголовка, иначе имя архива без .gz (.tgz — .tar).
std::string gzInnerName(const std::string &path, const GzHeader &g)
{
    if (!g.name.empty())
    {
        std::string n = fsu::isValidUtf8(g.name) ? g.name : fsu::fromCodePage(g.name, 28591);
        n = fsu::baseName(tarName(n));
        if (!n.empty())
            return n;
    }
    std::string base = fsu::baseName(path);
    std::string ext = fsu::upper(fsu::extension(base));
    if (ext == "TGZ")
        return fsu::stripExt(base) + ".tar";
    if (ext == "GZ")
        return fsu::stripExt(base);
    return base + ".out";
}

bool tarRead(const std::string &path, bool gzip, CabInfo &info, std::string &err)
{
    Source src(path, gzip);
    if (!src.isOpen())
    {
        err = "Не удалось открыть архив " + path;
        return false;
    }
    TarWalker w(src);
    uint32_t index = 0;
    bool ok = w.walk([&](const TarEntry &e, TarWalker &) {
        if (e.isFile() && !e.name.empty())
        {
            CabEntry ce;
            ce.name = e.name;
            ce.size = e.size;
            toDos(e.mtime, ce.date, ce.time);
            ce.index = index;
            info.totalSize += ce.size;
            info.entries.push_back(std::move(ce));
        }
        ++index;
        return 0;
    }, err);
    info.method = gzip ? "gzip" : "Без сжатия";
    return ok;
}

bool gzRead(const std::string &path, CabInfo &info, std::string &err)
{
    GzHeader g;
    if (!readGzHeader(path, g))
    {
        err = "Файл не является архивом gzip";
        return false;
    }
    CabEntry e;
    e.name = gzInnerName(path, g);
    e.size = g.size;    // размер по модулю 4 Гб, как его хранит формат
    toDos(g.mtime, e.date, e.time);
    info.totalSize = e.size;
    info.entries.push_back(std::move(e));
    info.method = "gzip";
    return true;
}

// Запись данных в файл назначения (или проверка без записи).
template <class ReadFn>
bool writeTarget(ExtractJob &j, const std::string &name, uint64_t size, time_t mtime, ReadFn readFn)
{
    if (j.progress && !j.progress->onFile(name))
    {
        j.cancelled = true;
        return false;
    }
    std::string out;
    int fd = -1;
    if (!j.test)
    {
        out = cabTargetPath(j.destDir, name, j.keepPaths, j.base);
        Target t = prepareTarget(j, out, size);
        if (t == Target::Stop)
            return false;
        if (t == Target::Skip)
            return true;
        fd = _wopen(fsu::sysPath(out).c_str(), _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
                    _S_IREAD | _S_IWRITE);
        if (fd == -1)
        {
            j.error = "Не удалось создать файл " + out + "\n" + fsu::lastErrorText();
            return false;
        }
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;)
    {
        int64_t got = readFn(buf.data(), buf.size());
        if (got < 0)
        {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        if (fd != -1 && _write(fd, buf.data(), (unsigned) got) != got)
        {
            j.error = "Ошибка записи файла " + out + "\n" + fsu::lastErrorText();
            ok = false;
            break;
        }
        j.done += (uint64_t) got;
        if (j.progress && !j.progress->onBytes(j.done, j.total))
        {
            j.cancelled = true;
            ok = false;
            break;
        }
    }
    if (fd != -1)
        _close(fd);
    if (!ok)
    {
        if (!out.empty())
            fsu::removeFile(out);
        return false;
    }
    if (!out.empty())
    {
        uint16_t date, time;
        toDos(mtime, date, time);
        applyFileInfo(out, date, time, 0);
    }
    ++j.result.files;
    return true;
}

bool tarExtract(ExtractJob &j, bool gzip)
{
    Source src(j.path, gzip);
    if (!src.isOpen())
    {
        j.error = "Не удалось открыть архив " + j.path;
        return false;
    }
    TarWalker w(src);
    std::string err;
    bool ok = w.walk([&](const TarEntry &e, TarWalker &walker) {
        if (e.name.empty())
            return 0;
        if (e.isDir())
        {
            if (j.all && j.keepPaths && !j.test)
                fsu::makeDirs(cabTargetPath(j.destDir, e.name, true, j.base));
            return 0;
        }
        if (!e.isFile() || !j.wants(e.name))
            return 0;
        bool readError = false;
        bool written = writeTarget(j, e.name, e.size, e.mtime, [&](void *buf, size_t n) {
            int64_t got = walker.readData(buf, n);
            if (got < 0)
                readError = true;
            return got;
        });
        if (!written)
        {
            if (readError && j.error.empty() && !j.cancelled)
                j.error = e.name + ": " + walker.error();
            return -1;
        }
        return 0;
    }, err);
    if (!ok && j.error.empty() && !j.cancelled)
        j.error = err;
    return ok;
}

bool gzExtract(ExtractJob &j)
{
    GzHeader g;
    readGzHeader(j.path, g);
    std::string name = gzInnerName(j.path, g);
    if (!j.wants(name))
        return true;
    Source src(j.path, true);
    if (!src.isOpen())
    {
        j.error = "Не удалось открыть архив " + j.path;
        return false;
    }
    bool readError = false;
    bool ok = writeTarget(j, name, g.size, g.mtime, [&](void *buf, size_t n) {
        int64_t got = src.read(buf, n);
        if (got < 0)
            readError = true;
        return got;
    });
    if (!ok && readError && j.error.empty() && !j.cancelled)
        j.error = name + ": " + src.error();
    return ok;
}

// gzip с tar внутри: первый блок распакованных данных — заголовок tar.
bool gzContainsTar(const std::string &path)
{
    Source src(path, true);
    uint8_t h[kTarBlock];
    return src.isOpen() && src.read(h, kTarBlock) == (int64_t) kTarBlock && tarChecksumOk(h) &&
           !tarZeroBlock(h);
}

// ---------------------------------------------------------------------------
// Самораспаковывающиеся RAR, 7z и ZIP: программа (MZ), после которой дописан
// архив. Подписи RAR и 7z ищутся в первых 4 Мб (столько просматривает UnRAR),
// оглавление ZIP — в конце файла. Каждая находка подтверждается контрольной
// суммой заголовка: те же байты встречаются в данных обычных программ
// (например, в самом cabine.exe — в коде UnRAR и LZMA SDK).
// ---------------------------------------------------------------------------

const size_t kSfxScan = 0x400000;

bool readAt(const std::string &path, uint64_t pos, std::vector<uint8_t> &buf, size_t max)
{
    FILE *f = _wfopen(fsu::widen(path).c_str(), L"rb");
    if (!f)
        return false;
    buf.resize(max);
    size_t n = _fseeki64(f, (int64_t) pos, SEEK_SET) == 0 ? fread(buf.data(), 1, max, f) : 0;
    fclose(f);
    buf.resize(n);
    return n > 0;
}

uint32_t crcOf(const uint8_t *p, size_t n)
{
    return (uint32_t) crc32(0, p, (uInt) n);
}

// Число переменной длины RAR5 (vint): по 7 бит в байте, старший бит — продолжение.
bool rarVint(const uint8_t *p, const uint8_t *end, uint64_t &value, size_t &len)
{
    value = 0;
    len = 0;
    for (int shift = 0; p + len < end && shift < 64; shift += 7)
    {
        uint8_t b = p[len++];
        value |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

// Подпись RAR и следующий за ней главный заголовок архива с верной контрольной суммой.
bool rarHeaderAt(const uint8_t *p, const uint8_t *end)
{
    if (end - p >= 7 + 7 && memcmp(p, "Rar!\x1A\x07\x00", 7) == 0)
    {
        // RAR4: CRC16 (младшие биты CRC32), тип 0x73, флаги, размер заголовка.
        const uint8_t *h = p + 7;
        uint16_t size = rd16(h + 5);
        if (h[2] != 0x73 || size < 7 || size > end - h)
            return false;
        return (crcOf(h + 2, size - 2) & 0xFFFF) == rd16(h);
    }
    if (end - p >= 8 + 4 + 2 && memcmp(p, "Rar!\x1A\x07\x01\x00", 8) == 0)
    {
        // RAR5: CRC32, размер заголовка (vint), тип (vint; 1 — главный заголовок).
        const uint8_t *h = p + 8;
        uint64_t size, type;
        size_t len, typeLen;
        if (!rarVint(h + 4, end, size, len) || size == 0 || size > uint64_t(end - (h + 4 + len)))
            return false;
        if (!rarVint(h + 4 + len, end, type, typeLen) || type != 1)
            return false;
        return crcOf(h + 4, len + (size_t) size) == rd32(h);
    }
    return false;
}

// Подпись 7z и начальный заголовок (20 байт) с верной контрольной суммой.
bool sevenZipHeaderAt(const uint8_t *p, const uint8_t *end)
{
    static const uint8_t kSig[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
    return end - p >= 32 && memcmp(p, kSig, 6) == 0 && crcOf(p + 12, 20) == rd32(p + 8);
}

// Оглавление ZIP в конце файла: запись конца оглавления (PK 05 06) и начало
// оглавления (PK 01 02) там, куда она указывает, — по смещению из записи либо
// сразу перед ней (если смещения в архиве не исправлены после дописывания к программе).
bool zipTailOk(const std::string &path)
{
    uint64_t size = fsu::fileSize(path);
    if (size < 22)
        return false;
    size_t tail = (size_t) std::min<uint64_t>(size, 0xFFFF + 22);
    std::vector<uint8_t> t;
    if (!readAt(path, size - tail, t, tail) || t.size() != tail)
        return false;
    for (size_t i = tail - 22 + 1; i-- > 0;)
    {
        const uint8_t *e = t.data() + i;
        if (memcmp(e, "PK\x05\x06", 4) != 0)
            continue;
        uint64_t eocd = size - tail + i;
        uint32_t cdSize = rd32(e + 12), cdOffset = rd32(e + 16);
        if (cdOffset == 0xFFFFFFFF || cdSize == 0xFFFFFFFF)
            return i >= 20 && memcmp(e - 20, "PK\x06\x07", 4) == 0;   // ZIP64
        if (cdSize == 0 || cdSize > eocd)
            return false;
        std::vector<uint8_t> sig;
        for (uint64_t at : {uint64_t(cdOffset), eocd - cdSize})
            if (at < eocd && readAt(path, at, sig, 4) && sig.size() == 4 && memcmp(sig.data(), "PK\x01\x02", 4) == 0)
                return true;
        return false;
    }
    return false;
}

// Формат и начало архива в самораспаковывающемся файле; Unknown — не SFX.
ArchiveFormat sfxFormat(const std::string &path, uint64_t *offset = nullptr)
{
    std::vector<uint8_t> b;
    if (!readAt(path, 0, b, 2) || b.size() != 2 || b[0] != 'M' || b[1] != 'Z')
        return ArchiveFormat::Unknown;
    if (!readAt(path, 0, b, kSfxScan) || b.size() < 64)
        return ArchiveFormat::Unknown;
    const uint8_t *end = b.data() + b.size();
    for (size_t i = 2; i + 14 <= b.size(); ++i)
    {
        const uint8_t *p = b.data() + i;
        ArchiveFormat f = *p == 'R' && rarHeaderAt(p, end)        ? ArchiveFormat::Rar
                        : *p == 0x37 && sevenZipHeaderAt(p, end) ? ArchiveFormat::SevenZip
                                                                  : ArchiveFormat::Unknown;
        if (f != ArchiveFormat::Unknown)
        {
            if (offset)
                *offset = i;
            return f;
        }
    }
    if (zipTailOk(path))
    {
        if (offset)
            *offset = 0;
        return ArchiveFormat::Zip;
    }
    return ArchiveFormat::Unknown;
}

// ---------------------------------------------------------------------------
// 7z (LZMA SDK, распаковщик на C)
// ---------------------------------------------------------------------------

const ISzAlloc g_szAlloc = {SzAlloc, SzFree};
const ISzAlloc g_szAllocTemp = {SzAllocTemp, SzFreeTemp};

// Открытый архив 7z: файл, буферизованный поток и разобранные заголовки.
class SevenZip
{
public:
    explicit SevenZip(const std::string &path)
    {
        static bool crcReady = (CrcGenerateTable(), true);
        (void) crcReady;
        SzArEx_Init(&db);
        std::wstring w = fsu::sysPath(path);
        if (InFile_OpenW(&file.file, w.c_str()) != 0)
        {
            result = SZ_ERROR_READ;
            return;
        }
        fileOpen = true;
        FileInStream_CreateVTable(&file);
        file.wres = 0;
        LookToRead2_CreateVTable(&look, False);
        look.buf = static_cast<Byte *>(ISzAlloc_Alloc(&g_szAlloc, kBufSize));
        if (!look.buf)
        {
            result = SZ_ERROR_MEM;
            return;
        }
        look.bufSize = kBufSize;
        look.realStream = &file.vt;
        LookToRead2_INIT(&look)
        // Самораспаковывающийся 7z: архив начинается после программы, SzArEx_Open
        // читает его с текущей позиции потока.
        uint64_t start = 0;
        if (sfxFormat(path, &start) == ArchiveFormat::SevenZip && start != 0)
        {
            Int64 pos = (Int64) start;
            if (ILookInStream_Seek(&look.vt, &pos, SZ_SEEK_SET) != SZ_OK)
            {
                result = SZ_ERROR_READ;
                return;
            }
        }
        result = SzArEx_Open(&db, &look.vt, &g_szAlloc, &g_szAllocTemp);
        opened = result == SZ_OK;
    }

    ~SevenZip()
    {
        if (outBuffer)
            ISzAlloc_Free(&g_szAlloc, outBuffer);
        SzArEx_Free(&db, &g_szAlloc);
        if (look.buf)
            ISzAlloc_Free(&g_szAlloc, look.buf);
        if (fileOpen)
            File_Close(&file.file);
    }

    bool ok() const { return opened; }
    SRes error() const { return result; }
    UInt32 count() const { return db.NumFiles; }
    bool isDir(UInt32 i) const { return SzArEx_IsDir(&db, i) != 0; }
    uint64_t size(UInt32 i) const { return SzArEx_GetFileSize(&db, i); }

    std::string name(UInt32 i) const
    {
        size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        std::vector<UInt16> buf(len + 1, 0);
        SzArEx_GetFileNameUtf16(&db, i, buf.data());
        std::wstring w(reinterpret_cast<const wchar_t *>(buf.data()));
        std::string n = fsu::narrow(w);
        for (char &c : n)
            if (c == '/')
                c = '\\';
        while (!n.empty() && n.back() == '\\')
            n.pop_back();
        return n;
    }

    void dosTime(UInt32 i, uint16_t &date, uint16_t &time) const
    {
        date = (1 << 5) | 1;
        time = 0;
        if (!SzBitWithVals_Check(&db.MTime, i))
            return;
        FILETIME utc = {db.MTime.Vals[i].Low, db.MTime.Vals[i].High}, local;
        WORD d, t;
        if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToDosDateTime(&local, &d, &t))
        {
            date = d;
            time = t;
        }
    }

    uint16_t attribs(UInt32 i) const
    {
        return SzBitWithVals_Check(&db.Attribs, i) ? uint16_t(db.Attribs.Vals[i] & kAttrMask) : 0;
    }

    // Есть ли в архиве блоки, зашифрованные 7zAES (метод 06F10701).
    bool encrypted() const
    {
        for (UInt32 f = 0; f < db.db.NumFolders; ++f)
            if (folderEncrypted(f))
                return true;
        return false;
    }

    // Зашифрован ли файл: AES в блоке, где он лежит. Пустой файл блока не имеет.
    bool fileEncrypted(UInt32 i) const
    {
        UInt32 f = db.FileToFolder ? db.FileToFolder[i] : (UInt32) -1;
        return f != (UInt32) -1 && folderEncrypted(f);
    }

    bool folderEncrypted(UInt32 f) const
    {
        CSzFolder folder;
        CSzData sd;
        sd.Data = db.db.CodersData + db.db.FoCodersOffsets[f];
        sd.Size = db.db.FoCodersOffsets[f + 1] - db.db.FoCodersOffsets[f];
        if (SzGetNextFolderItem(&folder, &sd) != SZ_OK)
            return false;
        for (UInt32 c = 0; c < folder.NumCoders; ++c)
            if (folder.Coders[c].MethodID == 0x6F10701)
                return true;
        return false;
    }

    // Распаковка файла: solid-блок распаковывается в память целиком
    // и кэшируется для следующих файлов того же блока.
    SRes extract(UInt32 i, const Byte *&data, size_t &length)
    {
        size_t offset = 0, processed = 0;
        SRes r = SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset,
                                &processed, &g_szAlloc, &g_szAllocTemp);
        // После ошибки SDK считает блок распакованным: кэш сбрасывается,
        // иначе повтор (например, с другим паролем) вернул бы испорченные данные.
        if (r != SZ_OK)
            blockIndex = 0xFFFFFFFF;
        data = outBuffer ? outBuffer + offset : nullptr;
        length = processed;
        return r;
    }

private:
    static const size_t kBufSize = 1 << 18;
    CSzArEx db;
    CFileInStream file = {};
    CLookToRead2 look = {};
    bool fileOpen = false;
    bool opened = false;
    SRes result = SZ_OK;
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte *outBuffer = nullptr;
    size_t outBufferSize = 0;
};

std::string szErrorText(SRes r)
{
    switch (r)
    {
        case SZ_ERROR_UNSUPPORTED: return "Неподдерживаемый метод сжатия";
        case SZ_ERROR_NO_PASSWORD: return "Для распаковки нужен пароль";
        case SZ_ERROR_DATA:      return "Ошибка распаковки данных: архив повреждён";
        case SZ_ERROR_CRC:       return "Ошибка контрольной суммы: архив повреждён";
        case SZ_ERROR_MEM:       return "Недостаточно памяти (solid-блок 7z распаковывается в память целиком)";
        case SZ_ERROR_NO_ARCHIVE:return "Файл не является архивом 7z";
        case SZ_ERROR_ARCHIVE:   return "Архив повреждён: неверная структура 7z";
        case SZ_ERROR_INPUT_EOF: return "Архив повреждён: неожиданный конец файла";
        case SZ_ERROR_READ:      return "Ошибка чтения архива";
        default:                 return "Ошибка 7z (код " + std::to_string(r) + ")";
    }
}

// Пароль для расшифровки 7z; nullptr — без пароля.
void szSetPassword(const std::string *password)
{
    if (!password)
    {
        Sz7zAes_SetPassword(nullptr, 0);
        return;
    }
    std::wstring w = fsu::widen(*password);
    Sz7zAes_SetPassword(reinterpret_cast<const unsigned short *>(w.c_str()), w.size());
}

// Ошибка из-за пароля: зашифрованные данные встречались, а пароль не задан
// либо не подошёл (неверный ключ даёт ошибку данных или контрольной суммы).
bool szPasswordProblem(SRes r)
{
    return Sz7zAes_Used() && (r == SZ_ERROR_NO_PASSWORD || r == SZ_ERROR_DATA || r == SZ_ERROR_CRC ||
                              r == SZ_ERROR_ARCHIVE || r == SZ_ERROR_UNSUPPORTED);
}

// Пароль после неудачной попытки: не задан — спрашивается, не подошёл —
// спрашивается снова. false — пароль не введён.
bool szAskPassword(const std::string &path, SRes r)
{
    bool retry = r != SZ_ERROR_NO_PASSWORD;
    if (retry)
        forgetPassword(path);
    std::string password;
    if (!getPassword(path, retry, password))
        return false;
    szSetPassword(&password);
    return true;
}

// Открытие 7z. Без пароля к зашифрованным заголовкам список файлов недоступен:
// пароль спрашивается сразу, при неверном — повторно. refused — пароль не введён.
std::unique_ptr<SevenZip> szOpen(const std::string &path, std::string &err, bool *refused = nullptr)
{
    auto it = g_passwords.find(fsu::upper(path));
    szSetPassword(it != g_passwords.end() ? &it->second : nullptr);
    for (;;)
    {
        Sz7zAes_ResetUsed();
        auto z = std::make_unique<SevenZip>(path);
        if (z->ok())
            return z;
        SRes r = z->error();
        z.reset();
        if (!szPasswordProblem(r))
        {
            err = "Не удалось открыть архив 7z " + path + "\n" + szErrorText(r);
            return nullptr;
        }
        if (!szAskPassword(path, r))
        {
            err = r == SZ_ERROR_NO_PASSWORD ? "Для открытия архива 7z нужен пароль\n" + path
                                            : "Неверный пароль к архиву 7z\n" + path;
            if (refused)
                *refused = r == SZ_ERROR_NO_PASSWORD;
            return nullptr;
        }
    }
}

bool szRead(const std::string &path, CabInfo &info, std::string &err)
{
    auto z = szOpen(path, err);
    if (!z)
        return false;
    for (UInt32 i = 0; i < z->count(); ++i)
    {
        if (z->isDir(i))
            continue;
        CabEntry e;
        e.name = z->name(i);
        if (e.name.empty())
            continue;
        e.size = z->size(i);
        z->dosTime(i, e.date, e.time);
        e.attribs = z->attribs(i);
        e.encrypted = z->fileEncrypted(i);
        e.index = i;
        info.totalSize += e.size;
        info.entries.push_back(std::move(e));
    }
    info.method = z->encrypted() ? "7z, пароль" : "7z";
    return true;
}

bool szExtract(ExtractJob &j)
{
    bool refused = false;
    auto zp = szOpen(j.path, j.error, &refused);
    if (!zp)
    {
        j.cancelled = refused;
        return false;
    }
    SevenZip &z = *zp;
    for (UInt32 i = 0; i < z.count(); ++i)
    {
        std::string name = z.name(i);
        if (name.empty())
            continue;
        if (z.isDir(i))
        {
            if (j.all && j.keepPaths && !j.test)
                fsu::makeDirs(cabTargetPath(j.destDir, name, true, j.base));
            continue;
        }
        if (!j.wants(name))
            continue;
        const Byte *data = nullptr;
        size_t length = 0;
        SRes r;
        for (;;)
        {
            Sz7zAes_ResetUsed();
            r = z.extract(i, data, length);
            if (r == SZ_OK || !szPasswordProblem(r))
                break;
            if (!szAskPassword(j.path, r))
            {
                if (r == SZ_ERROR_NO_PASSWORD)
                    j.cancelled = true;
                else
                    j.error = name + ": неверный пароль";
                return false;
            }
        }
        if (r != SZ_OK)
        {
            j.error = name + ": " + szErrorText(r);
            return false;
        }
        size_t pos = 0;
        uint16_t date, time;
        z.dosTime(i, date, time);
        FILETIME ft;
        time_t mtime = 0;
        if (DosDateTimeToFileTime(date, time, &ft))
        {
            // Время для writeTarget: из DOS-времени через FILETIME местного времени.
            SYSTEMTIME st;
            FileTimeToSystemTime(&ft, &st);
            tm t = {};
            t.tm_year = st.wYear - 1900;
            t.tm_mon = st.wMonth - 1;
            t.tm_mday = st.wDay;
            t.tm_hour = st.wHour;
            t.tm_min = st.wMinute;
            t.tm_sec = st.wSecond;
            t.tm_isdst = -1;
            mtime = mktime(&t);
        }
        bool written = writeTarget(j, name, length, mtime, [&](void *buf, size_t n) {
            size_t chunk = std::min(n, length - pos);
            memcpy(buf, data + pos, chunk);
            pos += chunk;
            return (int64_t) chunk;
        });
        if (!written)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

bool runJob(ArchiveFormat format, ExtractJob &j, std::string &err, CabResult *result)
{
    if (!j.test && !fsu::makeDirs(j.destDir))
    {
        err = "Не удалось создать директорию " + j.destDir + "\n" + fsu::lastErrorText();
        return false;
    }
    switch (format)
    {
        case ArchiveFormat::Zip:   zipExtract(j); break;
        case ArchiveFormat::Rar:   rarExtract(j); break;
        case ArchiveFormat::Tar:   tarExtract(j, false); break;
        case ArchiveFormat::TarGz: tarExtract(j, true); break;
        case ArchiveFormat::SevenZip: szExtract(j); break;
        default:                   gzExtract(j); break;
    }
    return finish(j, err, result);
}

} // namespace

ArchiveFormat archiveFormat(const std::string &path, bool sfx)
{
    unsigned char sig[kTarBlock] = {};
    FILE *f = _wfopen(fsu::sysPath(path).c_str(), L"rb");
    if (!f)
        return ArchiveFormat::Unknown;
    size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    if (n >= 3 && sig[0] == 0x1F && sig[1] == 0x8B && sig[2] == 8)
        return gzContainsTar(path) ? ArchiveFormat::TarGz : ArchiveFormat::Gz;
    // tar: подпись ustar в заголовке либо, для старого формата без подписи,
    // верная контрольная сумма первого заголовка у файла .tar.
    if (n == kTarBlock && !tarZeroBlock(sig) && tarChecksumOk(sig) &&
        (memcmp(sig + 257, "ustar", 5) == 0 || fsu::upper(fsu::extension(path)) == "TAR"))
        return ArchiveFormat::Tar;
    static const unsigned char kSevenZipSig[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
    if (n >= 6 && memcmp(sig, kSevenZipSig, 6) == 0)
        return ArchiveFormat::SevenZip;
    if (n >= 4 && memcmp(sig, "MSCF", 4) == 0)
        return ArchiveFormat::Cab;
    if (n >= 4 && (memcmp(sig, "PK\x03\x04", 4) == 0 || memcmp(sig, "PK\x05\x06", 4) == 0))
        return ArchiveFormat::Zip;
    if (n >= 7 && memcmp(sig, "Rar!\x1A\x07", 6) == 0 && (sig[6] == 0 || sig[6] == 1))
        return ArchiveFormat::Rar;
    if (sfx && cabHasSignature(path, true))
        return ArchiveFormat::Cab;
    if (sfx && n >= 2 && sig[0] == 'M' && sig[1] == 'Z')
        return sfxFormat(path);
    return ArchiveFormat::Unknown;
}

const char *formatName(ArchiveFormat format)
{
    switch (format)
    {
        case ArchiveFormat::Cab: return "CAB";
        case ArchiveFormat::Zip: return "ZIP";
        case ArchiveFormat::Rar: return "RAR";
        case ArchiveFormat::Gz:  return "GZIP";
        case ArchiveFormat::Tar: return "TAR";
        case ArchiveFormat::TarGz: return "TAR.GZ";
        case ArchiveFormat::SevenZip: return "7Z";
        default:                 return "?";
    }
}

bool hasArchiveExtension(const std::string &name)
{
    std::string ext = fsu::upper(fsu::extension(name));
    return ext == "CAB" || ext == "ZIP" || ext == "RAR" || ext == "GZ" || ext == "TGZ" || ext == "TAR" ||
           ext == "7Z";
}

std::string archiveStem(const std::string &name)
{
    std::string base = fsu::baseName(name);
    std::string ext = fsu::upper(fsu::extension(base));
    std::string stem = fsu::stripExt(base);
    // «.tar.gz» снимается целиком.
    if (ext == "GZ" && fsu::upper(fsu::extension(stem)) == "TAR")
        stem = fsu::stripExt(stem);
    return stem.empty() ? base : stem;
}

bool archRead(const std::string &path, CabInfo &info, std::string &err)
{
    ArchiveFormat format = archiveFormat(path, true);
    switch (format)
    {
        case ArchiveFormat::Cab:
            if (!cabRead(path, info, err))
                return false;
            break;
        case ArchiveFormat::Zip:
        case ArchiveFormat::Rar:
        case ArchiveFormat::Gz:
        case ArchiveFormat::Tar:
        case ArchiveFormat::TarGz:
        case ArchiveFormat::SevenZip:
        {
            info = CabInfo();
            info.path = path;
            bool ok = format == ArchiveFormat::Zip ? zipRead(path, info, err)
                    : format == ArchiveFormat::Rar ? rarRead(path, info, err)
                    : format == ArchiveFormat::Gz  ? gzRead(path, info, err)
                    : format == ArchiveFormat::SevenZip ? szRead(path, info, err)
                    : tarRead(path, format == ArchiveFormat::TarGz, info, err);
            if (!ok)
                return false;
            info.fileSize = fsu::fileSize(path);
            break;
        }
        default:
            if (!fsu::fileExists(path))
                err = "Не удалось открыть файл " + path;
            else
                err = "Файл не является архивом CAB, ZIP, RAR, 7Z, GZ или TAR";
            return false;
    }
    info.format = format;
    return true;
}

bool archExtract(const std::string &path, const std::string &destDir,
                 const std::vector<std::string> &names, bool keepPaths, Overwrite ow,
                 CabProgress *progress, std::string &err, CabResult *result,
                 const std::string &base)
{
    ArchiveFormat format = archiveFormat(path, true);
    if (format == ArchiveFormat::Cab)
        return cabExtract(path, destDir, names, keepPaths, ow, progress, err, result, base);
    CabInfo info;
    if (!archRead(path, info, err))
        return false;
    ExtractJob j;
    j.path = path;
    j.destDir = destDir;
    j.all = names.empty();
    j.wanted.insert(names.begin(), names.end());
    j.keepPaths = keepPaths;
    j.base = base;
    j.overwrite = ow;
    j.progress = progress;
    for (const CabEntry &e : info.entries)
        if (j.wants(e.name))
            j.total += e.size;
    return runJob(format, j, err, result);
}

bool archTest(const std::string &path, CabProgress *progress, std::string &err, CabResult *result)
{
    ArchiveFormat format = archiveFormat(path, true);
    if (format == ArchiveFormat::Cab)
        return cabTest(path, progress, err, result);
    CabInfo info;
    if (!archRead(path, info, err))
        return false;
    ExtractJob j;
    j.path = path;
    j.test = true;
    j.progress = progress;
    j.total = info.totalSize;
    return runJob(format, j, err, result);
}

void setPasswordPrompt(PasswordPrompt prompt)
{
    g_prompt = std::move(prompt);
}
