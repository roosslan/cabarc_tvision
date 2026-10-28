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
#include "archive.h"
#include "commands.h"
#include "dialogs.h"
#include "fsutil.h"
#include "format.h"
#include "hotkeys.h"
#include "panel.h"
#include "sfxstub.h"
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
    defaultCompression = compressionFromName(fsu::loadSetting("Compression", "lzx21"));
    setPasswordPrompt(askPassword);
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
            *new TMenuItem("~Д~обавить в архив...", cmAddFiles, kbAltA, hcNoContext, "Alt-A") +
            *new TMenuItem("~И~звлечь...", cmExtract, kbAltE, hcNoContext, "Alt-E") +
            *new TMenuItem("~У~далить", cmDeleteFiles, kbF8, hcNoContext, "F8, Del") +
            *new TMenuItem("Переи~м~еновать...", cmRename, kbShiftF6, hcNoContext, "Shift-F6, F2") +
            newLine() +
            *new TMenuItem("~П~росмотр", cmViewFile, kbF3, hcNoContext, "F3") +
            *new TMenuItem("~Р~едактировать", cmEditFile, kbF4, hcNoContext, "F4") +
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
            *new TStatusItem("~F4~ Правка", kbF4, cmEditFile) +
            *new TStatusItem("~Alt-E~ Извлечь", kbAltE, cmExtract) +
            *new TStatusItem("~F8~ Удалить", kbF8, cmDeleteFiles) +
            *new TStatusItem("~F9~ Меню", kbF9, cmMenu) +
            *new TStatusItem(0, kbAltF3, cmClose) +
            *new TStatusItem(0, kbF2, cmRename));
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
    opt.allowSfx = true;
    opt.sfx = fsu::upper(fsu::extension(path)) == "EXE";
    opt.archivePath = path;
    if (!addFilesDialog("Новый архив", opt))
        return;
    if (!confirmChangedTarget(path, opt.archivePath))
        return;
    path = opt.archivePath;
    std::string stub;
    if (opt.sfx && !prepareSfx(path, stub))
        return;
    if (!confirmVolumes(path, opt.volumeSize))
        return;
    std::vector<CabSource> sources;
    if (!collectSources(opt, sources))
        return;
    CompressionSpec comp = opt.compression;
    uint64_t volume = opt.volumeSize;
    if (runWithProgress("Создание архива", [&](CabProgress *p, std::string &err) {
            return cabCreate(path, sources, comp, p, err, stub, volume);
        }))
        openPath(path);
}

void TCabineApp::openArchiveDialog()
{
    std::string path;
    if (chooseFile("Открыть архив", "*.cab;*.zip;*.rar;*.7z;*.gz;*.tgz;*.tar", false, path))
        openPath(path);
}

void TCabineApp::options()
{
    Settings s;
    s.associateCab = isAssociated("cab");
    s.associateZip = isAssociated("zip");
    s.associateRar = isAssociated("rar");
    s.associate7z = isAssociated("7z");
    s.associateTar = isAssociated("tar") && isAssociated("tgz") && isAssociated("gz");
    s.archivesFirst = panel ? panel->archivesFirst : fsu::loadSetting("ArchivesFirst", "0") == "1";
    s.compression = defaultCompression;
    s.editor = fsu::loadSetting("Editor", "vim");
    Settings was = s;
    if (!settingsDialog(s))
        return;

    defaultCompression = s.compression;
    fsu::saveSetting("Compression", compressionName(defaultCompression));
    fsu::saveSetting("ArchivesFirst", s.archivesFirst ? "1" : "0");
    fsu::saveSetting("Editor", s.editor);
    if (panel && panel->archivesFirst != s.archivesFirst)
    {
        panel->archivesFirst = s.archivesFirst;
        panel->refresh();
    }

    // Ассоциации меняются только у тех расширений, где изменилась галочка.
    struct { const char *ext; bool now, before; } assoc[] = {
        {"cab", s.associateCab, was.associateCab},
        {"zip", s.associateZip, was.associateZip},
        {"rar", s.associateRar, was.associateRar},
        {"7z", s.associate7z, was.associate7z},
        {"tar", s.associateTar, was.associateTar},
        {"tgz", s.associateTar, was.associateTar},
        {"gz", s.associateTar, was.associateTar},
    };
    std::string overridden;
    for (const auto &a : assoc)
    {
        if (a.now == a.before)
            continue;
        std::string err;
        if (!setAssociation(a.ext, a.now, fsu::exePath(), err))
            showError(err);
        else if (a.now && userChoiceOverrides(a.ext))
            overridden += std::string(overridden.empty() ? "" : ", ") + "." + a.ext;
    }
    if (!overridden.empty())
        showInfo("Ассоциация записана, но для файлов " + overridden + " в Windows выбрана другая "
                 "программа («Открыть с помощью» → «Всегда использовать»).\n"
                 "Чтобы двойной щелчок открывал Cabine, необходимо выбрать её там.");
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
    if (batch == BatchMode::Add || batch == BatchMode::AddAsk)
        batchAdd(batch == BatchMode::AddAsk);
    else if (batch == BatchMode::Convert)
        batchConvert();
    else
        batchExtract(batch == BatchMode::ExtractToDir, batch == BatchMode::ExtractAsk);
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
void TCabineApp::batchAdd(bool ask)
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

    // -ai: окно «Добавить в архив»: имя архива, степень сжатия, размер тома и
    // самораспаковывающийся архив. Имя с расширением .exe — тоже самораспаковывающийся.
    std::string stub;
    CompressionSpec comp = exists && !existing.entries.empty() ? existing.compression : defaultCompression;
    uint64_t volume = 0;
    if (ask)
    {
        ArchiveTarget t;
        t.path = cab;
        t.compression = comp;
        for (;;)
        {
            if (!archiveTargetDialog("Добавить в архив", t))
                return;
            std::string path = fsu::fullPath(fsu::isAbsolutePath(t.path) ? t.path : fsu::joinPath(dir, t.path));
            if (fsu::extension(path).empty())
                path += ".cab";
            bool sfx = t.sfx || fsu::upper(fsu::extension(path)) == "EXE";
            if (sfx)
                path = sfxPath(path);
            if (fsu::dirExists(path))
            {
                showError("Существует директория с таким именем\n" + path);
                continue;
            }
            existing = CabInfo();
            exists = fsu::fileExists(path);
            if (exists && !cabRead(path, existing, err))
            {
                showError("Файл существует и не является CAB-архивом\n" + path + "\n\n" + err);
                continue;
            }
            if (exists && (t.volumeSize || (t.sfx && existing.offset == 0)))
            {
                showError("Архив уже существует:\n" + path + "\n\nМноготомный и самораспаковывающийся "
                          "архивы создаются только новыми — необходимо указать другое имя.");
                continue;
            }
            if (!exists && sfx && !sfxStub(stub, err))
            {
                showError(err);
                return;
            }
            if (!exists && !confirmVolumes(path, t.volumeSize))
                continue;
            cab = path;
            comp = t.compression;
            volume = exists ? 0 : t.volumeSize;
            break;
        }
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
    opt.compression = comp;
    std::vector<CabSource> sources;
    if (!collectSources(opt, sources))
        return;
    std::string self = fsu::upper(cab);
    sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const CabSource &s) {
        return fsu::upper(s.diskPath) == self;
    }), sources.end());
    if (sources.empty())
        return;

    runWithProgress(exists ? "Добавление в архив" : "Создание архива",
        [&](CabProgress *p, std::string &e) {
            p->onStage(fsu::baseName(cab));
            return exists ? cabUpdate(cab, {}, sources, comp, p, e)
                          : cabCreate(cab, sources, comp, p, e, stub, volume);
        }, kBatchHoldMs);
}

// -x: каждый архив распаковывается в директорию, где он лежит;
// -e: в поддиректорию с именем архива (без расширения);
// -ei: в директорию, указанную в окне. Для одного архива предлагается
// поддиректория с его именем; для нескольких — общая директория, в которой
// каждый архив распаковывается в свою поддиректорию.
void TCabineApp::batchExtract(bool toOwnDir, bool ask)
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
    std::string target;
    if (ask)
    {
        bool single = cabs.size() == 1;
        target = single ? fsu::joinPath(fsu::dirName(cabs[0]), archiveStem(cabs[0]))
                        : fsu::dirName(cabs[0]);
        if (!askPath("Извлечь", single ? "~И~звлечь в:" : "~И~звлечь в (каждый архив — в свою поддиректорию):",
                     PathKind::Directory, nullptr, target))
            return;
        target = fsu::fullPath(fsu::isAbsolutePath(target) ? target
                                                           : fsu::joinPath(fsu::dirName(cabs[0]), target));
        toOwnDir = !single;
    }
    runWithProgress("Извлечение", [&](CabProgress *p, std::string &err) {
        for (const std::string &cab : cabs)
        {
            p->onStage("Извлечение " + fsu::baseName(cab));
            std::string dest = ask ? target : fsu::dirName(cab);
            if (toOwnDir)
                dest = fsu::joinPath(dest, archiveStem(cab));
            std::string e;
            if (!archExtract(cab, dest, {}, true, Overwrite::Ask, p, e))
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

// -c: архив другого формата (ZIP, RAR, 7Z, GZ, TAR, TAR.GZ) распаковывается во
// временную директорию и упаковывается в <имя>.cab рядом с ним со структурой
// директорий как в исходном, с максимальным сжатием (LZX:21). Исходный архив
// не удаляется. Занятое имя заменяется на «имя (2).cab» и т. д.
void TCabineApp::batchConvert()
{
    std::vector<std::string> archives;
    std::string missing;
    for (const std::string &p : batchPaths)
    {
        std::string full = fsu::fullPath(p);
        if (fsu::fileExists(full))
            archives.push_back(full);
        else
            missing += "\n" + full;
    }
    if (!missing.empty())
        showError("Не найдены:" + missing);
    if (archives.empty())
        return;

    const CompressionSpec maximum = {CabCompression::LZX, 21};
    runWithProgress("Преобразование в CAB", [&](CabProgress *p, std::string &err) {
        for (const std::string &arc : archives)
        {
            std::string name = fsu::baseName(arc), e;
            auto fail = [&](const std::string &msg) {
                err += (err.empty() ? "" : "\n\n") + name + ": " + msg;
            };
            ArchiveFormat format = archiveFormat(arc, true);
            if (format == ArchiveFormat::Cab)
            {
                fail("уже архив CAB");
                continue;
            }
            if (format == ArchiveFormat::Unknown)
            {
                fail("не является архивом ZIP, RAR, 7Z, GZ или TAR");
                continue;
            }
            std::string dir = fsu::dirName(arc), stem = archiveStem(arc);
            std::string cab = fsu::joinPath(dir, stem + ".cab");
            for (int n = 2; fsu::fileExists(cab) || fsu::dirExists(cab); ++n)
                cab = fsu::joinPath(dir, stem + " (" + std::to_string(n) + ").cab");

            std::string temp = newTempDir();
            if (temp.empty())
            {
                fail("не удалось создать временную директорию");
                continue;
            }
            p->onStage("Распаковка " + name);
            bool ok = archExtract(arc, temp, {}, true, Overwrite::Always, p, e);
            std::vector<fsu::FoundFile> found;
            if (ok && !fsu::collectFiles(fsu::joinPath(temp, "*"), true, true, found, e))
                ok = false;
            if (ok && found.empty())
            {
                e = "в архиве нет файлов";
                ok = false;
            }
            if (ok)
            {
                std::vector<CabSource> sources;
                for (const fsu::FoundFile &f : found)
                    sources.push_back({f.path, f.name});
                p->onStage("Создание " + fsu::baseName(cab));
                ok = cabCreate(cab, sources, maximum, p, e);
                if (!ok)
                    fsu::removeFile(cab);   // недоделанный архив не оставляется
            }
            fsu::removeTree(temp);
            if (!ok)
            {
                if (e == kCabCancelled)
                {
                    err = e;
                    return false;
                }
                fail(e);
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
