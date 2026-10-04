#define Uses_TKeys
#define Uses_TEvent
#define Uses_TRect
#define Uses_TPoint
#define Uses_TView
#define Uses_TGroup
#define Uses_TWindow
#define Uses_TFrame
#define Uses_TListViewer
#define Uses_TScrollBar
#define Uses_TDrawBuffer
#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_MsgBox
#include <tvision/tv.h>

#include <algorithm>
#include <unordered_map>

#include "app.h"
#include "commands.h"
#include "dialogs.h"
#include "format.h"
#include "fsutil.h"
#include "panel.h"
#include "viewer.h"

namespace {

// Цвета панели в стиле Norton Commander (атрибуты BIOS).
const TColorAttr cNormal = 0x1B;
const TColorAttr cDir = 0x1F;
const TColorAttr cArchive = 0x1D;
const TColorAttr cMarked = 0x1E;
const TColorAttr cCursor = 0x30;
const TColorAttr cCursorDir = 0x3F;
const TColorAttr cCursorMarked = 0x3E;
const TColorAttr cHeader = 0x1E;
const TColorAttr cHeaderSorted = 0x1F;
const TColorAttr cInfo = 0x70;

const int kMaxRows = 32767;     // TListViewer нумерует строки типом short
const uint64_t kMaxViewSize = 16 * 1024 * 1024;

bool isCab(const std::string &name)
{
    return fsu::upper(fsu::extension(name)) == "CAB";
}

std::string pluralDirs(uint64_t n)
{
    uint64_t mod100 = n % 100, mod10 = n % 10;
    const char *word = "директорий";
    if (mod100 < 11 || mod100 > 14)
    {
        if (mod10 == 1)
            word = "директория";
        else if (mod10 >= 2 && mod10 <= 4)
            word = "директории";
    }
    return formatNumber(n) + " " + word;
}

const char *columnTitle(ColumnId id)
{
    switch (id)
    {
        case ColumnId::Name: return "Имя";
        case ColumnId::Size: return "Размер";
        case ColumnId::Date: return "Изменён";
        default:             return "Атр";
    }
}

std::string cellText(const PanelItem &it, ColumnId id, bool archiveMode)
{
    switch (id)
    {
        case ColumnId::Name:
            if (it.kind == ItemKind::Parent)
                return "..";
            if (it.kind == ItemKind::Drive)
                return "[" + it.name.substr(0, 2) + "]";
            return it.name;
        case ColumnId::Size:
            switch (it.kind)
            {
                case ItemKind::Parent: return "Вверх";
                case ItemKind::Dir:    return "Директория";
                case ItemKind::Drive:  return "Диск";
                default:               return formatNumber(it.size);
            }
        case ColumnId::Date:
            if (it.kind == ItemKind::Parent || it.kind == ItemKind::Drive || it.date == 0)
                return {};
            return formatDateTime(it.date, it.time);
        default:
            if (it.kind == ItemKind::Parent || it.kind == ItemKind::Drive ||
                (archiveMode && it.kind == ItemKind::Dir))
                return {};
            return formatAttr(it.attribs);
    }
}

// Текст ячейки: размер выравнивается вправо, остальное влево.
void putCell(TDrawBuffer &b, const Column &col, const std::string &text, TColorAttr color)
{
    int x = col.x;
    if (col.id == ColumnId::Size)
        x += std::max(0, col.width - strwidth(text));
    b.moveStr(x, text, color, col.width - (x - col.x));
}

void postCommand(TView *view, ushort command)
{
    TEvent e;
    e.what = evCommand;
    e.message.command = command;
    e.message.infoPtr = nullptr;
    view->putEvent(e);
}

PanelItem makeItem(const std::string &name, ItemKind kind)
{
    PanelItem it;
    it.name = name;
    it.kind = kind;
    return it;
}

std::string propertiesText(const CabInfo &i, const std::string &display)
{
    std::string t;
    t += "Архив:            " + fsu::baseName(display) + "\n";
    t += "Директория:       " + fsu::dirName(display) + "\n";
    t += "Размер:           " + formatNumber(i.fileSize) + " байт (" + formatSize(i.fileSize) + ")\n";
    t += "Файлов:           " + formatNumber(i.entries.size()) + "\n";
    t += "Исходный размер:  " + formatNumber(i.totalSize) + " байт (" + formatSize(i.totalSize) + ")\n";
    t += "Степень сжатия:   " + formatRatio(i.fileSize, i.totalSize) + "\n";
    t += "Метод сжатия:     " + i.method + "\n";
    t += "Блоков (папок):   " + formatNumber(i.folders) + "\n";
    t += "Набор:            ID " + std::to_string(i.setID) + ", том " + std::to_string(i.iCabinet + 1);
    if (i.hasPrev)
        t += "\nПредыдущий том:   " + i.prevCab;
    if (i.hasNext)
        t += "\nСледующий том:    " + i.nextCab;
    return t;
}

std::string innerParent(const std::string &inner)
{
    size_t pos = inner.rfind('\\');
    return pos == std::string::npos ? std::string() : inner.substr(0, pos);
}

} // namespace

std::vector<Column> layoutColumns(int width)
{
    const int wSize = 13, wDate = 16, wAttr = 4;
    std::vector<std::pair<ColumnId, int>> spec;
    int rest;
    if ((rest = width - wSize - wDate - wAttr - 3) >= 12)
        spec = {{ColumnId::Name, rest}, {ColumnId::Size, wSize}, {ColumnId::Date, wDate},
                {ColumnId::Attr, wAttr}};
    else if ((rest = width - wSize - wDate - 2) >= 12)
        spec = {{ColumnId::Name, rest}, {ColumnId::Size, wSize}, {ColumnId::Date, wDate}};
    else if ((rest = width - wSize - 1) >= 8)
        spec = {{ColumnId::Name, rest}, {ColumnId::Size, wSize}};
    else
        spec = {{ColumnId::Name, std::max(width, 1)}};

    std::vector<Column> cols;
    int x = 0;
    for (const auto &s : spec)
    {
        cols.push_back({s.first, x, s.second});
        x += s.second + 1;
    }
    return cols;
}

// ---------------------------------------------------------------------------
// Заголовок колонок
// ---------------------------------------------------------------------------

class THeaderView : public TView
{
public:
    THeaderView(const TRect &bounds, TPanelWindow *win) :
        TView(bounds),
        win(win)
    {
        growMode = gfGrowHiX;
    }

    void draw() override
    {
        TDrawBuffer b;
        b.moveChar(0, ' ', cHeader, size.x);
        std::vector<Column> cols = layoutColumns(size.x);
        for (size_t i = 0; i < cols.size(); ++i)
        {
            const Column &col = cols[i];
            std::string title = columnTitle(col.id);
            SortKey k = win->sortKey;
            bool sorted = (col.id == ColumnId::Name && k == SortKey::Name) ||
                          (col.id == ColumnId::Size && k == SortKey::Size) ||
                          (col.id == ColumnId::Date && k == SortKey::Date);
            if (col.id == ColumnId::Name && k == SortKey::Ext)
            {
                title = "Имя (по типу)";
                sorted = true;
            }
            if (sorted)
                title += win->descending ? " ▼" : " ▲";
            putCell(b, col, title, sorted ? cHeaderSorted : cHeader);
            if (i + 1 < cols.size())
                b.moveChar(col.x + col.width, '\xB3', cHeader, 1);
        }
        writeLine(0, 0, size.x, 1, b);
    }

    void handleEvent(TEvent &ev) override
    {
        TView::handleEvent(ev);
        if (ev.what == evMouseDown)
        {
            TPoint p = makeLocal(ev.mouse.where);
            for (const Column &col : layoutColumns(size.x))
                if (p.x >= col.x && p.x <= col.x + col.width)
                {
                    win->sortByColumn(col.id);
                    break;
                }
            clearEvent(ev);
        }
    }

private:
    TPanelWindow *win;
};

// ---------------------------------------------------------------------------
// Строка сводки
// ---------------------------------------------------------------------------

class TInfoBar : public TView
{
public:
    TInfoBar(const TRect &bounds, TPanelWindow *win) :
        TView(bounds),
        win(win)
    {
        growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
    }

    void draw() override
    {
        size_t files = 0, dirs = 0, marked = 0;
        uint64_t markedSize = 0;
        for (size_t i = 0; i < win->items.size(); ++i)
        {
            const PanelItem &it = win->items[i];
            if (it.kind == ItemKind::File)
                ++files;
            else if (it.kind == ItemKind::Dir)
                ++dirs;
            if (win->marked[i])
            {
                ++marked;
                markedSize += it.size;
            }
        }
        std::string left;
        if (marked)
            left = " Выбрано: " + formatNumber(marked) + " (" + formatSize(markedSize) + ")";
        else if (files && dirs)
            left = " " + pluralFiles(files) + ", " + pluralDirs(dirs);
        else if (files || dirs)
            left = " " + (files ? pluralFiles(files) : pluralDirs(dirs));
        else
            left = " Пусто";
        if (win->items.size() > (size_t) kMaxRows)
            left += ", показаны первые " + formatNumber(kMaxRows);

        std::string right;
        if (win->archiveMode)
        {
            const CabInfo &info = win->info;
            right = formatSize(info.totalSize) + " → " + formatSize(info.fileSize) + " (" +
                formatRatio(info.fileSize, info.totalSize) + ")  " + info.method + " ";
            if (win->readOnly())
                right = "только чтение  " + right;
        }
        else
            right = "Свободно: " + formatSize(freeBytes) + " ";

        TDrawBuffer b;
        b.moveChar(0, ' ', cInfo, size.x);
        b.moveStr(0, left, cInfo, size.x);
        int rw = strwidth(right);
        if (strwidth(left) + rw + 2 <= size.x)
            b.moveStr(size.x - rw, right, cInfo);
        writeLine(0, 0, size.x, 1, b);
    }

    uint64_t freeBytes = 0;

private:
    TPanelWindow *win;
};

// ---------------------------------------------------------------------------
// Список панели
// ---------------------------------------------------------------------------

class TPanelList : public TListViewer
{
public:
    TPanelList(const TRect &bounds, TScrollBar *vsb, TPanelWindow *win) :
        TListViewer(bounds, 1, nullptr, vsb),
        win(win)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        eventMask |= evMouseWheel;
    }

    void updateRange()
    {
        topItem = 0;
        setRange((short) std::min<size_t>(win->items.size(), kMaxRows));
    }

    void draw() override
    {
        bool active = (state & (sfSelected | sfActive)) == (sfSelected | sfActive);
        std::vector<Column> cols = layoutColumns(size.x);
        for (int y = 0; y < size.y; ++y)
        {
            int row = topItem + y;
            const PanelItem *it = row < range ? &win->items[row] : nullptr;
            TColorAttr c = cNormal;
            if (it)
            {
                bool mark = win->marked[row] != 0;
                bool dir = it->kind != ItemKind::File;
                bool cursor = active && row == focused;
                if (mark)
                    c = cursor ? cCursorMarked : cMarked;
                else if (cursor)
                    c = dir ? cCursorDir : cCursor;
                else if (dir)
                    c = cDir;
                else if (isCab(it->name))
                    c = cArchive;
            }
            TDrawBuffer b;
            b.moveChar(0, ' ', c, size.x);
            for (size_t i = 0; i < cols.size(); ++i)
            {
                if (it)
                    putCell(b, cols[i], cellText(*it, cols[i].id, win->archiveMode), c);
                if (i + 1 < cols.size())
                    b.moveChar(cols[i].x + cols[i].width, '\xB3', c, 1);
            }
            writeLine(0, y, size.x, 1, b);
        }
    }

    void handleEvent(TEvent &ev) override
    {
        if (ev.what == evMouseWheel)
        {
            if (vScrollBar)
                vScrollBar->handleEvent(ev);
            clearEvent(ev);
            return;
        }
        if (ev.what == evMouseDown && (ev.mouse.buttons & mbRightButton))
        {
            int row = topItem + makeLocal(ev.mouse.where).y;
            if (row < range)
            {
                focusItemNum((short) row);
                toggle(row);
            }
            clearEvent(ev);
            return;
        }
        if (ev.what == evKeyDown)
        {
            ushort cmd = 0;
            switch (ev.keyDown.keyCode)
            {
                case kbIns:   toggleAndAdvance(); clearEvent(ev); return;
                case kbEnter: cmd = cmOpenFile; break;
                case kbBack:  cmd = cmGoUp; break;
                case kbDel:   cmd = cmDeleteFiles; break;
                case kbHome:
                    focusItemNum(0);
                    drawView();
                    clearEvent(ev);
                    return;
                case kbEnd:
                    focusItemNum(range - 1);
                    drawView();
                    clearEvent(ev);
                    return;
            }
            switch (ev.keyDown.charScan.charCode)
            {
                case ' ': toggleAndAdvance(); clearEvent(ev); return;
                case '+': cmd = cmSelectMask; break;
                case '-': cmd = cmUnselectMask; break;
                case '*': cmd = cmInvertSel; break;
            }
            if (cmd)
            {
                postCommand(this, cmd);
                clearEvent(ev);
                return;
            }
        }
        TListViewer::handleEvent(ev);
    }

    // Двойной щелчок.
    void selectItem(short) override
    {
        postCommand(this, cmOpenFile);
    }

private:
    void toggle(int row)
    {
        if (row < 0 || row >= range)
            return;
        ItemKind k = win->items[row].kind;
        if (k != ItemKind::File && k != ItemKind::Dir)
            return;
        win->marked[row] ^= 1;
        drawView();
        message(owner, evBroadcast, cmMarksChanged, this);
    }

    void toggleAndAdvance()
    {
        toggle(focused);
        focusItemNum(focused + 1);
        drawView();
    }

    TPanelWindow *win;
};

// ---------------------------------------------------------------------------
// TPanelWindow
// ---------------------------------------------------------------------------

TPanelWindow::TPanelWindow(const TRect &bounds) :
    TWindowInit(&TWindow::initFrame),
    TWindow(bounds, "", wnNoNumber)
{
    // Панель занимает весь рабочий стол: без перемещения, масштаба и закрытия.
    flags &= ~(wfClose | wfMove | wfGrow | wfZoom);
    growMode = gfGrowHiX | gfGrowHiY;

    TRect r = getExtent().grow(-1, -1);
    header = new THeaderView(TRect(r.a.x, r.a.y, r.b.x, r.a.y + 1), this);
    insert(header);
    TScrollBar *vsb = standardScrollBar(sbVertical);
    list = new TPanelList(TRect(r.a.x, r.a.y + 1, r.b.x, r.b.y - 1), vsb, this);
    insert(list);
    infoBar = new TInfoBar(TRect(r.a.x, r.b.y - 1, r.b.x, r.b.y), this);
    insert(infoBar);
    list->select();
}

void TPanelWindow::sizeLimits(TPoint &min, TPoint &max)
{
    TWindow::sizeLimits(min, max);
    min.x = 40;
    min.y = 8;
}

void TPanelWindow::changeBounds(const TRect &bounds)
{
    TWindow::changeBounds(bounds);
    updateTitle();
}

void TPanelWindow::handleEvent(TEvent &event)
{
    TWindow::handleEvent(event);
    if (event.what == evBroadcast && event.message.command == cmMarksChanged)
    {
        infoBar->drawView();
        return;
    }
    if (event.what != evCommand)
        return;
    switch (event.message.command)
    {
        case cmAddFiles:     addFiles(); break;
        case cmExtract:      extractFiles(); break;
        case cmDeleteFiles:  deleteFiles(); break;
        case cmViewFile:     viewFile(); break;
        case cmOpenFile:     enterItem(focusedIndex()); break;
        case cmGoUp:         goUp(); break;
        case cmTestArchive:  testArchive(); break;
        case cmArchiveInfo:  showProperties(); break;
        case cmReload:       refresh(); break;
        case cmSelectAll:    markAll(true); break;
        case cmUnselectAll:  markAll(false); break;
        case cmInvertSel:    invertMarks(); break;
        case cmSelectMask:   markByMask(true); break;
        case cmUnselectMask: markByMask(false); break;
        case cmSortName:     sortBy(SortKey::Name); break;
        case cmSortExt:      sortBy(SortKey::Ext); break;
        case cmSortDate:     sortBy(SortKey::Date); break;
        case cmSortSize:     sortBy(SortKey::Size); break;
        case cmSortNone:     sortBy(SortKey::None); break;
        default:
            return;
    }
    clearEvent(event);
}

// --------------------------------------------------------------- содержимое

bool TPanelWindow::loadDirectoryItems(const std::string &dir, std::vector<PanelItem> &out)
{
    std::vector<fsu::DirItem> found;
    if (!fsu::listDirectory(dir, found))
        return false;
    out.clear();
    bool root = fsu::isRootDir(dir);
    if (!root)
        out.push_back(makeItem("..", ItemKind::Parent));
    for (const fsu::DirItem &d : found)
    {
        PanelItem it = makeItem(d.name, d.isDir ? ItemKind::Dir : ItemKind::File);
        it.size = d.isDir ? 0 : d.size;
        it.date = d.date;
        it.time = d.time;
        it.attribs = d.attribs;
        out.push_back(std::move(it));
    }
    // В корне диска — переход на другие диски.
    if (root)
        for (const std::string &drive : fsu::logicalDrives())
            if (fsu::upper(drive.substr(0, 2)) != fsu::upper(dir.substr(0, 2)))
                out.push_back(makeItem(drive, ItemKind::Drive));
    return true;
}

void TPanelWindow::loadArchiveItems()
{
    // Директории архива не хранятся отдельно: они выводятся из путей файлов.
    items.clear();
    items.push_back(makeItem("..", ItemKind::Parent));
    std::string prefix = inner.empty() ? std::string() : fsu::upper(inner) + "\\";
    size_t plen = prefix.size();
    std::unordered_map<std::string, size_t> dirs;
    for (size_t i = 0; i < info.entries.size(); ++i)
    {
        const std::string &up = upperNames[i];
        const CabEntry &e = info.entries[i];
        if (up.size() <= plen || e.name.size() <= plen || up.compare(0, plen, prefix) != 0)
            continue;
        std::string rest = e.name.substr(plen);
        size_t sep = rest.find('\\');
        if (sep == std::string::npos)
        {
            PanelItem f = makeItem(rest, ItemKind::File);
            f.size = e.size;
            f.date = e.date;
            f.time = e.time;
            f.attribs = e.attribs;
            f.entry = (int) i;
            items.push_back(std::move(f));
            continue;
        }
        if (sep == 0)
            continue;
        std::string key = up.substr(plen, sep);
        auto it = dirs.find(key);
        if (it == dirs.end())
        {
            PanelItem d = makeItem(rest.substr(0, sep), ItemKind::Dir);
            d.size = e.size;
            d.date = e.date;
            d.time = e.time;
            dirs.emplace(key, items.size());
            items.push_back(std::move(d));
        }
        else
        {
            PanelItem &d = items[it->second];
            d.size += e.size;
            if ((uint32_t(e.date) << 16 | e.time) > (uint32_t(d.date) << 16 | d.time))
            {
                d.date = e.date;
                d.time = e.time;
            }
        }
    }
}

void TPanelWindow::setArchive(CabInfo &&newInfo)
{
    info = std::move(newInfo);
    upperNames.clear();
    upperNames.reserve(info.entries.size());
    for (const CabEntry &e : info.entries)
        upperNames.push_back(fsu::upper(e.name));
}

void TPanelWindow::resort()
{
    auto num = [](uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; };
    auto rank = [](ItemKind k) {
        return k == ItemKind::Parent ? 0 : k == ItemKind::Dir ? 1 : k == ItemKind::File ? 2 : 3;
    };
    for (PanelItem &it : items)
        if (it.wName.empty() && it.kind != ItemKind::Parent)
        {
            it.wName = fsu::widen(it.name);
            it.wExt = fsu::widen(fsu::extension(it.name));
        }
    std::stable_sort(items.begin(), items.end(), [&](const PanelItem &a, const PanelItem &b) {
        int ra = rank(a.kind), rb = rank(b.kind);
        if (ra != rb)
            return ra < rb;
        if (a.kind == ItemKind::Parent || a.kind == ItemKind::Drive || sortKey == SortKey::None)
            return false;
        int c = 0;
        switch (sortKey)
        {
            case SortKey::Ext:  c = fsu::compareNatural(a.wExt, b.wExt); break;
            case SortKey::Date: c = num(uint32_t(a.date) << 16 | a.time, uint32_t(b.date) << 16 | b.time); break;
            case SortKey::Size: c = num(a.size, b.size); break;
            default: break;
        }
        if (!c)
            c = fsu::compareNatural(a.wName, b.wName);
        return descending ? c > 0 : c < 0;
    });
}

std::unordered_set<std::string> TPanelWindow::markedNames() const
{
    std::unordered_set<std::string> names;
    for (size_t i = 0; i < items.size(); ++i)
        if (marked[i])
            names.insert(items[i].name);
    return names;
}

void TPanelWindow::rebuild(const std::string &focus, const std::unordered_set<std::string> &marks)
{
    resort();
    marked.assign(items.size(), 0);
    for (size_t i = 0; i < items.size(); ++i)
        if ((items[i].kind == ItemKind::File || items[i].kind == ItemKind::Dir) &&
            marks.count(items[i].name))
            marked[i] = 1;
    infoBar->freeBytes = archiveMode ? 0 : fsu::freeSpace(fsDir);
    list->updateRange();
    focusName(focus);
    updateTitle();
    header->drawView();
    list->drawView();
    infoBar->drawView();
}

void TPanelWindow::focusName(const std::string &name)
{
    short index = 0;
    if (!name.empty())
        for (size_t i = 0; i < items.size() && i < (size_t) kMaxRows; ++i)
            if (items[i].name == name && items[i].kind != ItemKind::Parent)
            {
                index = (short) i;
                break;
            }
    list->focusItemNum(index);
}

std::string TPanelWindow::location() const
{
    if (!archiveMode)
        return fsDir;
    return inner.empty() ? archiveDisplay : archiveDisplay + "\\" + inner;
}

void TPanelWindow::updateTitle()
{
    // Длинный путь показывается с конца.
    std::string t = location();
    int maxW = std::max(10, size.x - 14);
    if (strwidth(t) > maxW)
    {
        while (!t.empty() && strwidth(t) > maxW - 1)
        {
            size_t n = 1;
            while (n < t.size() && ((unsigned char) t[n] & 0xC0) == 0x80)
                ++n;
            t.erase(0, n);
        }
        t = "…" + t;
    }
    delete[] (char *) title;
    title = newStr(t);
    if (frame)
        frame->drawView();
}

// --------------------------------------------------------------- навигация

bool TPanelWindow::openDirectory(const std::string &dir, const std::string &focus)
{
    std::vector<PanelItem> loaded;
    if (!loadDirectoryItems(dir, loaded))
    {
        showError("Не удалось прочитать директорию\n" + dir + "\n" + fsu::lastErrorText());
        return false;
    }
    archiveMode = false;
    parents.clear();
    info = CabInfo();
    upperNames.clear();
    inner.clear();
    fsDir = dir;
    lastDirectory() = dir;
    items = std::move(loaded);
    rebuild(focus, {});
    return true;
}

bool TPanelWindow::openArchive(const std::string &path, const std::string &innerDir,
                               const std::string &focus)
{
    CabInfo newInfo;
    std::string err;
    if (!cabRead(path, newInfo, err))
    {
        showError("Не удалось открыть архив\n" + path + "\n\n" + err);
        return false;
    }
    archiveMode = true;
    parents.clear();
    fsDir = fsu::dirName(path);
    lastDirectory() = fsDir;
    archiveDisplay = path;
    setArchive(std::move(newInfo));
    inner = innerDir;
    loadArchiveItems();
    if (!inner.empty() && items.size() <= 1)
    {
        // Сохранённой директории в архиве больше нет.
        inner.clear();
        loadArchiveItems();
    }
    rebuild(focus, {});
    return true;
}

void TPanelWindow::enterNested(const std::string &tempPath, const std::string &name)
{
    CabInfo newInfo;
    std::string err;
    if (!cabRead(tempPath, newInfo, err))
    {
        showError("Не удалось открыть архив\n" + name + "\n\n" + err);
        return;
    }
    std::string display = fsu::joinPath(location(), name);
    parents.push_back({info.path, inner, name, archiveDisplay});
    archiveDisplay = display;
    setArchive(std::move(newInfo));
    inner.clear();
    loadArchiveItems();
    rebuild(std::string(), {});
}

void TPanelWindow::goUp()
{
    if (!archiveMode)
    {
        if (!fsu::isRootDir(fsDir))
            openDirectory(fsu::dirName(fsDir), fsu::baseName(fsDir));
        return;
    }
    if (!inner.empty())
    {
        std::string from = fsu::baseName(inner);
        inner = innerParent(inner);
        loadArchiveItems();
        rebuild(from, {});
        return;
    }
    if (!parents.empty())
    {
        Frame f = parents.back();
        CabInfo parentInfo;
        std::string err;
        if (!cabRead(f.archive, parentInfo, err))
        {
            showError("Не удалось открыть архив\n" + f.display + "\n\n" + err);
            openDirectory(fsDir);
            return;
        }
        parents.pop_back();
        archiveDisplay = f.display;
        setArchive(std::move(parentInfo));
        inner = f.inner;
        loadArchiveItems();
        rebuild(f.focus, {});
        return;
    }
    // Выход из архива: курсор встаёт на сам архив.
    openDirectory(fsDir, fsu::baseName(info.path));
}

void TPanelWindow::refresh()
{
    int index = focusedIndex();
    std::string focus = index >= 0 ? items[index].name : std::string();
    std::unordered_set<std::string> marks = markedNames();
    if (archiveMode)
    {
        CabInfo newInfo;
        std::string err;
        if (!cabRead(info.path, newInfo, err))
        {
            showError("Не удалось перечитать архив\n" + archiveDisplay + "\n\n" + err);
            openDirectory(fsu::existingDir(fsDir));
            return;
        }
        setArchive(std::move(newInfo));
        loadArchiveItems();
        // Директория в архиве опустела — подъём до существующей.
        while (!inner.empty() && items.size() <= 1)
        {
            focus = fsu::baseName(inner);
            inner = innerParent(inner);
            loadArchiveItems();
        }
    }
    else if (!loadDirectoryItems(fsDir, items))
    {
        openDirectory(fsu::existingDir(fsDir));
        return;
    }
    rebuild(focus, marks);
}

void TPanelWindow::saveState() const
{
    std::string archive, archiveInner;
    if (archiveMode)
    {
        // Во вложенном архиве запоминается внешний: временные файлы удаляются.
        archive = parents.empty() ? info.path : parents.front().archive;
        archiveInner = parents.empty() ? inner : parents.front().inner;
    }
    fsu::saveSetting("PanelDir", fsDir);
    fsu::saveSetting("PanelArchive", archive);
    fsu::saveSetting("PanelInner", archiveInner);
}

void TPanelWindow::enterItem(int index)
{
    if (index < 0 || index >= (int) items.size())
        return;
    const PanelItem it = items[index];
    switch (it.kind)
    {
        case ItemKind::Parent:
            goUp();
            return;
        case ItemKind::Drive:
            openDirectory(it.name);
            return;
        case ItemKind::Dir:
            if (archiveMode)
            {
                inner = archiveName(it);
                loadArchiveItems();
                rebuild(std::string(), {});
            }
            else
                openDirectory(fsu::joinPath(fsDir, it.name));
            return;
        case ItemKind::File:
            break;
    }

    std::string file;
    if (archiveMode)
    {
        file = extractToTemp(info.entries[it.entry].name);
        if (file.empty())
            return;
        if (isCab(it.name))
        {
            enterNested(file, it.name);
            return;
        }
    }
    else
    {
        file = fsu::joinPath(fsDir, it.name);
        if (isCab(it.name))
        {
            openArchive(file);
            return;
        }
    }
    std::string err;
    if (!fsu::shellOpen(file, err))
        showError(err);
}

void TPanelWindow::sortBy(SortKey key)
{
    if (key == sortKey && key != SortKey::None)
        descending = !descending;
    else
    {
        sortKey = key;
        descending = false;
    }
    int index = focusedIndex();
    std::string focus = index >= 0 ? items[index].name : std::string();
    rebuild(focus, markedNames());
}

void TPanelWindow::sortByColumn(ColumnId column)
{
    switch (column)
    {
        case ColumnId::Name: sortBy(SortKey::Name); break;
        case ColumnId::Size: sortBy(SortKey::Size); break;
        case ColumnId::Date: sortBy(SortKey::Date); break;
        default: break;
    }
}

// --------------------------------------------------------------- выбор

int TPanelWindow::focusedIndex() const
{
    return list->focused >= 0 && list->focused < list->range ? list->focused : -1;
}

std::vector<int> TPanelWindow::targets() const
{
    std::vector<int> result;
    for (size_t i = 0; i < marked.size(); ++i)
        if (marked[i])
            result.push_back((int) i);
    if (result.empty())
    {
        int index = focusedIndex();
        if (index >= 0 && (items[index].kind == ItemKind::File || items[index].kind == ItemKind::Dir))
            result.push_back(index);
    }
    return result;
}

std::string TPanelWindow::archiveName(const PanelItem &item) const
{
    return fsu::joinPath(inner, item.name);
}

std::vector<std::string> TPanelWindow::entryNames(const std::vector<int> &indexes) const
{
    std::vector<std::string> names;
    for (int i : indexes)
    {
        const PanelItem &it = items[i];
        if (it.kind == ItemKind::File)
            names.push_back(info.entries[it.entry].name);
        else if (it.kind == ItemKind::Dir)
        {
            std::string prefix = fsu::upper(archiveName(it)) + "\\";
            for (size_t e = 0; e < upperNames.size(); ++e)
                if (upperNames[e].compare(0, prefix.size(), prefix) == 0)
                    names.push_back(info.entries[e].name);
        }
    }
    return names;
}

std::string TPanelWindow::focusedCab() const
{
    int index = focusedIndex();
    if (archiveMode || index < 0 || items[index].kind != ItemKind::File || !isCab(items[index].name))
        return {};
    return fsu::joinPath(fsDir, items[index].name);
}

void TPanelWindow::marksChanged()
{
    list->drawView();
    infoBar->drawView();
}

void TPanelWindow::markAll(bool mark)
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == ItemKind::File || items[i].kind == ItemKind::Dir)
            marked[i] = mark ? 1 : 0;
    marksChanged();
}

void TPanelWindow::invertMarks()
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == ItemKind::File || items[i].kind == ItemKind::Dir)
            marked[i] ^= 1;
    marksChanged();
}

void TPanelWindow::markByMask(bool mark)
{
    static std::string lastMask = "*.*";
    std::string mask = lastMask;
    if (!askMask(mark ? "Выделить по маске" : "Снять выделение по маске", mask))
        return;
    lastMask = mask;
    for (size_t i = 0; i < items.size(); ++i)
        if ((items[i].kind == ItemKind::File || items[i].kind == ItemKind::Dir) &&
            fsu::wildMatch(mask, items[i].name))
            marked[i] = mark ? 1 : 0;
    marksChanged();
}

// --------------------------------------------------------------- действия

std::string TPanelWindow::extractToTemp(const std::string &entryName)
{
    std::string dir = cabApp().newTempDir();
    if (dir.empty())
    {
        showError("Не удалось создать временную директорию");
        return {};
    }
    std::string path = info.path;
    bool ok = runWithProgress("Распаковка", [&](CabProgress *p, std::string &err) {
        p->onStage("Распаковка во временную директорию");
        return cabExtract(path, dir, {entryName}, false, Overwrite::Always, p, err);
    });
    return ok ? cabTargetPath(dir, entryName, false) : std::string();
}

void TPanelWindow::viewFile()
{
    int index = focusedIndex();
    if (index < 0)
        return;
    const PanelItem it = items[index];
    if (it.kind != ItemKind::File)
    {
        enterItem(index);
        return;
    }
    if (it.size > kMaxViewSize)
    {
        showError("Файл слишком велик для встроенного просмотра (более 16 МБ).\n"
                  "Открыть его во внешней программе можно клавишей Enter.");
        return;
    }
    std::string file, title;
    if (archiveMode)
    {
        file = extractToTemp(info.entries[it.entry].name);
        if (file.empty())
            return;
        title = fsu::joinPath(location(), it.name);
    }
    else
        title = file = fsu::joinPath(fsDir, it.name);
    std::string data;
    if (!fsu::readFile(file, data, kMaxViewSize))
    {
        showError("Не удалось прочитать файл\n" + title);
        return;
    }
    TProgram::application->insertWindow(
        new TViewerWindow(TProgram::deskTop->getExtent(), title, std::move(data)));
}

void TPanelWindow::extractFiles()
{
    if (!archiveMode)
    {
        // На диске извлекаются выбранные архивы целиком.
        std::vector<std::string> cabs;
        for (int i : targets())
            if (items[i].kind == ItemKind::File && isCab(items[i].name))
                cabs.push_back(fsu::joinPath(fsDir, items[i].name));
        if (cabs.empty())
        {
            showInfo("Для извлечения необходимо выбрать CAB-архив курсором или пометкой.");
            return;
        }
        ExtractOptions opt;
        opt.dest = cabs.size() == 1 ? fsu::joinPath(fsDir, fsu::stripExt(fsu::baseName(cabs[0])))
                                    : fsDir;
        if (!extractDialog(0, 0, opt))
            return;
        CabResult total;
        bool ok = runWithProgress("Извлечение", [&](CabProgress *p, std::string &err) {
            for (const std::string &cab : cabs)
            {
                std::string dest = cabs.size() == 1
                    ? opt.dest : fsu::joinPath(opt.dest, fsu::stripExt(fsu::baseName(cab)));
                p->onStage("Извлечение " + fsu::baseName(cab));
                CabResult r;
                if (!cabExtract(cab, dest, {}, opt.keepPaths, opt.overwrite, p, err, &r))
                    return false;
                total.files += r.files;
                total.skipped += r.skipped;
            }
            return true;
        });
        refresh();
        if (ok)
        {
            std::string msg = "Извлечено: " + pluralFiles(total.files);
            if (total.skipped)
                msg += "\nПропущено: " + pluralFiles(total.skipped);
            showInfo(msg + "\n\n" + opt.dest);
        }
        return;
    }

    // Без выбора (курсор на "..") извлекается содержимое текущей директории архива.
    std::vector<int> picked = targets();
    if (picked.empty())
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].kind == ItemKind::File || items[i].kind == ItemKind::Dir)
                picked.push_back((int) i);
    std::vector<std::string> names = entryNames(picked);
    ExtractOptions opt;
    opt.dest = !extractDir.empty() ? extractDir
        : fsu::joinPath(fsDir, fsu::stripExt(fsu::baseName(archiveDisplay)));
    opt.onlySelected = !names.empty();
    if (!extractDialog((int) names.size(), (int) info.entries.size(), opt))
        return;
    extractDir = opt.dest;

    std::vector<std::string> selected;
    std::string base;
    if (opt.onlySelected)
    {
        selected = names;
        base = inner;
    }
    std::string path = info.path;
    CabResult res;
    bool ok = runWithProgress("Извлечение", [&](CabProgress *p, std::string &err) {
        p->onStage("Извлечение в " + opt.dest);
        return cabExtract(path, opt.dest, selected, opt.keepPaths, opt.overwrite, p, err, &res, base);
    });
    if (ok)
    {
        std::string msg = "Извлечено: " + pluralFiles(res.files);
        if (res.skipped)
            msg += "\nПропущено: " + pluralFiles(res.skipped);
        showInfo(msg + "\n\n" + opt.dest);
    }
}

void TPanelWindow::addFiles()
{
    std::string archivePath;
    CabInfo existing;
    bool exists = false;
    AddOptions opt;
    opt.compression = cabApp().defaultCompression;
    std::vector<int> picked;

    if (archiveMode)
    {
        if (readOnly())
        {
            showError("Вложенный архив открыт только для чтения: изменения не попали бы "
                      "в родительский архив.");
            return;
        }
        archivePath = info.path;
        existing = info;
        exists = true;
        opt.prefix = inner;
    }
    else
    {
        // На диске выбранные файлы и директории помещаются в архив
        // (новый или существующий).
        picked = targets();
        std::string def;
        if (picked.size() == 1)
        {
            const PanelItem &it = items[picked[0]];
            def = it.kind == ItemKind::Dir ? it.name : fsu::stripExt(it.name);
        }
        else
        {
            def = fsu::isRootDir(fsDir) ? std::string("archive") : fsu::baseName(fsDir);
        }
        archivePath = fsu::joinPath(fsDir, def + ".cab");
        if (!askText("Добавить в архив", "~А~рхив:", archivePath))
            return;
        if (!fsu::isAbsolutePath(archivePath))
            archivePath = fsu::joinPath(fsDir, archivePath);
        archivePath = fsu::fullPath(archivePath);
        if (fsu::extension(archivePath).empty())
            archivePath += ".cab";
        if (fsu::dirExists(archivePath))
        {
            showError("Существует директория с таким именем\n" + archivePath);
            return;
        }
        exists = fsu::fileExists(archivePath);
        std::string err;
        if (exists && !cabRead(archivePath, existing, err))
        {
            showError("Файл существует и не является CAB-архивом\n" + archivePath + "\n\n" + err);
            return;
        }
        for (int i : picked)
            opt.items.push_back(fsu::joinPath(fsDir, items[i].name));
    }
    if (exists && (existing.hasPrev || existing.hasNext))
    {
        showError("Изменение многотомных архивов не поддерживается.");
        return;
    }
    if (exists && !existing.entries.empty())
        opt.compression = existing.compression;

    if (!addFilesDialog(exists ? "Добавить в архив" : "Новый архив", opt))
        return;
    std::vector<CabSource> sources;
    if (!collectSources(opt, sources))
        return;

    // Архив не добавляется сам в себя.
    std::string self = fsu::upper(archivePath);
    sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const CabSource &s) {
        return fsu::upper(s.diskPath) == self;
    }), sources.end());

    if (exists)
    {
        std::unordered_set<std::string> names;
        for (const CabEntry &e : existing.entries)
            names.insert(fsu::upper(e.name));
        auto clash = [&](const CabSource &s) { return names.count(fsu::upper(s.nameInCab)) != 0; };
        size_t dup = std::count_if(sources.begin(), sources.end(), clash);
        if (dup)
        {
            ushort r = confirm3("В архиве уже есть " + pluralFiles(dup) +
                                " с такими же именами.\nЗаменить их?");
            if (r == cmCancel)
                return;
            if (r == cmNo)
                sources.erase(std::remove_if(sources.begin(), sources.end(), clash), sources.end());
        }
    }
    if (sources.empty())
        return;

    CompressionSpec comp = opt.compression;
    bool ok = runWithProgress(exists ? "Добавление в архив" : "Создание архива",
        [&](CabProgress *p, std::string &err) {
            return exists ? cabUpdate(archivePath, {}, sources, comp, p, err)
                          : cabCreate(archivePath, sources, comp, p, err);
        });
    if (archiveMode)
        refresh();
    else if (loadDirectoryItems(fsDir, items))
        rebuild(ok ? fsu::baseName(archivePath) : std::string(), {});
}

void TPanelWindow::deleteFromDisk()
{
    std::vector<int> picked = targets();
    if (picked.empty())
        return;
    std::vector<std::string> paths;
    size_t files = 0, dirs = 0;
    for (int i : picked)
    {
        paths.push_back(fsu::joinPath(fsDir, items[i].name));
        ++(items[i].kind == ItemKind::Dir ? dirs : files);
    }
    // Винительный падеж: «1 директорию», «2 директории», «5 директорий».
    auto dirsAcc = [](size_t n) {
        return n % 10 == 1 && n % 100 != 11 ? formatNumber(n) + " директорию" : pluralDirs(n);
    };
    std::string what;
    if (picked.size() == 1)
        what = (dirs ? "директорию\n" : "файл\n") + items[picked[0]].name;
    else if (files && dirs)
        what = pluralFiles(files) + " и " + dirsAcc(dirs);
    else
        what = files ? pluralFiles(files) : dirsAcc(dirs);
    if (!confirm("Удалить в корзину " + what + "?"))
        return;

    // Курсор встаёт на первую оставшуюся строку после удаляемых.
    std::string next;
    std::unordered_set<int> removed(picked.begin(), picked.end());
    for (int i = focusedIndex(); i >= 0 && i < (int) items.size(); ++i)
        if (!removed.count(i))
        {
            next = items[i].name;
            break;
        }

    std::string err;
    if (!fsu::recycle(paths, err))
        showError(err);
    if (loadDirectoryItems(fsDir, items))
        rebuild(next, {});
    else
        openDirectory(fsu::existingDir(fsDir));
}

void TPanelWindow::deleteFiles()
{
    if (!archiveMode)
    {
        deleteFromDisk();
        return;
    }
    std::vector<int> picked = targets();
    std::vector<std::string> names = entryNames(picked);
    if (names.empty())
        return;
    if (readOnly())
    {
        showError("Вложенный архив открыт только для чтения: изменения не попали бы "
                  "в родительский архив.");
        return;
    }
    if (info.hasPrev || info.hasNext)
    {
        showError("Изменение многотомных архивов не поддерживается.");
        return;
    }
    std::string path = info.path;
    if (names.size() >= info.entries.size())
    {
        if (!confirm("В архиве не останется файлов.\nУдалить файл архива?\n\n" + path))
            return;
        if (!fsu::removeFile(path))
        {
            showError("Не удалось удалить файл\n" + path + "\n" + fsu::lastErrorText());
            return;
        }
        openDirectory(fsDir);
        return;
    }
    std::string what = pluralFiles(names.size());
    if (picked.size() == 1)
    {
        const PanelItem &it = items[picked[0]];
        what = it.kind == ItemKind::Dir
            ? "директорию\n" + it.name + "\n(" + pluralFiles(names.size()) + ")"
            : "файл\n" + it.name;
    }
    if (!confirm("Удалить из архива " + what + "?"))
        return;
    CompressionSpec comp = info.compression;
    runWithProgress("Удаление из архива", [&](CabProgress *p, std::string &err) {
        return cabUpdate(path, names, {}, comp, p, err);
    });
    refresh();
}

void TPanelWindow::testArchive()
{
    std::string path = archiveMode ? info.path : focusedCab();
    std::string display = archiveMode ? archiveDisplay : path;
    if (path.empty())
    {
        showInfo("Для проверки необходимо установить курсор на CAB-архив.");
        return;
    }
    CabResult res;
    bool ok = runWithProgress("Проверка архива", [&](CabProgress *p, std::string &err) {
        p->onStage("Проверка " + fsu::baseName(display));
        return cabTest(path, p, err, &res);
    });
    if (ok)
        showInfo("Ошибок не обнаружено.\nПроверено: " + pluralFiles(res.files) + ".");
}

void TPanelWindow::showProperties()
{
    if (archiveMode)
    {
        textDialog("Свойства архива", propertiesText(info, archiveDisplay));
        return;
    }
    std::string path = focusedCab();
    if (path.empty())
    {
        showInfo("Для просмотра свойств необходимо установить курсор на CAB-архив.");
        return;
    }
    CabInfo cab;
    std::string err;
    if (!cabRead(path, cab, err))
    {
        showError("Не удалось открыть архив\n" + path + "\n\n" + err);
        return;
    }
    textDialog("Свойства архива", propertiesText(cab, path));
}

bool collectSources(const AddOptions &opt, std::vector<CabSource> &sources)
{
    std::string prefix = opt.prefix;
    for (char &c : prefix)
        if (c == '/')
            c = '\\';
    while (!prefix.empty() && prefix.front() == '\\')
        prefix.erase(0, 1);
    while (!prefix.empty() && prefix.back() == '\\')
        prefix.pop_back();

    std::vector<fsu::FoundFile> found;
    for (const std::string &item : opt.items)
    {
        std::string err;
        if (!fsu::collectFiles(item, opt.recurse, opt.keepPaths, found, err))
        {
            showError(err);
            return false;
        }
    }
    if (found.empty())
    {
        showError("Не найдено ни одного файла для добавления.");
        return false;
    }
    for (const fsu::FoundFile &f : found)
        sources.push_back({f.path, fsu::joinPath(prefix, f.name)});
    return true;
}
