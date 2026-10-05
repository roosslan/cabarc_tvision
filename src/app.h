#pragma once

#include <string>
#include <vector>

#define Uses_TApplication
#define Uses_TRect
#include <tvision/tv.h>

#include "cabfile.h"

class TMenuBar;
class TStatusLine;
class TPanelWindow;

// Пакетный режим командной строки: действие выполняется, и программа завершается.
enum class BatchMode { None, Add, Extract };

class TCabineApp : public TApplication
{
public:
    TCabineApp();
    ~TCabineApp();

    void handleEvent(TEvent &event) override;
    void getEvent(TEvent &event) override;
    void idle() override;

    static TMenuBar *initMenuBar(TRect r);
    static TStatusLine *initStatusLine(TRect r);

    // Открытие архива или директории в панели.
    void openPath(const std::string &path);
    // Аргументы командной строки, обрабатываемые после запуска цикла событий.
    void openLater(const std::string &path);
    void setBatch(BatchMode mode, const std::vector<std::string> &paths);
    // Новая пустая поддиректория во временной директории сеанса.
    std::string newTempDir();
    // Сохранение положения панели и удаление временных файлов.
    void cleanup();

    CompressionSpec defaultCompression;

private:
    void start();
    void runBatch();
    void batchAdd();
    void batchExtract();
    void newArchive();
    void openArchiveDialog();
    void options();
    void about();

    TPanelWindow *panel = nullptr;
    bool started = false;
    bool panelCommands = true;
    BatchMode batch = BatchMode::None;
    std::vector<std::string> pending;       // пути для открытия в панели
    std::vector<std::string> batchPaths;    // пути пакетного режима (-a, -x)
    std::string tempRoot;
    unsigned tempCounter = 0;
};

TCabineApp &cabApp();
