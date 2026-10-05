#define Uses_TKeys
#define Uses_TEvent
#define Uses_TEventQueue
#define Uses_TRect
#define Uses_TView
#define Uses_TGroup
#define Uses_TDialog
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TLabel
#define Uses_TInputLine
#define Uses_THistory
#define Uses_TCheckBoxes
#define Uses_TRadioButtons
#define Uses_TSItem
#define Uses_TScrollBar
#define Uses_TListViewer
#define Uses_TDrawBuffer
#define Uses_TPalette
#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_TScreen
#define Uses_MsgBox
#include <tvision/tv.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#include "commands.h"
#include "dialogs.h"
#include "fsutil.h"

namespace {

// Идентификатор списка истории директорий извлечения.
const ushort hlExtract = 12;

void setInputText(TInputLine *il, const std::string &s)
{
    std::vector<char> buf(il->dataSize(), 0);
    strnzcpy(buf.data(), s, buf.size());
    il->setData(buf.data());
}

ushort clusterValue(TView *v)
{
    ushort value = 0;
    v->getData(&value);
    return value;
}

void setClusterValue(TView *v, ushort value)
{
    v->setData(&value);
}

size_t utf8Len(char lead)
{
    unsigned char c = (unsigned char) lead;
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

struct TextRow
{
    std::string text;
    bool center;
};

// Разбивка текста на строки по ширине: переносы по пробелам, слишком длинные
// слова (пути) режутся по символам. Абзац, начинающийся с '\003', центрируется.
std::vector<TextRow> wrapText(const std::string &text, int width)
{
    std::vector<TextRow> rows;
    width = std::max(width, 1);
    size_t start = 0;
    while (true)
    {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        bool center = !para.empty() && para[0] == '\003';
        if (center)
            para.erase(0, 1);
        while (strwidth(para) > width)
        {
            size_t i = 0, lastSpace = std::string::npos;
            int w = 0;
            while (i < para.size())
            {
                size_t len = utf8Len(para[i]);
                int cw = strwidth(TStringView(para.data() + i, len));
                if (w + cw > width)
                    break;
                if (para[i] == ' ')
                    lastSpace = i;
                w += cw;
                i += len;
            }
            if (lastSpace != std::string::npos && lastSpace > 0)
            {
                rows.push_back({para.substr(0, lastSpace), center});
                para.erase(0, lastSpace + 1);
            }
            else
            {
                i = std::max(i, utf8Len(para[0]));
                rows.push_back({para.substr(0, i), center});
                para.erase(0, i);
            }
        }
        rows.push_back({para, center});
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return rows;
}

} // namespace

// Многострочный текст без ограничения длины (TStaticText обрезает
// текст до 255 байт, то есть примерно до 127 русских букв).
class TMultiText : public TView
{
public:
    TMultiText(const TRect &bounds, const std::string &text) :
        TView(bounds),
        text(text)
    {
    }

    static int rowsFor(const std::string &text, int width)
    {
        return (int) wrapText(text, width).size();
    }

    void draw() override
    {
        TColorAttr c = getColor(1);
        std::vector<TextRow> rows = wrapText(text, size.x);
        for (int y = 0; y < size.y; ++y)
        {
            TDrawBuffer b;
            b.moveChar(0, ' ', c, size.x);
            if (y < (int) rows.size())
            {
                const TextRow &r = rows[y];
                int x = r.center ? std::max(0, (size.x - strwidth(r.text)) / 2) : 0;
                b.moveStr(x, r.text, c, size.x - x);
            }
            writeLine(0, y, size.x, 1, b);
        }
    }

    TPalette &getPalette() const override
    {
        static TPalette palette("\x06", 1);
        return palette;
    }

private:
    std::string text;
};

namespace {

// Окно сообщения, размер которого рассчитан по тексту (длинные пути).
ushort messageBoxFit(const std::string &msg, ushort options)
{
    static const char *const titles[] = {"Предупреждение", "Ошибка", "Информация", "Подтверждение"};
    static const char *const names[] = {"~Д~а", "~Н~ет", "O~K~", "Отмена"};
    static const ushort commands[] = {cmYes, cmNo, cmOK, cmCancel};

    TRect screen = TProgram::deskTop->getExtent();
    int longest = 0;
    for (const TextRow &r : wrapText(msg, 1000))
        longest = std::max(longest, strwidth(r.text));
    int w = std::min(std::max(longest + 6, 40), (int) screen.b.x - 2);
    int rows = TMultiText::rowsFor(msg, w - 6);
    int h = std::min(rows + 6, (int) screen.b.y);

    TDialog *d = new TDialog(TRect(0, 0, w, h), titles[options & 3]);
    d->options |= ofCentered;
    d->insert(new TMultiText(TRect(3, 2, w - 3, h - 4), msg));
    std::vector<TButton *> buttons;
    for (int i = 0; i < 4; ++i)
        if (options & (0x0100 << i))
            buttons.push_back(new TButton(TRect(0, 0, 10, 2), names[i], commands[i],
                                          buttons.empty() ? bfDefault : bfNormal));
    int total = (int) buttons.size() * 12 - 2;
    int x = (w - total) / 2;
    for (TButton *b : buttons)
    {
        d->insert(b);
        b->moveTo(x, h - 3);
        x += 12;
    }
    d->selectNext(False);
    ushort r = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Виды для окна хода выполнения
// ---------------------------------------------------------------------------

class TTextLine : public TView
{
public:
    explicit TTextLine(const TRect &bounds) : TView(bounds) {}

    void setText(const std::string &s)
    {
        if (s != text)
        {
            text = s;
            drawView();
        }
    }

    void draw() override
    {
        TDrawBuffer b;
        TColorAttr c = getColor(1);
        b.moveChar(0, ' ', c, size.x);
        int w = strwidth(text);
        if (w <= size.x)
            b.moveStr(0, text, c);
        else
        {
            // Длинный путь показывается с конца.
            b.moveStr(0, "…", c);
            b.moveStr(1, text, c, size.x - 1, w - (size.x - 1));
        }
        writeLine(0, 0, size.x, 1, b);
    }

    TPalette &getPalette() const override
    {
        static TPalette palette("\x06", 1);
        return palette;
    }

private:
    std::string text;
};

class TProgressBar : public TView
{
public:
    explicit TProgressBar(const TRect &bounds) : TView(bounds) {}

    void setValue(double v)
    {
        int p = (int) (std::min(std::max(v, 0.0), 1.0) * 1000);
        if (p != permille)
        {
            permille = p;
            drawView();
        }
    }

    void draw() override
    {
        TDrawBuffer b;
        TColorAttr c = getColor(1);
        int barW = size.x - 6;
        int filled = barW * permille / 1000;
        b.moveChar(0, '\xDB', c, filled);
        b.moveChar(filled, '\xB0', c, barW - filled);
        char pct[16];
        snprintf(pct, sizeof(pct), " %3d%%", permille / 10);
        b.moveChar(barW, ' ', c, size.x - barW);
        b.moveStr(barW, pct, c);
        writeLine(0, 0, size.x, 1, b);
    }

    TPalette &getPalette() const override
    {
        static TPalette palette("\x06", 1);
        return palette;
    }

private:
    int permille = 0;
};

// ---------------------------------------------------------------------------
// Диалог обзора файлов и директорий
// ---------------------------------------------------------------------------
// Заменяет TFileDialog и TChDirDialog: те хранят пути в буферах фиксированной
// длины (68 и 260 байт) и копируют их через strcpy, что при длинных путях
// в UTF-8 (кириллица — 2 байта на букву) портит память.

enum class BrowseMode { Open, Save, Directory };

class TBrowseDialog;

class TBrowseList : public TListViewer
{
public:
    TBrowseList(const TRect &bounds, TScrollBar *vsb, TBrowseDialog *dlg) :
        TListViewer(bounds, 1, nullptr, vsb),
        dlg(dlg)
    {
    }

    void getText(char *dest, short item, short maxLen) override;
    void focusItem(short item) override;
    void selectItem(short item) override;

private:
    TBrowseDialog *dlg;
};

class TBrowseDialog : public TDialog
{
public:
    TBrowseDialog(const char *title, BrowseMode mode, const std::string &mask,
                  const std::string &start);

    Boolean valid(ushort command) override;

    std::string result;

private:
    friend class TBrowseList;

    enum Kind { Parent, Dir, File, Drive };
    struct Entry
    {
        std::string text;
        std::string path;
        Kind kind;
    };

    void load(const std::string &newDir, const std::string &focusName = std::string());
    void open(const Entry &e);
    void setInput(const std::string &s);

    BrowseMode mode;
    std::string mask;
    std::string dir;
    std::vector<Entry> entries;
    TInputLine *input;
    TTextLine *dirLine;
    TBrowseList *list;
};

void TBrowseList::getText(char *dest, short item, short maxLen)
{
    const std::string &s = dlg->entries[item].text;
    size_t n = std::min(s.size(), (size_t) maxLen);
    memcpy(dest, s.data(), n);
    dest[n] = 0;
}

void TBrowseList::focusItem(short item)
{
    TListViewer::focusItem(item);
    // Имя выбранного файла переносится в строку ввода, как в TFileDialog.
    if (item >= 0 && item < (short) dlg->entries.size() &&
        dlg->entries[item].kind == TBrowseDialog::File && (state & sfFocused))
        dlg->setInput(dlg->entries[item].text);
}

void TBrowseList::selectItem(short item)
{
    if (item < 0 || item >= (short) dlg->entries.size())
        return;
    const TBrowseDialog::Entry e = dlg->entries[item];
    if (e.kind == TBrowseDialog::File)
    {
        dlg->setInput(e.text);
        TEvent ev;
        ev.what = evCommand;
        ev.message.command = cmOK;
        ev.message.infoPtr = nullptr;
        putEvent(ev);
    }
    else
        dlg->open(e);
}

TBrowseDialog::TBrowseDialog(const char *title, BrowseMode mode, const std::string &mask,
                             const std::string &start) :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 70, 21), title),
    mode(mode),
    mask(mask)
{
    options |= ofCentered;

    input = new TInputLine(TRect(3, 3, 54, 4), 1024);
    insert(input);
    insert(new TLabel(TRect(2, 2, 30, 3),
                      mode == BrowseMode::Directory ? "~И~мя директории:" : "~И~мя файла:", input));

    dirLine = new TTextLine(TRect(3, 5, 54, 6));
    insert(dirLine);

    TScrollBar *sb = new TScrollBar(TRect(53, 8, 54, 19));
    insert(sb);
    list = new TBrowseList(TRect(3, 8, 53, 19), sb, this);
    insert(list);
    insert(new TLabel(TRect(2, 7, 30, 8),
                      mode == BrowseMode::Directory ? "~С~одержимое:" : "~Ф~айлы:", list));

    const char *okText = mode == BrowseMode::Open ? "~О~ткрыть"
                       : mode == BrowseMode::Save ? "~С~охранить" : "~В~ыбрать";
    insert(new TButton(TRect(56, 3, 68, 5), okText, cmOK, bfDefault));
    insert(new TButton(TRect(56, 6, 68, 8), "Отмена", cmCancel, bfNormal));

    // Начальная директория — ближайшая существующая из указанной.
    std::string d = start.empty() ? fsu::currentDir() : fsu::fullPath(start);
    while (!fsu::dirExists(d) && !fsu::isRootDir(d))
        d = fsu::dirName(d);
    if (!fsu::dirExists(d))
        d = fsu::currentDir();
    load(d);
    if (mode != BrowseMode::Directory)
        setInput(mask);
    selectNext(False);
}

void TBrowseDialog::setInput(const std::string &s)
{
    setInputText(input, s);
    input->selectAll(True);
    input->drawView();
}

void TBrowseDialog::load(const std::string &newDir, const std::string &focusName)
{
    std::vector<fsu::DirItem> items;
    if (!fsu::listDirectory(newDir, items))
    {
        showError("Не удалось прочитать директорию\n" + newDir + "\n" + fsu::lastErrorText());
        if (!entries.empty())
            return;
    }
    dir = newDir;
    entries.clear();
    if (!fsu::isRootDir(dir))
        entries.push_back({"..\\", fsu::dirName(dir), Parent});

    std::vector<fsu::DirItem> dirs, files;
    for (fsu::DirItem &i : items)
        if (i.isDir)
            dirs.push_back(i);
        else if (mode != BrowseMode::Directory && fsu::wildMatch(mask, i.name))
            files.push_back(i);
    auto byName = [](const fsu::DirItem &a, const fsu::DirItem &b) {
        return fsu::compareNatural(fsu::widen(a.name), fsu::widen(b.name)) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (const fsu::DirItem &i : dirs)
        entries.push_back({i.name + "\\", fsu::joinPath(dir, i.name), Dir});
    for (const fsu::DirItem &i : files)
        entries.push_back({i.name, fsu::joinPath(dir, i.name), File});
    for (const std::string &drive : fsu::logicalDrives())
        entries.push_back({"[" + drive.substr(0, 2) + "]", drive, Drive});

    list->setRange((short) std::min<size_t>(entries.size(), 32767));
    short focus = 0;
    for (size_t i = 0; i < entries.size() && i < 32767; ++i)
        if (!focusName.empty() && entries[i].text == focusName + "\\")
            focus = (short) i;
    list->focusItemNum(focus);
    list->drawView();
    dirLine->setText(dir);
    if (mode == BrowseMode::Directory)
        setInput(dir);
}

void TBrowseDialog::open(const Entry &e)
{
    // При переходе вверх курсор встаёт на директорию, из которой вышли.
    std::string from = e.kind == Parent ? fsu::baseName(dir) : std::string();
    load(e.path, from);
}

Boolean TBrowseDialog::valid(ushort command)
{
    if (command != cmOK)
        return TDialog::valid(command);

    // Enter в списке на директории или диске выполняет переход.
    if (current == list && list->focused >= 0 && list->focused < (short) entries.size() &&
        entries[list->focused].kind != File)
    {
        open(entries[list->focused]);
        return False;
    }

    std::string text = input->data;
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    while (!text.empty() && text.front() == ' ')
        text.erase(0, 1);
    std::string path = text.empty() ? dir
        : fsu::fullPath(fsu::isAbsolutePath(text) ? text : fsu::joinPath(dir, text));

    if (mode == BrowseMode::Directory)
    {
        if (!fsu::dirExists(path))
        {
            showError("Директория не найдена\n" + path);
            input->focus();
            setInput(text);
            return False;
        }
        result = path;
        return True;
    }
    if (text.empty())
        return False;
    // Маска меняет фильтр, путь к директории — текущую директорию.
    if (fsu::baseName(text).find_first_of("*?") != std::string::npos)
    {
        mask = fsu::baseName(text);
        std::string d = fsu::dirName(path);
        load(fsu::dirExists(d) ? d : dir);
        setInput(mask);
        return False;
    }
    if (fsu::dirExists(path))
    {
        load(path);
        setInput(mask);
        return False;
    }
    // После ошибки текст выделяется, чтобы новый ввод его заменил.
    if (mode == BrowseMode::Open && !fsu::fileExists(path))
    {
        showError("Файл не найден\n" + path);
        input->focus();
        setInput(text);
        return False;
    }
    if (mode == BrowseMode::Save && !fsu::dirExists(fsu::dirName(path)))
    {
        showError("Директория не найдена\n" + fsu::dirName(path));
        input->focus();
        setInput(text);
        return False;
    }
    result = path;
    return True;
}

// Диалог, который завершается и по командам «для всех».
class TChoiceDialog : public TDialog
{
public:
    TChoiceDialog(const TRect &bounds, const char *title) :
        TWindowInit(&TDialog::initFrame),
        TDialog(bounds, title)
    {
    }

    void handleEvent(TEvent &ev) override
    {
        TDialog::handleEvent(ev);
        if (ev.what == evCommand &&
            (ev.message.command == cmYesAll || ev.message.command == cmNoAll))
        {
            endModal(ev.message.command);
            clearEvent(ev);
        }
    }
};

TProgressDialog::TProgressDialog(const char *title) :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 64, 10), title)
{
    options |= ofCentered;
    flags &= ~wfClose;
    stage = new TTextLine(TRect(2, 2, 62, 3));
    file = new TTextLine(TRect(2, 3, 62, 4));
    bar = new TProgressBar(TRect(2, 5, 62, 6));
    insert(stage);
    insert(file);
    insert(bar);
    cancelButton = new TButton(TRect(26, 7, 38, 9), "Отмена", cmCancel, bfDefault);
    insert(cancelButton);
}

bool TProgressDialog::confirmCancel()
{
    if (messageBox("Прервать операцию?", mfConfirmation | mfYesButton | mfNoButton) == cmYes)
        cancelled_ = true;
    TScreen::flushScreen();
    lastRefresh = std::chrono::steady_clock::now();
    return cancelled_;
}

void TProgressDialog::onStage(const std::string &text)
{
    stage->setText(text);
    bar->setValue(0);
    refresh(true);
}

bool TProgressDialog::onFile(const std::string &name)
{
    file->setText(name);
    return refresh(false);
}

bool TProgressDialog::onBytes(uint64_t done, uint64_t total)
{
    bar->setValue(total ? double(done) / double(total) : 0.0);
    return refresh(false);
}

AskResult TProgressDialog::askOverwrite(const std::string &path)
{
    TChoiceDialog *d = new TChoiceDialog(TRect(0, 0, 68, 10), "Файл существует");
    d->options |= ofCentered;
    d->insert(new TStaticText(TRect(2, 2, 66, 3), "Файл уже существует. Заменить?"));
    d->insert(new TMultiText(TRect(2, 3, 66, 6), path));
    d->insert(new TButton(TRect(2, 7, 11, 9), "~Д~а", cmYes, bfDefault));
    d->insert(new TButton(TRect(12, 7, 27, 9), "Да для ~в~сех", cmYesAll, bfNormal));
    d->insert(new TButton(TRect(28, 7, 37, 9), "~Н~ет", cmNo, bfNormal));
    d->insert(new TButton(TRect(38, 7, 54, 9), "Нет для в~с~ех", cmNoAll, bfNormal));
    d->insert(new TButton(TRect(55, 7, 66, 9), "Отмена", cmCancel, bfNormal));
    d->selectNext(False);
    ushort r = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    refresh(true);
    switch (r)
    {
        case cmYes:     return AskResult::Yes;
        case cmYesAll:  return AskResult::YesAll;
        case cmNo:      return AskResult::No;
        case cmNoAll:   return AskResult::NoAll;
        default:        return AskResult::Cancel;
    }
}

bool TProgressDialog::refresh(bool force)
{
    auto now = std::chrono::steady_clock::now();
    if (!force && now - lastRefresh < std::chrono::milliseconds(50))
        return !cancelled_;
    lastRefresh = now;
    TScreen::flushScreen();

    // Цикл событий во время операции не работает: мышь и клавиатура
    // опрашиваются здесь. Отмена — щелчок по кнопке, Esc, Enter или пробел.
    for (int i = 0; i < 32 && !cancelled_; ++i)
    {
        TEvent ev;
        ev.what = evNothing;
        ev.getMouseEvent();
        if (ev.what == evMouseDown && cancelButton->mouseInView(ev.mouse.where))
        {
            confirmCancel();
            break;
        }
        if (ev.what != evNothing)
            continue;
        ev.getKeyEvent(False);
        if (ev.what == evNothing)
            break;
        if (ev.what != evKeyDown)
        {
            // Например, смена размера экрана: обрабатывается после операции.
            TProgram::application->putEvent(ev);
            break;
        }
        ushort key = ev.keyDown.keyCode;
        if (key == kbEsc || key == kbEnter || ev.keyDown.charScan.charCode == ' ')
        {
            confirmCancel();
            break;
        }
    }
    return !cancelled_;
}

void TProgressDialog::showDone(unsigned holdMs)
{
    stage->setText("Готово");
    file->setText("");
    bar->setValue(1.0);
    // Turbo Vision ограничивает частоту вывода и пропускает слишком частые
    // flushScreen, поэтому экран сбрасывается повторно в течение паузы.
    for (unsigned t = 0; t < holdMs; t += 40)
    {
        TScreen::flushScreen();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

bool runWithProgress(const char *title,
                     const std::function<bool(CabProgress *, std::string &)> &op,
                     unsigned holdMs)
{
    TProgressDialog *pd = new TProgressDialog(title);
    TProgram::deskTop->insert(pd);
    TScreen::flushScreen();
    std::string err;
    bool ok = op(pd, err);
    bool cancelled = pd->cancelled() || err == kCabCancelled;
    if (ok && holdMs)
        pd->showDone(holdMs);
    TObject::destroy(pd);
    if (!ok && !cancelled)
        showError(err);
    return ok;
}

// ---------------------------------------------------------------------------
// Диалог добавления файлов
// ---------------------------------------------------------------------------

namespace {

class TStringListView : public TListViewer
{
public:
    TStringListView(const TRect &bounds, TScrollBar *vsb, std::vector<std::string> &items) :
        TListViewer(bounds, 1, nullptr, vsb),
        items(items)
    {
        update();
    }

    void getText(char *dest, short item, short maxLen) override
    {
        const std::string &s = items[item];
        size_t n = std::min(s.size(), (size_t) maxLen);
        memcpy(dest, s.data(), n);
        dest[n] = 0;
    }

    void update()
    {
        setRange((short) items.size());
        drawView();
    }

private:
    std::vector<std::string> &items;
};

class TAddDialog : public TDialog
{
public:
    TAddDialog(const char *title, AddOptions &opt);

    void handleEvent(TEvent &ev) override;
    Boolean valid(ushort command) override;

private:
    void addItem(const std::string &item);

    AddOptions &opt;
    std::vector<std::string> items;
    TStringListView *list;
    TCheckBoxes *flagsBox;
    TRadioButtons *method;
    TInputLine *prefix;
    TCheckBoxes *sfx = nullptr;
};

TAddDialog::TAddDialog(const char *title, AddOptions &opt) :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 70, opt.allowSfx ? 23 : 21), title),
    opt(opt),
    items(opt.items)
{
    options |= ofCentered;

    TScrollBar *sb = new TScrollBar(TRect(51, 3, 52, 10));
    insert(sb);
    list = new TStringListView(TRect(3, 3, 51, 10), sb, items);
    insert(list);
    insert(new TLabel(TRect(2, 2, 40, 3), "~С~писок файлов и директорий:", list));

    insert(new TButton(TRect(54, 3, 68, 5), "~Ф~айл...", cmAddItemFile, bfNormal));
    insert(new TButton(TRect(54, 5, 68, 7), "~Д~иректория...", cmAddItemDir, bfNormal));
    insert(new TButton(TRect(54, 7, 68, 9), "~М~аска...", cmAddItemMask, bfNormal));
    insert(new TButton(TRect(54, 9, 68, 11), "~У~брать", cmRemoveItem, bfNormal));

    flagsBox = new TCheckBoxes(TRect(3, 12, 38, 14),
        new TSItem("Включать поддиректории",
        new TSItem("Сохранять относительные пути", nullptr)));
    insert(flagsBox);
    insert(new TLabel(TRect(2, 11, 20, 12), "~П~араметры:", flagsBox));
    setClusterValue(flagsBox, (opt.recurse ? 1 : 0) | (opt.keepPaths ? 2 : 0));

    method = new TRadioButtons(TRect(40, 12, 67, 15),
        new TSItem("Без сжатия",
        new TSItem("MSZIP",
        new TSItem("LZX (максимальное)", nullptr))));
    insert(method);
    insert(new TLabel(TRect(39, 11, 50, 12), "Сжати~е~:", method));
    setClusterValue(method, (ushort) opt.compression.type);

    prefix = new TInputLine(TRect(3, 16, 67, 17), 200);
    insert(prefix);
    insert(new TLabel(TRect(2, 15, 30, 16), "Путь внутри ~а~рхива:", prefix));
    setInputText(prefix, opt.prefix);

    // Выбор доступен только при создании нового архива.
    int y = 18;
    if (opt.allowSfx)
    {
        sfx = new TCheckBoxes(TRect(3, 18, 67, 19),
            new TSItem("Создать самораспаковывающийся архив (EXE)", nullptr));
        insert(sfx);
        setClusterValue(sfx, opt.sfx ? 1 : 0);
        y = 20;
    }

    insert(new TButton(TRect(22, y, 34, y + 2), "O~K~", cmOK, bfDefault));
    insert(new TButton(TRect(36, y, 48, y + 2), "Отмена", cmCancel, bfNormal));

    selectNext(False);
}

void TAddDialog::addItem(const std::string &item)
{
    if (item.empty())
        return;
    if (std::find(items.begin(), items.end(), item) == items.end())
        items.push_back(item);
    list->update();
    list->focusItemNum((short) items.size() - 1);
    list->drawView();
}

void TAddDialog::handleEvent(TEvent &ev)
{
    TDialog::handleEvent(ev);
    if (ev.what != evCommand)
        return;
    switch (ev.message.command)
    {
        case cmAddItemFile:
        {
            std::string path;
            if (chooseFile("Добавить файл", "*.*", false, path))
                addItem(path);
            break;
        }
        case cmAddItemDir:
        {
            std::string dir;
            if (chooseDirectory(lastDirectory(), dir))
            {
                lastDirectory() = dir;
                addItem(dir);
            }
            break;
        }
        case cmAddItemMask:
        {
            std::string mask = fsu::joinPath(lastDirectory(), "*.*");
            if (askMask("Добавить по маске", mask))
                addItem(fsu::fullPath(mask));
            break;
        }
        case cmRemoveItem:
            if (list->focused >= 0 && list->focused < (short) items.size())
            {
                items.erase(items.begin() + list->focused);
                list->update();
            }
            break;
        default:
            return;
    }
    clearEvent(ev);
}

Boolean TAddDialog::valid(ushort command)
{
    if (command == cmOK)
    {
        if (items.empty())
        {
            showError("Необходимо выбрать хотя бы один файл или директорию.");
            return False;
        }
        ushort f = clusterValue(flagsBox);
        opt.items = items;
        opt.recurse = (f & 1) != 0;
        opt.keepPaths = (f & 2) != 0;
        opt.prefix = prefix->data;
        opt.compression.type = (CabCompression) clusterValue(method);
        opt.sfx = sfx && (clusterValue(sfx) & 1) != 0;
    }
    return TDialog::valid(command);
}

// ---------------------------------------------------------------------------
// Диалог извлечения
// ---------------------------------------------------------------------------

class TExtractDialog : public TDialog
{
public:
    // about — сведения об архиве над полем директории (окно распаковщика).
    TExtractDialog(int selectedCount, int totalCount, ExtractOptions &opt,
                   const std::string *about = nullptr);

    void handleEvent(TEvent &ev) override;
    Boolean valid(ushort command) override;

private:
    ExtractOptions &opt;
    TInputLine *dest;
    TRadioButtons *scope = nullptr;
    TRadioButtons *overwrite;
    TCheckBoxes *paths;
};

TExtractDialog::TExtractDialog(int selectedCount, int totalCount, ExtractOptions &opt,
                               const std::string *about) :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 66, about ? 16 + TMultiText::rowsFor(*about, 60) + 1 : 16),
            about ? "Самораспаковывающийся архив" : "Извлечь файлы"),
    opt(opt)
{
    options |= ofCentered;

    int dy = 0;
    if (about)
    {
        dy = TMultiText::rowsFor(*about, 60) + 1;
        insert(new TMultiText(TRect(3, 2, 63, 2 + dy - 1), *about));
    }

    dest = new TInputLine(TRect(3, 3 + dy, 50, 4 + dy), 250);
    insert(dest);
    insert(new THistory(TRect(50, 3 + dy, 53, 4 + dy), dest, hlExtract));
    insert(new TLabel(TRect(2, 2 + dy, 30, 3 + dy), about ? "~Р~аспаковать в:" : "Извлечь ~в~:", dest));
    insert(new TButton(TRect(54, 2 + dy, 65, 4 + dy), "~О~бзор...", cmBrowseDir, bfNormal));
    setInputText(dest, opt.dest);

    // Выбор «выбранные / все» нужен только при извлечении из открытого архива.
    if (totalCount > 0)
    {
        std::string selText = "Выбранные (" + std::to_string(selectedCount) + ")";
        std::string allText = "Весь архив (" + std::to_string(totalCount) + ")";
        scope = new TRadioButtons(TRect(3, 6, 32, 8),
            new TSItem(selText, new TSItem(allText, nullptr)));
        insert(scope);
        insert(new TLabel(TRect(2, 5, 12, 6), "~Ф~айлы:", scope));
        setClusterValue(scope, opt.onlySelected && selectedCount > 0 ? 0 : 1);
        if (selectedCount == 0)
            scope->setButtonState(1, False);
    }

    overwrite = new TRadioButtons(TRect(34, 6 + dy, 63, 9 + dy),
        new TSItem("Спрашивать",
        new TSItem("Заменять",
        new TSItem("Пропускать", nullptr))));
    insert(overwrite);
    insert(new TLabel(TRect(33, 5 + dy, 58, 6 + dy), "~С~уществующие файлы:", overwrite));
    setClusterValue(overwrite, (ushort) opt.overwrite);

    paths = new TCheckBoxes(TRect(3, 10 + dy, 32, 11 + dy),
        new TSItem("Сохранять пути", nullptr));
    insert(paths);
    insert(new TLabel(TRect(2, 9 + dy, 20, 10 + dy), "~П~араметры:", paths));
    setClusterValue(paths, opt.keepPaths ? 1 : 0);

    if (about)
    {
        insert(new TButton(TRect(17, 13 + dy, 33, 15 + dy), "Р~а~спаковать", cmOK, bfDefault));
        insert(new TButton(TRect(35, 13 + dy, 47, 15 + dy), "Отмена", cmCancel, bfNormal));
    }
    else
    {
        insert(new TButton(TRect(20, 13, 32, 15), "O~K~", cmOK, bfDefault));
        insert(new TButton(TRect(34, 13, 46, 15), "Отмена", cmCancel, bfNormal));
    }

    selectNext(False);
}

void TExtractDialog::handleEvent(TEvent &ev)
{
    TDialog::handleEvent(ev);
    if (ev.what == evCommand && ev.message.command == cmBrowseDir)
    {
        std::string start = dest->data, dir;
        while (!start.empty() && !fsu::dirExists(start) && fsu::dirName(start) != start)
            start = fsu::dirName(start);
        if (chooseDirectory(start, dir))
        {
            setInputText(dest, dir);
            dest->drawView();
        }
        clearEvent(ev);
    }
}

Boolean TExtractDialog::valid(ushort command)
{
    if (command == cmOK)
    {
        std::string d = dest->data;
        if (d.empty())
        {
            showError("Необходимо указать директорию для извлечения.");
            return False;
        }
        opt.dest = fsu::fullPath(d);
        opt.onlySelected = scope && clusterValue(scope) == 0;
        opt.overwrite = (Overwrite) clusterValue(overwrite);
        opt.keepPaths = (clusterValue(paths) & 1) != 0;
    }
    return TDialog::valid(command);
}

} // namespace

bool addFilesDialog(const char *title, AddOptions &opt)
{
    TAddDialog *d = new TAddDialog(title, opt);
    ushort r = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    return r == cmOK;
}

bool extractDialog(int selectedCount, int totalCount, ExtractOptions &opt)
{
    TExtractDialog *d = new TExtractDialog(selectedCount, totalCount, opt);
    ushort r = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    return r == cmOK;
}

bool sfxDialog(const std::string &about, ExtractOptions &opt)
{
    TExtractDialog *d = new TExtractDialog(0, 0, opt, &about);
    ushort r = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    return r == cmOK;
}

bool settingsDialog(Settings &s)
{
    TDialog *d = new TDialog(TRect(0, 0, 52, 15), "Параметры");
    d->options |= ofCentered;
    TCheckBoxes *general = new TCheckBoxes(TRect(3, 3, 49, 5),
        new TSItem("Ассоциировать файлы CAB с Cabine",
        new TSItem("Показывать архивы первыми", nullptr)));
    d->insert(general);
    d->insert(new TLabel(TRect(2, 2, 30, 3), "~О~бщие:", general));
    setClusterValue(general, (s.associate ? 1 : 0) | (s.archivesFirst ? 2 : 0));

    TRadioButtons *method = new TRadioButtons(TRect(3, 7, 49, 10),
        new TSItem("Без сжатия",
        new TSItem("MSZIP (быстрое)",
        new TSItem("LZX (максимальное)", nullptr))));
    d->insert(method);
    d->insert(new TLabel(TRect(2, 6, 40, 7), "~С~жатие новых архивов:", method));
    setClusterValue(method, (ushort) s.compression.type);

    d->insert(new TButton(TRect(13, 12, 25, 14), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(27, 12, 39, 14), "Отмена", cmCancel, bfNormal));
    d->selectNext(False);
    ushort r = TProgram::deskTop->execView(d);
    if (r == cmOK)
    {
        ushort g = clusterValue(general);
        s.associate = (g & 1) != 0;
        s.archivesFirst = (g & 2) != 0;
        s.compression.type = (CabCompression) clusterValue(method);
    }
    TObject::destroy(d);
    return r == cmOK;
}

void textDialog(const char *title, const std::string &text)
{
    int longest = 0;
    for (const TextRow &r : wrapText(text, 1000))
        longest = std::max(longest, strwidth(r.text));
    TRect screen = TProgram::deskTop->getExtent();
    int w = std::min(std::max(longest + 6, 36), (int) screen.b.x - 2);
    int h = std::min(TMultiText::rowsFor(text, w - 6) + 6, (int) screen.b.y);
    TDialog *d = new TDialog(TRect(0, 0, w, h), title);
    d->options |= ofCentered;
    d->insert(new TMultiText(TRect(3, 2, w - 3, h - 4), text));
    d->insert(new TButton(TRect(w / 2 - 6, h - 3, w / 2 + 6, h - 1), "O~K~", cmOK, bfDefault));
    d->selectNext(False);
    TProgram::deskTop->execView(d);
    TObject::destroy(d);
}

bool chooseFile(const char *title, const char *wildcard, bool forSave, std::string &path)
{
    TBrowseDialog *d = new TBrowseDialog(title, forSave ? BrowseMode::Save : BrowseMode::Open,
                                         wildcard, lastDirectory());
    bool ok = TProgram::deskTop->execView(d) == cmOK;
    if (ok)
    {
        path = d->result;
        lastDirectory() = fsu::dirName(path);
    }
    TObject::destroy(d);
    return ok;
}

bool chooseDirectory(const std::string &start, std::string &dir)
{
    TBrowseDialog *d = new TBrowseDialog("Выбор директории", BrowseMode::Directory, "*", start);
    bool ok = TProgram::deskTop->execView(d) == cmOK;
    if (ok)
        dir = d->result;
    TObject::destroy(d);
    return ok;
}

bool askText(const char *title, const char *label, std::string &value)
{
    // Своя строка ввода: inputBox ограничен 255 байтами.
    TDialog *d = new TDialog(TRect(0, 0, 70, 9), title);
    d->options |= ofCentered;
    TInputLine *input = new TInputLine(TRect(3, 3, 67, 4), 1024);
    d->insert(input);
    d->insert(new TLabel(TRect(2, 2, 40, 3), label, input));
    d->insert(new TButton(TRect(22, 6, 34, 8), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(36, 6, 48, 8), "Отмена", cmCancel, bfNormal));
    setInputText(input, value);
    d->selectNext(False);
    bool ok = TProgram::deskTop->execView(d) == cmOK;
    if (ok)
        value = input->data;
    TObject::destroy(d);
    return ok && !value.empty();
}

bool askMask(const char *title, std::string &mask)
{
    return askText(title, "~М~аска:", mask);
}

void showError(const std::string &msg)
{
    messageBoxFit(msg, mfError | mfOKButton);
}

void showInfo(const std::string &msg)
{
    messageBoxFit(msg, mfInformation | mfOKButton);
}

bool confirm(const std::string &msg)
{
    return messageBoxFit(msg, mfConfirmation | mfYesButton | mfNoButton) == cmYes;
}

ushort confirm3(const std::string &msg)
{
    return messageBoxFit(msg, mfConfirmation | mfYesNoCancel);
}

std::string &lastDirectory()
{
    // Обновляется панелью при переходе по директориям.
    static std::string dir = fsu::currentDir();
    return dir;
}

std::string compressionName(const CompressionSpec &spec)
{
    switch (spec.type)
    {
        case CabCompression::None:  return "none";
        case CabCompression::MSZIP: return "mszip";
        default:                    return "lzx";
    }
}

CompressionSpec compressionFromName(const std::string &name)
{
    CompressionSpec spec;
    if (name == "none")
        spec.type = CabCompression::None;
    else if (name == "lzx")
        spec.type = CabCompression::LZX;
    return spec;
}
