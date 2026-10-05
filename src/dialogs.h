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
};

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
struct Settings
{
    bool associate = false;         // файлы .cab открываются в Cabine
    bool archivesFirst = false;     // архивы в панели выше остальных файлов
    CompressionSpec compression;    // метод сжатия новых архивов
};

bool settingsDialog(Settings &s);
void textDialog(const char *title, const std::string &text);

bool chooseFile(const char *title, const char *wildcard, bool forSave, std::string &path);
bool chooseDirectory(const std::string &start, std::string &dir);
bool askMask(const char *title, std::string &mask);
bool askText(const char *title, const char *label, std::string &value);

void showError(const std::string &msg);
void showInfo(const std::string &msg);
bool confirm(const std::string &msg);
ushort confirm3(const std::string &msg);   // cmYes, cmNo или cmCancel

// Директория, с которой начинаются диалоги выбора файлов.
std::string &lastDirectory();

std::string compressionName(const CompressionSpec &spec);
CompressionSpec compressionFromName(const std::string &name);
