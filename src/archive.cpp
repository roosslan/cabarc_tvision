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
    std::wstring w = fsu::widen(path);
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
            sink.fd = _wopen(fsu::widen(out).c_str(), _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
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

// ---------------------------------------------------------------------------

bool runJob(ArchiveFormat format, ExtractJob &j, std::string &err, CabResult *result)
{
    if (!j.test && !fsu::makeDirs(j.destDir))
    {
        err = "Не удалось создать директорию " + j.destDir + "\n" + fsu::lastErrorText();
        return false;
    }
    if (format == ArchiveFormat::Zip)
        zipExtract(j);
    else
        rarExtract(j);
    return finish(j, err, result);
}

} // namespace

ArchiveFormat archiveFormat(const std::string &path, bool sfx)
{
    unsigned char sig[8] = {};
    FILE *f = _wfopen(fsu::widen(path).c_str(), L"rb");
    if (!f)
        return ArchiveFormat::Unknown;
    size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    if (n >= 4 && memcmp(sig, "MSCF", 4) == 0)
        return ArchiveFormat::Cab;
    if (n >= 4 && (memcmp(sig, "PK\x03\x04", 4) == 0 || memcmp(sig, "PK\x05\x06", 4) == 0))
        return ArchiveFormat::Zip;
    if (n >= 7 && memcmp(sig, "Rar!\x1A\x07", 6) == 0 && (sig[6] == 0 || sig[6] == 1))
        return ArchiveFormat::Rar;
    if (sfx && cabHasSignature(path, true))
        return ArchiveFormat::Cab;
    return ArchiveFormat::Unknown;
}

const char *formatName(ArchiveFormat format)
{
    switch (format)
    {
        case ArchiveFormat::Cab: return "CAB";
        case ArchiveFormat::Zip: return "ZIP";
        case ArchiveFormat::Rar: return "RAR";
        default:                 return "?";
    }
}

bool hasArchiveExtension(const std::string &name)
{
    std::string ext = fsu::upper(fsu::extension(name));
    return ext == "CAB" || ext == "ZIP" || ext == "RAR";
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
            info = CabInfo();
            info.path = path;
            if (!(format == ArchiveFormat::Zip ? zipRead(path, info, err) : rarRead(path, info, err)))
                return false;
            info.fileSize = fsu::fileSize(path);
            break;
        default:
            if (!fsu::fileExists(path))
                err = "Не удалось открыть файл " + path;
            else
                err = "Файл не является архивом CAB, ZIP или RAR";
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
