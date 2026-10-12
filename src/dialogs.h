#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#define Uses_TDialog
#include <tvision/tv.h>

#include "cabfile.h"

class TButton;
class TTextLine;
class TProgressBar;

// Окно хода длительной операции. Вставляется на рабочий стол немодально
// и перерисовывается принудительно; Esc прерывает операцию.
class TProgressDialog : public TDialog, public CabProgress
{
public:
    explicit TProgressDialog(const char *title);

    void onStage(const std::string &text) override;
    bool onFile(const std::string &name) override;
    bool onBytes(uint64_t done, uint64_t total) override;
    AskResult askOverwrite(const std::string &path) override;

    bool cancelled() const { return cancelled_; }
    // Показ завершённой операции: «Готово» и 100 % в течение holdMs.
    void showDone(unsigned holdMs);

private:
    bool refresh(bool force);
    bool confirmCancel();

    TButton *cancelButton;
    TTextLine *stage;
    TTextLine *file;
    TProgressBar *bar;
    bool cancelled_ = false;
    std::chrono::steady_clock::time_point lastRefresh;
};

// Выполняет операцию с окном хода выполнения. Ошибку (кроме отмены)
// показывает сообщением. Возвращает результат операции. holdMs — сколько
// показывать окно с 100 % после успешного завершения.
bool runWithProgress(const char *title,
                     const std::function<bool(CabProgress *, std::string &)> &op,
                     unsigned holdMs = 0);

struct AddOptions
{
    std::vector<std::string> items;
    bool recurse = true;
    bool keepPaths = true;
    std::string prefix;
    CompressionSpec compression;
    bool allowSfx = false;  // новый архив: показывать размер тома и выбор SFX
    bool sfx = false;
    uint64_t volumeSize = 0;    // 0 — один файл
};

// Новый архив или добавление в архив (окно «Добавить в архив ...»).
struct ArchiveTarget
{
    std::string path;
    CompressionSpec compression;
    uint64_t volumeSize = 0;
    bool sfx = false;
};

// Окно с путём архива («Обзор...»), степенью сжатия, размером тома и SFX.
bool archiveTargetDialog(const char *title, ArchiveTarget &t);

struct ExtractOptions
{
    std::string dest;
    bool onlySelected = true;
    bool keepPaths = true;
    Overwrite overwrite = Overwrite::Ask;
};

bool addFilesDialog(const char *title, AddOptions &opt);
// totalCount == 0: извлечение архивов целиком, без выбора «выбранные / все».
bool extractDialog(int selectedCount, int totalCount, ExtractOptions &opt);
// Окно самораспаковывающегося архива: сведения об архиве и куда распаковывать.
bool sfxDialog(const std::string &about, ExtractOptions &opt);
struct Settings
{
    bool associateCab = false;      // файлы архивов открываются в Cabine
    bool associateZip = false;
    bool associateRar = false;
    bool associate7z = false;
    bool associateTar = false;      // .tar, .tgz и .gz (в том числе .tar.gz)
    bool archivesFirst = false;     // архивы в панели выше остальных файлов
    CompressionSpec compression;    // метод сжатия новых архивов
};

bool settingsDialog(Settings &s);
void textDialog(const char *title, const std::string &text);

bool chooseFile(const char *title, const char *wildcard, bool forSave, std::string &path);
bool chooseDirectory(const std::string &start, std::string &dir);
bool askMask(const char *title, std::string &mask);
bool askText(const char *title, const char *label, std::string &value);
// Окно ввода пути с кнопкой «Обзор...»: выбор файла (wildcard — маска списка)
// или директории.
enum class PathKind { File, Directory };
bool askPath(const char *title, const char *label, PathKind kind, const char *wildcard,
             std::string &value);
// Пароль зашифрованного архива; retry — предыдущий пароль не подошёл.
bool askPassword(const std::string &archive, bool retry, std::string &password);

void showError(const std::string &msg);
void showInfo(const std::string &msg);
bool confirm(const std::string &msg);
ushort confirm3(const std::string &msg);   // cmYes, cmNo или cmCancel

// Директория, с которой начинаются диалоги выбора файлов.
std::string &lastDirectory();

// Степени сжатия для выпадающего списка: названия, номер степени по методу
// и метод по номеру (0 — без сжатия, последняя — максимальное).
std::vector<std::string> compressionLevels();
int compressionLevel(const CompressionSpec &spec);
CompressionSpec compressionOfLevel(int level);

std::string compressionName(const CompressionSpec &spec);
CompressionSpec compressionFromName(const std::string &name);
