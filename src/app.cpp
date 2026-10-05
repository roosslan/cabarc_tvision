#define Uses_TKeys
#define Uses_TEvent
#define Uses_TRect
#define Uses_TView
#define Uses_TGroup
#define Uses_TWindow
#define Uses_TProgram
#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TSubMenu
#define Uses_TStatusLine
#define Uses_TStatusItem
#define Uses_TStatusDef
#define Uses_TCommandSet
#include <tvision/tv.h>

#include <algorithm>

#include "app.h"
#include "commands.h"
#include "dialogs.h"
#include "fsutil.h"
#include "format.h"
#include "hotkeys.h"
#include "panel.h"
#include "shellreg.h"
#include "version.h"

namespace {

// Сколько показывать «Готово» после пакетной операции.
const unsigned kBatchHoldMs = 600;

TCommandSet panelCommandSet()
{
    TCommandSet cs;
    for (int c = cmPanelFirst; c <= cmPanelLast; ++c)
        cs += c;
    return cs;
}

} // namespace

TCabineApp &cabApp()
{
    return *static_cast<TCabineApp *>(TProgram::application);
}

TCabineApp::TCabineApp() :
    TProgInit(&TCabineApp::initStatusLine, &TCabineApp::initMenuBar, &TCabineApp::initDeskTop)
{
    defaultCompression = compressionFromName(fsu::loadSetting("Compression", "mszip"));
}

TCabineApp::~TCabineApp()
{
    cleanup();
}

TMenuBar *TCabineApp::initMenuBar(TRect r)
{
    r.b.y = r.a.y + 1;
    return new TCabMenuBar(r,
        *new TSubMenu("~Ф~айл", kbNoKey) +
            *new TMenuItem("~Н~овый архив...", cmNewArchive, kbCtrlN, hcNoContext, "Ctrl-N") +
            *new TMenuItem("~О~ткрыть архив...", cmOpenArchive, kbCtrlO, hcNoContext, "Ctrl-O") +
            newLine() +
            *new TMenuItem("~С~войства архива...", cmArchiveInfo, kbAltI, hcNoContext, "Alt-I") +
            *new TMenuItem("О~б~новить", cmReload, kbCtrlR, hcNoContext, "Ctrl-R") +
            newLine() +
            *new TMenuItem("В~ы~ход", cmQuit, kbAltX, hcNoContext, "Alt-X") +
        *new TSubMenu("~К~оманды", kbNoKey) +
            *new TMenuItem("~Д~обавить в архив...", cmAddFiles, kbF6, hcNoContext, "F6") +
            *new TMenuItem("~И~звлечь...", cmExtract, kbF5, hcNoContext, "F5") +
            *new TMenuItem("~У~далить", cmDeleteFiles, kbF8, hcNoContext, "F8, Del") +
            newLine() +
            *new TMenuItem("~П~росмотр", cmViewFile, kbF3, hcNoContext, "F3") +
            *new TMenuItem("~О~ткрыть", cmOpenFile, kbNoKey, hcNoContext, "Enter") +
            *new TMenuItem("Перейти ~в~верх", cmGoUp, kbCtrlPgUp, hcNoContext, "Ctrl-PgUp") +
            newLine() +
            *new TMenuItem("~Т~естировать архив", cmTestArchive, kbAltT, hcNoContext, "Alt-T") +
        *new TSubMenu("~В~ыделение", kbNoKey) +
            *new TMenuItem("~В~ыделить всё", cmSelectAll, kbCtrlA, hcNoContext, "Ctrl-A") +
            *new TMenuItem("~С~нять выделение", cmUnselectAll, kbNoKey) +
            *new TMenuItem("~И~нвертировать", cmInvertSel, kbNoKey, hcNoContext, "*") +
            newLine() +
            *new TMenuItem("Выделить по ~м~аске...", cmSelectMask, kbNoKey, hcNoContext, "+") +
            *new TMenuItem("Снять по м~а~ске...", cmUnselectMask, kbNoKey, hcNoContext, "-") +
        *new TSubMenu("~С~ортировка", kbNoKey) +
            *new TMenuItem("По ~и~мени", cmSortName, kbCtrlF3, hcNoContext, "Ctrl-F3") +
            *new TMenuItem("По ~т~ипу", cmSortExt, kbCtrlF4, hcNoContext, "Ctrl-F4") +
            *new TMenuItem("По ~д~ате", cmSortDate, kbCtrlF5, hcNoContext, "Ctrl-F5") +
            *new TMenuItem("По ~р~азмеру", cmSortSize, kbCtrlF6, hcNoContext, "Ctrl-F6") +
            *new TMenuItem("~Б~ез сортировки", cmSortNone, kbCtrlF7, hcNoContext, "Ctrl-F7") +
        *new TSubMenu("~Н~астройки", kbNoKey) +
            *new TMenuItem("~П~араметры...", cmOptions, kbNoKey) +
            *new TMenuItem("~О~ программе...", cmAbout, kbNoKey));
}

TStatusLine *TCabineApp::initStatusLine(TRect r)
{
    r.a.y = r.b.y - 1;
    return new TStatusLine(r,
        *new TStatusDef(0, 0xFFFF) +
            *new TStatusItem("~Alt-X~ Выход", kbAltX, cmQuit) +
            *new TStatusItem("~F3~ Просмотр", kbF3, cmViewFile) +
            *new TStatusItem("~F5~ Извлечь", kbF5, cmExtract) +
            *new TStatusItem("~F6~ В архив", kbF6, cmAddFiles) +
            *new TStatusItem("~F8~ Удалить", kbF8, cmDeleteFiles) +
            *new TStatusItem("~F10~ Меню", kbF10, cmMenu) +
            *new TStatusItem(0, kbAltF3, cmClose));
}

void TCabineApp::getEvent(TEvent &event)
{
    TApplication::getEvent(event);
    translateCyrillicHotkey(event, TopView());
}

void TCabineApp::idle()
{
    TApplication::idle();
    if (!started)
    {
        started = true;
        start();
    }

    // Команды панели доступны, только когда она активна.
    bool want = panel && deskTop->current == panel;
    if (want != panelCommands)
    {
        TCommandSet cs = panelCommandSet();
        if (want)
            enableCommands(cs);
        else
            disableCommands(cs);
        panelCommands = want;
    }

    if (!pending.empty())
    {
        std::vector<std::string> paths;
        paths.swap(pending);
        for (const std::string &p : paths)
            openPath(p);
    }
}

// Начальное положение панели: аргумент командной строки либо место,
// на котором программу закрыли в прошлый раз.
void TCabineApp::start()
{
    if (batch != BatchMode::None)
    {
        runBatch();
        return;
    }
    // Панель на весь рабочий стол, растягивается вместе с окном консоли.
    panel = new TPanelWindow(deskTop->getExtent());
    panel->archivesFirst = fsu::loadSetting("ArchivesFirst", "0") == "1";
    deskTop->insert(panel);
    if (!pending.empty())
        return;
    std::string dir = fsu::loadSetting("PanelDir", fsu::currentDir());
    std::string archive = fsu::loadSetting("PanelArchive", "");
    std::string inner = fsu::loadSetting("PanelInner", "");
    if (!archive.empty() && fsu::fileExists(archive) && panel->openArchive(archive, inner))
        return;
    panel->openDirectory(fsu::existingDir(dir));
}

void TCabineApp::handleEvent(TEvent &event)
{
    TApplication::handleEvent(event);
    if (event.what != evCommand)
        return;
    switch (event.message.command)
    {
        case cmNewArchive:  newArchive(); break;
        case cmOpenArchive: openArchiveDialog(); break;
        case cmOptions:     options(); break;
        case cmAbout:       about(); break;
        default:
            return;
    }
    clearEvent(event);
}

void TCabineApp::openLater(const std::string &path)
{
    pending.push_back(path);
}

void TCabineApp::openPath(const std::string &pathIn)
{
    if (!panel)
        return;
    std::string path = fsu::fullPath(pathIn);
    bool ok = false;
    if (fsu::dirExists(path))
        ok = panel->openDirectory(path);
    else if (fsu::fileExists(path))
        ok = panel->openArchive(path);
    else
        showError("Не найден файл или директория\n" + path);
    if (!ok && panel->items.empty())
        panel->openDirectory(fsu::existingDir(path));
    panel->select();
}

void TCabineApp::newArchive()
{
    std::string path;
    if (!chooseFile("Новый архив", "*.cab", true, path))
        return;
    if (fsu::extension(path).empty())
        path += ".cab";
    if (fsu::dirExists(path))
    {
        showError("Существует директория с таким именем\n" + path);
        return;
    }
    if (fsu::fileExists(path) &&
        !confirm("Файл уже существует:\n" + path + "\n\nЗаменить его новым архивом?"))
        return;

    AddOptions opt;
    opt.compression = defaultCompression;
    if (!addFilesDialog("Новый архив", opt))
        return;
    std::vector<CabSource> sources;
    if (!collectSources(opt, sources))
        return;
    CompressionSpec comp = opt.compression;
    if (runWithProgress("Создание архива", [&](CabProgress *p, std::string &err) {
            return cabCreate(path, sources, comp, p, err);
        }))
        openPath(path);
}

void TCabineApp::openArchiveDialog()
{
    std::string path;
    if (chooseFile("Открыть архив", "*.cab", false, path))
        openPath(path);
}

void TCabineApp::options()
{
    Settings s;
    s.associate = isCabAssociated();
    s.archivesFirst = panel ? panel->archivesFirst : fsu::loadSetting("ArchivesFirst", "0") == "1";
    s.compression = defaultCompression;
    bool wasAssociated = s.associate;
    if (!settingsDialog(s))
        return;

    defaultCompression = s.compression;
    fsu::saveSetting("Compression", compressionName(defaultCompression));
    fsu::saveSetting("ArchivesFirst", s.archivesFirst ? "1" : "0");
    if (panel && panel->archivesFirst != s.archivesFirst)
    {
        panel->archivesFirst = s.archivesFirst;
        panel->refresh();
    }

    if (s.associate != wasAssociated)
    {
        std::string err;
        if (!setCabAssociation(s.associate, fsu::exePath(), err))
            showError(err);
        else if (s.associate && cabUserChoiceOverrides())
            showInfo("Ассоциация записана, но для файлов .cab в Windows выбрана другая программа "
                     "(«Открыть с помощью» → «Всегда использовать»).\n"
                     "Чтобы двойной щелчок открывал Cabine, необходимо выбрать её там.");
    }
}

void TCabineApp::about()
{
    textDialog("О программе",
               "\003Cabine " CABINE_VERSION "\n"
               "\n"
               "\003Файловый менеджер и архиватор CAB\n"
               "\003" CABINE_COPYRIGHT);
}

void TCabineApp::setBatch(BatchMode mode, const std::vector<std::string> &paths)
{
    batch = mode;
    batchPaths = paths;
}

void TCabineApp::runBatch()
{
    // При вызове из Проводника видно только окно хода: меню и строка состояния
    // здесь ни к чему, рабочий стол занимает весь экран.
    menuBar->hide();
    statusLine->hide();
    deskTop->changeBounds(getExtent());
    deskTop->drawView();
    if (batch == BatchMode::Add)
        batchAdd();
    else
        batchExtract();
    batchPaths.clear();
    TEvent e;
    e.what = evCommand;
    e.message.command = cmQuit;
    e.message.infoPtr = nullptr;
    putEvent(e);
}

// -a: файлы и директории помещаются в архив рядом с ними. Имя архива — по
// единственному объекту или по директории, в которой лежат несколько объектов.
// Существующий CAB с таким именем дополняется (одноимённые файлы заменяются).
void TCabineApp::batchAdd()
{
    std::vector<std::string> items;
    std::string missing;
    for (const std::string &p : batchPaths)
    {
        std::string full = fsu::fullPath(p);
        while (full.size() > 3 && (full.back() == '\\' || full.back() == '/'))
            full.pop_back();
        if (fsu::fileExists(full) || fsu::dirExists(full))
            items.push_back(full);
        else
            missing += "\n" + full;
    }
    if (!missing.empty())
        showError("Не найдены:" + missing);
    if (items.empty())
        return;

    std::string dir = fsu::dirName(items[0]);
    std::string name;
    if (items.size() == 1)
        name = fsu::dirExists(items[0]) ? fsu::baseName(items[0]) : fsu::stripExt(fsu::baseName(items[0]));
    else if (!fsu::isRootDir(dir))
        name = fsu::baseName(dir);
    if (name.empty() || name.find(':') != std::string::npos)
        name = "archive";

    // Занятое не-CAB файлом или директорией имя заменяется на «имя (2).cab» и т. д.
    std::string cab = fsu::joinPath(dir, name + ".cab");
    CabInfo existing;
    std::string err;
    bool exists = fsu::fileExists(cab) && cabRead(cab, existing, err);
    for (int n = 2; (fsu::fileExists(cab) && !exists) || fsu::dirExists(cab); ++n)
    {
        cab = fsu::joinPath(dir, name + " (" + std::to_string(n) + ").cab");
        exists = fsu::fileExists(cab) && cabRead(cab, existing, err);
    }
    if (exists && (existing.hasPrev || existing.hasNext))
    {
        showError("Изменение многотомных архивов не поддерживается\n" + cab);
        return;
    }

    AddOptions opt;
    opt.items = items;
    opt.recurse = true;
    opt.keepPaths = true;
    opt.compression = exists && !existing.entries.empty() ? existing.compression : defaultCompression;
    std::vector<CabSource> sources;
    if (!collectSources(opt, sources))
        return;
    std::string self = fsu::upper(cab);
    sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const CabSource &s) {
        return fsu::upper(s.diskPath) == self;
    }), sources.end());
    if (sources.empty())
        return;

    CompressionSpec comp = opt.compression;
    runWithProgress(exists ? "Добавление в архив" : "Создание архива",
        [&](CabProgress *p, std::string &e) {
            p->onStage(fsu::baseName(cab));
            return exists ? cabUpdate(cab, {}, sources, comp, p, e)
                          : cabCreate(cab, sources, comp, p, e);
        }, kBatchHoldMs);
}

// -x: каждый архив распаковывается в директорию, где он лежит.
void TCabineApp::batchExtract()
{
    std::vector<std::string> cabs;
    std::string missing;
    for (const std::string &p : batchPaths)
    {
        std::string full = fsu::fullPath(p);
        if (fsu::fileExists(full))
            cabs.push_back(full);
        else
            missing += "\n" + full;
    }
    if (!missing.empty())
        showError("Не найдены:" + missing);
    if (cabs.empty())
        return;
    runWithProgress("Извлечение", [&](CabProgress *p, std::string &err) {
        for (const std::string &cab : cabs)
        {
            p->onStage("Извлечение " + fsu::baseName(cab));
            std::string e;
            if (!cabExtract(cab, fsu::dirName(cab), {}, true, Overwrite::Ask, p, e))
            {
                if (e == kCabCancelled)
                {
                    err = e;
                    return false;
                }
                err += (err.empty() ? "" : "\n\n") + fsu::baseName(cab) + ": " + e;
            }
        }
        return err.empty();
    }, kBatchHoldMs);
}

std::string TCabineApp::newTempDir()
{
    if (tempRoot.empty())
    {
        tempRoot = fsu::makeTempDir();
        if (tempRoot.empty())
            return {};
    }
    std::string dir = fsu::joinPath(tempRoot, std::to_string(++tempCounter));
    return fsu::makeDirs(dir) ? dir : std::string();
}

void TCabineApp::cleanup()
{
    if (panel && started)
        panel->saveState();
    panel = nullptr;
    if (!tempRoot.empty())
    {
        fsu::removeTree(tempRoot);
        tempRoot.clear();
    }
}
