// Стандартные заголовки подключаются до fci.h/fdi.h: те определяют
// пустой макрос HUGE, который ломает объявления в <cmath>.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cabfile.h"
#include "fsutil.h"

#include <windows.h>
#include <fci.h>
#include <fdi.h>

const char *const kCabCancelled = "Операция отменена";

namespace {

const USHORT kAttrMask = _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH;

// Флаги CFHEADER (спецификация MS-CAB).
const uint16_t kHdrPrevCabinet = 0x0001;
const uint16_t kHdrNextCabinet = 0x0002;
const uint16_t kHdrReservePresent = 0x0004;

// Имена в CAB хранятся либо в UTF-8 (флаг _A_NAME_IS_UTF),
// либо в кодовой странице системы, где архив создавался.
std::string decodeName(const std::string &raw, uint16_t attribs)
{
    std::string name;
    if ((attribs & _A_NAME_IS_UTF) || fsu::isAscii(raw))
        name = raw;
    else
        name = fsu::fromCodePage(raw, CP_ACP);
    for (char &c : name)
        if (c == '/')
            c = '\\';
    return name;
}

std::string methodName(uint16_t type)
{
    switch (type & tcompMASK_TYPE)
    {
        case tcompTYPE_NONE:    return "Без сжатия";
        case tcompTYPE_MSZIP:   return "MSZIP";
        case tcompTYPE_QUANTUM: return "Quantum";
        case tcompTYPE_LZX:     return "LZX:" + std::to_string((type >> 8) & 0x1F);
        default:                return "Неизвестный";
    }
}

CompressionSpec specFromType(uint16_t type)
{
    CompressionSpec spec;
    switch (type & tcompMASK_TYPE)
    {
        case tcompTYPE_NONE:
            spec.type = CabCompression::None;
            break;
        case tcompTYPE_MSZIP:
            spec.type = CabCompression::MSZIP;
            break;
        default:
        {
            // Quantum средствами FCI не создаётся: при пересборке используется LZX.
            spec.type = CabCompression::LZX;
            int w = (type & tcompMASK_TYPE) == tcompTYPE_LZX ? (type >> 8) & 0x1F : 21;
            spec.lzxWindow = w < 15 ? 15 : w > 21 ? 21 : w;
            break;
        }
    }
    return spec;
}

TCOMP toTcomp(const CompressionSpec &spec)
{
    switch (spec.type)
    {
        case CabCompression::None:  return tcompTYPE_NONE;
        case CabCompression::MSZIP: return tcompTYPE_MSZIP;
        default:                    return (TCOMP) TCOMPfromLZXWindow(spec.lzxWindow);
    }
}

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

bool readExact(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

bool readSz(FILE *f, std::string &s)
{
    s.clear();
    for (int i = 0; i < 256; ++i)
    {
        int c = fgetc(f);
        if (c == EOF)
            return false;
        if (c == 0)
            return true;
        s.push_back((char) c);
    }
    return false;
}

INT_PTR openFile(const char *path, int oflag, int pmode)
{
    return _wopen(fsu::widen(path).c_str(), oflag | _O_BINARY, pmode);
}

void applyFileInfo(const std::string &path, USHORT date, USHORT time, USHORT attribs)
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

// ---------------------------------------------------------------------------
// FDI: распаковка
// ---------------------------------------------------------------------------

// Дескриптор-«пустышка» для проверки архива: данные никуда не пишутся.
const INT_PTR kNullSink = 0x7FFFFFF0;

struct FdiContext
{
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
    int curFd = -1;
    std::string curPath;
    CabResult result;

    INT_PTR copyFile(PFDINOTIFICATION p);
    INT_PTR closeFile(PFDINOTIFICATION p);
};

// У FNWRITE/FNCLOSE нет пользовательского параметра.
FdiContext *g_fdi = nullptr;

FNALLOC(memAlloc) { return malloc(cb); }
FNFREE(memFree) { free(pv); }
FNOPEN(fdiOpen) { return openFile(pszFile, oflag, pmode); }
FNREAD(fdiRead) { return (UINT) _read((int) hf, pv, cb); }

FNWRITE(fdiWrite)
{
    UINT n = hf == kNullSink ? cb : (UINT) _write((int) hf, pv, cb);
    if (n != (UINT) -1 && g_fdi)
    {
        g_fdi->done += n;
        if (g_fdi->progress && !g_fdi->progress->onBytes(g_fdi->done, g_fdi->total))
        {
            g_fdi->cancelled = true;
            return (UINT) -1;
        }
    }
    return n;
}

FNCLOSE(fdiClose)
{
    if (hf == kNullSink)
        return 0;
    if (g_fdi && hf == g_fdi->curFd)
        g_fdi->curFd = -1;
    return _close((int) hf);
}

FNSEEK(fdiSeek) { return _lseek((int) hf, dist, seektype); }

INT_PTR FdiContext::copyFile(PFDINOTIFICATION p)
{
    std::string name = decodeName(p->psz1, p->attribs);
    if (!all && !wanted.count(name))
        return 0;
    if (progress && !progress->onFile(name))
    {
        cancelled = true;
        return -1;
    }
    if (test)
        return kNullSink;

    std::string out = cabTargetPath(destDir, name, keepPaths, base);
    if (fsu::dirExists(out))
    {
        error = "Невозможно создать файл: существует директория с тем же именем\n" + out;
        return -1;
    }
    if (fsu::fileExists(out))
    {
        bool replace = overwrite == Overwrite::Always;
        if (overwrite == Overwrite::Ask)
        {
            switch (progress ? progress->askOverwrite(out) : AskResult::Yes)
            {
                case AskResult::Yes:    replace = true; break;
                case AskResult::YesAll: replace = true; overwrite = Overwrite::Always; break;
                case AskResult::No:     replace = false; break;
                case AskResult::NoAll:  replace = false; overwrite = Overwrite::Never; break;
                case AskResult::Cancel: cancelled = true; return -1;
            }
        }
        if (!replace)
        {
            done += (uint32_t) p->cb;
            ++result.skipped;
            return 0;
        }
        fsu::clearReadOnly(out);
    }

    std::string dir = fsu::dirName(out);
    if (!fsu::makeDirs(dir))
    {
        error = "Не удалось создать директорию " + dir + "\n" + fsu::lastErrorText();
        return -1;
    }
    int fd = _wopen(fsu::widen(out).c_str(), _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
                    _S_IREAD | _S_IWRITE);
    if (fd == -1)
    {
        error = "Не удалось создать файл " + out + "\n" + fsu::lastErrorText();
        return -1;
    }
    curFd = fd;
    curPath = out;
    return fd;
}

INT_PTR FdiContext::closeFile(PFDINOTIFICATION p)
{
    if (p->hf != kNullSink)
    {
        _close((int) p->hf);
        curFd = -1;
        applyFileInfo(curPath, p->date, p->time, p->attribs);
        curPath.clear();
    }
    ++result.files;
    return TRUE;
}

FNFDINOTIFY(fdiNotify)
{
    FdiContext &c = *static_cast<FdiContext *>(pfdin->pv);
    switch (fdint)
    {
        case fdintCOPY_FILE:
            return c.copyFile(pfdin);
        case fdintCLOSE_FILE_INFO:
            return c.closeFile(pfdin);
        case fdintNEXT_CABINET:
            // FDI ищет следующий том в той же директории.
            if (pfdin->fdie != FDIERROR_NONE)
            {
                c.error = "Не найден следующий том архива: " +
                    decodeName(pfdin->psz1 ? pfdin->psz1 : "", 0);
                return -1;
            }
            return 0;
        default:
            return 0;
    }
}

std::string fdiErrorText(int code)
{
    switch (code)
    {
        case FDIERROR_CABINET_NOT_FOUND:        return "Файл архива не найден";
        case FDIERROR_NOT_A_CABINET:            return "Файл не является CAB-архивом";
        case FDIERROR_UNKNOWN_CABINET_VERSION:  return "Неподдерживаемая версия CAB-архива";
        case FDIERROR_CORRUPT_CABINET:          return "Архив повреждён";
        case FDIERROR_ALLOC_FAIL:               return "Недостаточно памяти";
        case FDIERROR_BAD_COMPR_TYPE:           return "Неподдерживаемый метод сжатия";
        case FDIERROR_MDI_FAIL:                 return "Ошибка распаковки данных: архив повреждён";
        case FDIERROR_TARGET_FILE:              return "Ошибка записи распакованного файла";
        case FDIERROR_RESERVE_MISMATCH:         return "Тома архива не согласованы между собой";
        case FDIERROR_WRONG_CABINET:            return "Неверный том многотомного архива";
        case FDIERROR_USER_ABORT:               return kCabCancelled;
        default:                                return "Ошибка распаковки (код " + std::to_string(code) + ")";
    }
}

bool runFdi(const std::string &cabPath, FdiContext &c, std::string &err, CabResult *result)
{
    ERF erf = {};
    HFDI hfdi = FDICreate(memAlloc, memFree, fdiOpen, fdiRead, fdiWrite, fdiClose, fdiSeek,
                          cpuUNKNOWN, &erf);
    if (!hfdi)
    {
        err = "Не удалось инициализировать распаковщик CAB";
        return false;
    }
    std::string name = fsu::baseName(cabPath);
    std::string dir = fsu::withSlash(fsu::dirName(cabPath));
    std::vector<char> nameBuf(name.begin(), name.end()), dirBuf(dir.begin(), dir.end());
    nameBuf.push_back(0);
    dirBuf.push_back(0);

    g_fdi = &c;
    BOOL ok = FDICopy(hfdi, nameBuf.data(), dirBuf.data(), 0, fdiNotify, nullptr, &c);
    g_fdi = nullptr;
    FDIDestroy(hfdi);

    // Недописанный файл после ошибки или отмены не оставляется.
    if (c.curFd != -1)
    {
        _close(c.curFd);
        fsu::removeFile(c.curPath);
    }
    if (result)
        *result = c.result;
    if (c.cancelled)
        err = kCabCancelled;
    else if (!c.error.empty())
        err = c.error;
    else if (!ok)
        err = fdiErrorText(erf.erfOper);
    else
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// FCI: упаковка
// ---------------------------------------------------------------------------

struct FciContext
{
    CabProgress *progress = nullptr;
    uint64_t done = 0, total = 0;
    bool cancelled = false;
    bool utfName = false;
    std::string error;
};

FNFCIALLOC(fciAlloc) { return malloc(cb); }
FNFCIFREE(fciFree) { free(memory); }

FNFCIOPEN(fciOpen)
{
    INT_PTR h = openFile(pszFile, oflag, pmode);
    if (h == -1)
        *err = errno;
    return h;
}

FNFCIREAD(fciRead)
{
    UINT n = (UINT) _read((int) hf, memory, cb);
    if (n != cb)
        *err = errno;
    return n;
}

FNFCIWRITE(fciWrite)
{
    UINT n = (UINT) _write((int) hf, memory, cb);
    if (n != cb)
        *err = errno;
    return n;
}

FNFCICLOSE(fciClose)
{
    int r = _close((int) hf);
    if (r != 0)
        *err = errno;
    return r;
}

FNFCISEEK(fciSeek)
{
    long r = _lseek((int) hf, dist, seektype);
    if (r == -1)
        *err = errno;
    return r;
}

FNFCIDELETE(fciDelete)
{
    int r = _wremove(fsu::widen(pszFile).c_str());
    if (r != 0)
        *err = errno;
    return r;
}

FNFCIGETTEMPFILE(fciTempFile)
{
    wchar_t dir[MAX_PATH + 1], file[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH + 1, dir) || !GetTempFileNameW(dir, L"cab", 0, file))
        return FALSE;
    // FCI создаёт файл сам, нужен только свободный путь.
    DeleteFileW(file);
    std::string s = fsu::narrow(file);
    if ((int) s.size() >= cbTempName)
        return FALSE;
    memcpy(pszTempName, s.c_str(), s.size() + 1);
    return TRUE;
}

FNFCIFILEPLACED(fciFilePlaced) { return 0; }

// Размер тома не ограничен, поэтому переход на следующий том — ошибка.
FNFCIGETNEXTCABINET(fciNextCabinet) { return FALSE; }

FNFCISTATUS(fciStatus)
{
    FciContext &c = *static_cast<FciContext *>(pv);
    switch (typeStatus)
    {
        case statusFile:
            c.done += cb2;
            if (c.progress && !c.progress->onBytes(c.done, c.total))
            {
                c.cancelled = true;
                return -1;
            }
            return 0;
        case statusFolder:
            if (c.progress && !c.progress->onBytes(cb1, cb2))
            {
                c.cancelled = true;
                return -1;
            }
            return 0;
        case statusCabinet:
            return (long) cb2;
        default:
            return 0;
    }
}

FNFCIGETOPENINFO(fciOpenInfo)
{
    FciContext &c = *static_cast<FciContext *>(pv);
    std::wstring w = fsu::widen(pszName);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad))
    {
        c.error = "Не удалось открыть файл " + std::string(pszName) + "\n" + fsu::lastErrorText();
        *err = ENOENT;
        return -1;
    }
    FILETIME local;
    if (!FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) ||
        !FileTimeToDosDateTime(&local, pdate, ptime))
    {
        *pdate = (1 << 5) | 1;  // 01.01.1980
        *ptime = 0;
    }
    *pattribs = (USHORT) (fad.dwFileAttributes & kAttrMask);
    if (c.utfName)
        *pattribs |= _A_NAME_IS_UTF;
    int fd = _wopen(w.c_str(), _O_RDONLY | _O_BINARY);
    if (fd == -1)
    {
        c.error = "Не удалось открыть файл " + std::string(pszName) + "\n" + fsu::lastErrorText();
        *err = errno;
        return -1;
    }
    return fd;
}

std::string fciErrorText(const ERF &erf)
{
    switch (erf.erfOper)
    {
        case FCIERR_OPEN_SRC:           return "Не удалось открыть исходный файл";
        case FCIERR_READ_SRC:           return "Ошибка чтения исходного файла";
        case FCIERR_ALLOC_FAIL:         return "Недостаточно памяти";
        case FCIERR_TEMP_FILE:          return "Ошибка работы с временным файлом";
        case FCIERR_BAD_COMPR_TYPE:     return "Неподдерживаемый метод сжатия";
        case FCIERR_CAB_FILE:           return "Ошибка записи файла архива";
        case FCIERR_USER_ABORT:         return kCabCancelled;
        case FCIERR_MCI_FAIL:           return "Ошибка сжатия данных";
        case FCIERR_CAB_FORMAT_LIMIT:   return "Превышено ограничение формата CAB (размер архива до 2 Гб)";
        default:                        return "Ошибка создания архива (код " + std::to_string(erf.erfOper) + ")";
    }
}

std::vector<char> cstr(const std::string &s)
{
    std::vector<char> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

} // namespace

// ---------------------------------------------------------------------------

bool cabRead(const std::string &path, CabInfo &info, std::string &err)
{
    info = CabInfo();
    info.path = path;
    FILE *f = _wfopen(fsu::widen(path).c_str(), L"rb");
    if (!f)
    {
        err = "Не удалось открыть файл " + path + "\n" + fsu::lastErrorText();
        return false;
    }
    std::unique_ptr<FILE, int (*)(FILE *)> guard(f, fclose);

    const char *const corrupt = "Архив повреждён: неверная структура заголовка";
    uint8_t h[36];
    if (!readExact(f, h, sizeof(h)) || memcmp(h, "MSCF", 4) != 0)
    {
        err = "Файл не является CAB-архивом";
        return false;
    }
    if (h[25] != 1)
    {
        err = "Неподдерживаемая версия формата CAB: " + std::to_string(h[25]) + "." +
            std::to_string(h[24]);
        return false;
    }
    uint32_t coffFiles = rd32(h + 16);
    uint16_t cFolders = rd16(h + 26);
    uint16_t cFiles = rd16(h + 28);
    uint16_t flags = rd16(h + 30);
    info.setID = rd16(h + 32);
    info.iCabinet = rd16(h + 34);
    info.hasPrev = (flags & kHdrPrevCabinet) != 0;
    info.hasNext = (flags & kHdrNextCabinet) != 0;

    uint8_t cbFolderReserve = 0;
    if (flags & kHdrReservePresent)
    {
        uint8_t r[4];
        if (!readExact(f, r, sizeof(r)) || fseek(f, rd16(r), SEEK_CUR) != 0)
        {
            err = corrupt;
            return false;
        }
        cbFolderReserve = r[2];
    }
    std::string disk;
    if (info.hasPrev && !(readSz(f, info.prevCab) && readSz(f, disk)))
    {
        err = corrupt;
        return false;
    }
    if (info.hasNext && !(readSz(f, info.nextCab) && readSz(f, disk)))
    {
        err = corrupt;
        return false;
    }
    info.prevCab = decodeName(info.prevCab, 0);
    info.nextCab = decodeName(info.nextCab, 0);

    std::vector<uint16_t> types;
    for (uint16_t i = 0; i < cFolders; ++i)
    {
        uint8_t fo[8];
        if (!readExact(f, fo, sizeof(fo)) || fseek(f, cbFolderReserve, SEEK_CUR) != 0)
        {
            err = corrupt;
            return false;
        }
        types.push_back(rd16(fo + 6));
    }

    if (fseek(f, (long) coffFiles, SEEK_SET) != 0)
    {
        err = corrupt;
        return false;
    }
    info.entries.reserve(cFiles);
    for (uint16_t i = 0; i < cFiles; ++i)
    {
        uint8_t fe[16];
        std::string raw;
        if (!readExact(f, fe, sizeof(fe)) || !readSz(f, raw))
        {
            err = corrupt;
            return false;
        }
        CabEntry e;
        e.size = rd32(fe);
        e.folder = rd16(fe + 8);
        e.date = rd16(fe + 10);
        e.time = rd16(fe + 12);
        uint16_t attribs = rd16(fe + 14);
        e.attribs = attribs & (kAttrMask | _A_EXEC);
        e.name = decodeName(raw, attribs);
        e.index = i;
        info.totalSize += e.size;
        info.entries.push_back(std::move(e));
    }

    info.folders = cFolders;
    info.fileSize = fsu::fileSize(path);
    if (types.empty())
        info.method = "—";
    else
    {
        info.method = methodName(types[0]);
        for (uint16_t t : types)
            if (methodName(t) != info.method)
            {
                info.method = "Смешанный";
                break;
            }
        info.compression = specFromType(types[0]);
    }
    return true;
}

bool cabHasSignature(const std::string &path)
{
    FILE *f = _wfopen(fsu::widen(path).c_str(), L"rb");
    if (!f)
        return false;
    char sig[4] = {0};
    bool ok = fread(sig, 1, 4, f) == 4 && memcmp(sig, "MSCF", 4) == 0;
    fclose(f);
    return ok;
}

std::string cabTargetPath(const std::string &destDir, const std::string &name, bool keepPaths,
                          const std::string &base)
{
    std::string rel = keepPaths ? name : fsu::baseName(name);
    if (keepPaths && !base.empty() && name.size() > base.size() + 1 && name[base.size()] == '\\' &&
        fsu::upper(name.substr(0, base.size())) == fsu::upper(base))
        rel = name.substr(base.size() + 1);
    return fsu::joinPath(destDir, fsu::safeRelPath(rel));
}

bool cabExtract(const std::string &cabPath, const std::string &destDir,
                const std::vector<std::string> &names, bool keepPaths, Overwrite ow,
                CabProgress *progress, std::string &err, CabResult *result,
                const std::string &base)
{
    CabInfo info;
    if (!cabRead(cabPath, info, err))
        return false;
    FdiContext c;
    c.destDir = destDir;
    c.all = names.empty();
    c.wanted.insert(names.begin(), names.end());
    c.keepPaths = keepPaths;
    c.base = base;
    c.overwrite = ow;
    c.progress = progress;
    for (const CabEntry &e : info.entries)
        if (c.all || c.wanted.count(e.name))
            c.total += e.size;
    if (!fsu::makeDirs(destDir))
    {
        err = "Не удалось создать директорию " + destDir + "\n" + fsu::lastErrorText();
        return false;
    }
    return runFdi(cabPath, c, err, result);
}

bool cabTest(const std::string &cabPath, CabProgress *progress, std::string &err,
             CabResult *result)
{
    CabInfo info;
    if (!cabRead(cabPath, info, err))
        return false;
    FdiContext c;
    c.test = true;
    c.progress = progress;
    c.total = info.totalSize;
    return runFdi(cabPath, c, err, result);
}

bool cabCreate(const std::string &cabPath, const std::vector<CabSource> &filesIn,
               CompressionSpec comp, CabProgress *progress, std::string &err)
{
    // Одинаковые имена в архиве не допускаются: последний источник заменяет предыдущий.
    std::vector<CabSource> files;
    std::unordered_map<std::string, size_t> byName;
    for (const CabSource &s : filesIn)
    {
        std::string key = fsu::upper(s.nameInCab);
        auto it = byName.find(key);
        if (it != byName.end())
            files[it->second] = s;
        else
        {
            byName.emplace(key, files.size());
            files.push_back(s);
        }
    }
    if (files.empty())
    {
        err = "Нет файлов для помещения в архив";
        return false;
    }
    if (files.size() > 0xFFFF)
    {
        err = "Слишком много файлов: формат CAB допускает не более 65535";
        return false;
    }

    FciContext c;
    c.progress = progress;
    for (const CabSource &s : files)
    {
        if (s.nameInCab.size() >= CB_MAX_FILENAME)
        {
            err = "Слишком длинное имя в архиве:\n" + s.nameInCab;
            return false;
        }
        uint64_t size = fsu::fileSize(s.diskPath);
        if (size >= 0x7FFF8000ull)
        {
            err = "Файл слишком велик для формата CAB (не более 2 Гб):\n" + s.diskPath;
            return false;
        }
        c.total += size;
    }

    // Архив пишется во временный файл рядом с целевым и подменяет его только при успехе.
    std::string tmpPath = cabPath + ".tmp~";
    std::string dir = fsu::withSlash(fsu::dirName(tmpPath));
    std::string name = fsu::baseName(tmpPath);
    if (dir.size() >= CB_MAX_CAB_PATH || name.size() >= CB_MAX_CABINET_NAME)
    {
        err = "Слишком длинный путь к архиву";
        return false;
    }

    CCAB ccab;
    memset(&ccab, 0, sizeof(ccab));
    ccab.cb = 0x7FFFFFFF;
    ccab.cbFolderThresh = 0x7FFFFFFF;
    ccab.iCab = 1;
    ccab.setID = (USHORT) (GetTickCount() & 0xFFFF);
    memcpy(ccab.szCabPath, dir.c_str(), dir.size() + 1);
    memcpy(ccab.szCab, name.c_str(), name.size() + 1);

    ERF erf = {};
    HFCI hfci = FCICreate(&erf, fciFilePlaced, fciAlloc, fciFree, fciOpen, fciRead, fciWrite,
                          fciClose, fciSeek, fciDelete, fciTempFile, &ccab, &c);
    if (!hfci)
    {
        err = fciErrorText(erf);
        return false;
    }

    if (progress)
        progress->onStage("Сжатие файлов");
    TCOMP tcomp = toTcomp(comp);
    bool ok = true;
    for (const CabSource &s : files)
    {
        if (progress && !progress->onFile(s.nameInCab))
        {
            c.cancelled = true;
            ok = false;
            break;
        }
        c.utfName = !fsu::isAscii(s.nameInCab);
        std::vector<char> src = cstr(s.diskPath), dst = cstr(s.nameInCab);
        if (!FCIAddFile(hfci, src.data(), dst.data(), FALSE, fciNextCabinet, fciStatus,
                        fciOpenInfo, tcomp))
        {
            ok = false;
            break;
        }
    }
    if (ok)
    {
        if (progress)
            progress->onStage("Запись архива");
        ok = FCIFlushCabinet(hfci, FALSE, fciNextCabinet, fciStatus) != FALSE;
    }
    FCIDestroy(hfci);

    if (!ok)
    {
        err = c.cancelled ? std::string(kCabCancelled)
            : !c.error.empty() ? c.error
            : fciErrorText(erf);
        fsu::removeFile(tmpPath);
        return false;
    }
    if (!fsu::moveReplace(tmpPath, cabPath))
    {
        err = "Не удалось записать архив " + cabPath + "\n" + fsu::lastErrorText();
        fsu::removeFile(tmpPath);
        return false;
    }
    return true;
}

bool cabUpdate(const std::string &cabPath, const std::vector<std::string> &remove,
               const std::vector<CabSource> &add, CompressionSpec comp,
               CabProgress *progress, std::string &err)
{
    CabInfo info;
    if (!cabRead(cabPath, info, err))
        return false;
    if (info.hasPrev || info.hasNext)
    {
        err = "Изменение многотомных архивов не поддерживается";
        return false;
    }

    std::unordered_set<std::string> drop;
    for (const std::string &n : remove)
        drop.insert(fsu::upper(n));
    for (const CabSource &s : add)
        drop.insert(fsu::upper(s.nameInCab));
    std::vector<std::string> keep;
    for (const CabEntry &e : info.entries)
        if (!drop.count(fsu::upper(e.name)))
            keep.push_back(e.name);

    // Оставшиеся файлы распаковываются во временную директорию
    // и упаковываются заново вместе с добавляемыми.
    std::vector<CabSource> sources;
    std::string tmp;
    if (!keep.empty())
    {
        tmp = fsu::makeTempDir();
        if (tmp.empty())
        {
            err = "Не удалось создать временную директорию";
            return false;
        }
        if (progress)
            progress->onStage("Распаковка существующих файлов");
        if (!cabExtract(cabPath, tmp, keep, true, Overwrite::Always, progress, err))
        {
            fsu::removeTree(tmp);
            return false;
        }
        for (const std::string &n : keep)
            sources.push_back({cabTargetPath(tmp, n, true), n});
    }
    sources.insert(sources.end(), add.begin(), add.end());

    bool ok = cabCreate(cabPath, sources, comp, progress, err);
    if (!tmp.empty())
        fsu::removeTree(tmp);
    return ok;
}
