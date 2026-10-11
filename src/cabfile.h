#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Работа с CAB-архивами через Cabinet API Windows (FCI/FDI).
// Модуль не зависит от Turbo Vision; все строки в UTF-8.

enum class CabCompression { None, MSZIP, LZX };

struct CompressionSpec
{
    CabCompression type = CabCompression::MSZIP;
    int lzxWindow = 21;     // 15..21
};

// Формат архива. Структуры CabInfo и CabEntry описывают архив любого формата.
enum class ArchiveFormat { Unknown, Cab, Zip, Rar };

struct CabEntry
{
    std::string name;       // полное имя в архиве, разделитель '\'
    uint64_t size = 0;
    uint16_t date = 0;      // формат MS-DOS
    uint16_t time = 0;
    uint16_t attribs = 0;
    uint16_t folder = 0;
    uint32_t index = 0;     // порядковый номер в архиве
};

struct CabInfo
{
    std::string path;
    ArchiveFormat format = ArchiveFormat::Cab;
    // Смещение архива в файле. У самораспаковывающегося архива CAB
    // записан после программы-распаковщика, иначе смещение нулевое.
    uint64_t offset = 0;
    uint64_t fileSize = 0;      // размер файла архива
    uint64_t totalSize = 0;     // суммарный размер файлов
    uint16_t folders = 0;
    uint16_t setID = 0;
    uint16_t iCabinet = 0;
    bool hasPrev = false;
    bool hasNext = false;
    std::string prevCab, nextCab;
    std::string method;         // описание метода сжатия
    CompressionSpec compression;
    std::vector<CabEntry> entries;
};

enum class Overwrite { Ask, Always, Never };
enum class AskResult { Yes, YesAll, No, NoAll, Cancel };

// Обратная связь длительных операций. Возврат false означает отмену.
class CabProgress
{
public:
    virtual ~CabProgress() = default;
    virtual void onStage(const std::string &) {}
    virtual bool onFile(const std::string &) { return true; }
    virtual bool onBytes(uint64_t, uint64_t) { return true; }
    virtual AskResult askOverwrite(const std::string &) { return AskResult::Yes; }
};

struct CabSource
{
    std::string diskPath;
    std::string nameInCab;
};

struct CabResult
{
    int files = 0;      // обработано файлов
    int skipped = 0;    // пропущено (не перезаписаны)
};

extern const char *const kCabCancelled;

bool cabRead(const std::string &path, CabInfo &info, std::string &err);

// Файл начинается с сигнатуры CAB-архива "MSCF" (независимо от расширения).
// sfx: подходит и самораспаковывающийся архив (CAB после программы).
bool cabHasSignature(const std::string &path, bool sfx = false);

// Путь, по которому файл архива будет извлечён в destDir. С сохранением путей
// путь строится относительно директории base внутри архива.
std::string cabTargetPath(const std::string &destDir, const std::string &name, bool keepPaths,
                          const std::string &base = std::string());

// names: пустой список означает «все файлы».
bool cabExtract(const std::string &cabPath, const std::string &destDir,
                const std::vector<std::string> &names, bool keepPaths, Overwrite ow,
                CabProgress *progress, std::string &err, CabResult *result = nullptr,
                const std::string &base = std::string());

// Полная распаковка без записи на диск.
bool cabTest(const std::string &cabPath, CabProgress *progress, std::string &err,
             CabResult *result = nullptr);

// prefix — данные, записываемые перед архивом (программа-распаковщик
// самораспаковывающегося архива).
bool cabCreate(const std::string &cabPath, const std::vector<CabSource> &files,
               CompressionSpec comp, CabProgress *progress, std::string &err,
               const std::string &prefix = std::string());

// Пересборка архива: удаление файлов remove и добавление (замена) файлов add.
// Программа-распаковщик самораспаковывающегося архива сохраняется.
bool cabUpdate(const std::string &cabPath, const std::vector<std::string> &remove,
               const std::vector<CabSource> &add, CompressionSpec comp,
               CabProgress *progress, std::string &err);
