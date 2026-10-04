#define Uses_TKeys
#define Uses_TEvent
#define Uses_TView
#define Uses_TGroup
#define Uses_TProgram
#define Uses_TMenuView
#define Uses_TMenuBar
#define Uses_TMenu
#define Uses_TMenuItem
#define Uses_TSubMenu
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TLabel
#define Uses_TInputLine
#include <tvision/tv.h>

#include <cstdint>
#include <cstring>

#include "hotkeys.h"

namespace {

// Буквы раскладки ЙЦУКЕН на клавишах A..Z.
const char *const kCyrByLatin[26] = {
    "Ф", "И", "С", "В", "У", "А", "П", "Р", "Ш", "О", "Л", "Д", "Ь",
    "Т", "Щ", "З", "Й", "К", "Ы", "Е", "Г", "М", "Ц", "Ч", "Н", "Я",
};

uint32_t decodeUtf8(const char *s, size_t len)
{
    if (len == 0)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80)
        return c;
    if ((c & 0xE0) == 0xC0 && len >= 2)
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && len >= 3)
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return 0;
}

uint32_t toUpper(uint32_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 0x20;
    if (c >= 0x430 && c <= 0x44F)
        return c - 0x20;
    if (c == 0x451)     // ё
        return 0x401;
    return c;
}

bool isCyrillic(uint32_t c)
{
    return c >= 0x400 && c <= 0x4FF;
}

// Выделенная тильдой буква в верхнем регистре.
uint32_t hotLetter(const char *text)
{
    if (!text)
        return 0;
    const char *p = strchr(text, '~');
    if (!p || !p[1])
        return 0;
    return toUpper(decodeUtf8(p + 1, strlen(p + 1)));
}

// Кириллическая буква, соответствующая нажатию; 0, если её нет.
uint32_t keyLetter(const TEvent &ev, bool &withAlt)
{
    withAlt = false;
    char latin = getAltChar(ev.keyDown.keyCode);
    if (latin >= 'A' && latin <= 'Z')
    {
        withAlt = true;
        const char *s = kCyrByLatin[latin - 'A'];
        return decodeUtf8(s, strlen(s));
    }
    if (ev.keyDown.textLength > 0)
    {
        uint32_t c = toUpper(decodeUtf8(ev.keyDown.text, ev.keyDown.textLength));
        if (isCyrillic(c))
            return c;
    }
    return 0;
}

// Доступ к защищённым полям через указатель на член производного класса.
struct MenuAccess : TMenuView
{
    static TMenu *menuOf(TMenuView *v) { return v->*(&MenuAccess::menu); }
    static TMenuItem *&currentOf(TMenuView *v) { return v->*(&MenuAccess::current); }
    static TMenuView *parentOf(TMenuView *v) { return v->*(&MenuAccess::parentMenu); }
};

struct LabelAccess : TLabel
{
    static TView *linkOf(TLabel *l) { return l->*(&LabelAccess::link); }
    static const char *textOf(TLabel *l) { return l->*(&LabelAccess::text); }
};

TMenuItem *findByLetter(TMenu *menu, uint32_t letter)
{
    if (!menu || !letter)
        return nullptr;
    for (TMenuItem *p = menu->items; p; p = p->next)
        if (p->name && !p->disabled && hotLetter(p->name) == letter)
            return p;
    return nullptr;
}

void setKey(TEvent &ev, ushort keyCode)
{
    ev.keyDown.keyCode = keyCode;
    ev.keyDown.controlKeyState = 0;
    ev.keyDown.textLength = 0;
}

void translateMenu(TEvent &ev, TMenuView *mv, uint32_t letter, bool withAlt)
{
    bool isBar = dynamic_cast<TMenuBar *>(mv) != nullptr;
    if (isBar || !withAlt)
    {
        // Выделение пункта и его выбор средствами самого меню:
        // в строке меню kbDown раскрывает подменю, в подменю kbEnter выбирает пункт.
        TMenuItem *p = findByLetter(MenuAccess::menuOf(mv), letter);
        if (p)
        {
            MenuAccess::currentOf(mv) = p;
            setKey(ev, isBar ? kbDown : kbEnter);
        }
        return;
    }

    // Alt+буква в раскрытом подменю переключает на другой пункт строки меню:
    // строке выделяется предшествующий пункт, а kbRight закрывает подменю
    // и переводит выделение на найденный.
    TMenuView *bar = mv;
    while (MenuAccess::parentOf(bar))
        bar = MenuAccess::parentOf(bar);
    TMenu *top = MenuAccess::menuOf(bar);
    TMenuItem *p = findByLetter(top, letter);
    if (!p)
        return;
    TMenuItem *prev = nullptr, *last = nullptr;
    for (TMenuItem *q = top->items; q; q = q->next)
    {
        if (q == p)
            prev = last;
        if (q->name)
            last = q;
    }
    MenuAccess::currentOf(bar) = prev ? prev : last;
    setKey(ev, kbRight);
}

void translateDialog(TEvent &ev, TGroup *g, uint32_t letter, bool withAlt)
{
    // Обычные буквы в строке ввода — это текст.
    if (!withAlt && dynamic_cast<TInputLine *>(g->current))
        return;
    TView *first = g->first();
    if (!first)
        return;
    TView *v = first;
    do
    {
        if (auto *b = dynamic_cast<TButton *>(v))
        {
            if (!(b->state & sfDisabled) && hotLetter(b->title) == letter)
            {
                b->press();
                ev.what = evNothing;
                return;
            }
        }
        else if (auto *l = dynamic_cast<TLabel *>(v))
        {
            TView *link = LabelAccess::linkOf(l);
            if (link && (link->options & ofSelectable) && !(link->state & sfDisabled) &&
                hotLetter(LabelAccess::textOf(l)) == letter)
            {
                link->focus();
                ev.what = evNothing;
                return;
            }
        }
        v = v->next;
    } while (v != first);
}

} // namespace

TCabMenuBar::TCabMenuBar(const TRect &bounds, TSubMenu &aMenu) :
    TMenuBar(bounds, aMenu)
{
}

void TCabMenuBar::handleEvent(TEvent &event)
{
    if (event.what == evKeyDown && menu)
    {
        bool withAlt;
        uint32_t letter = keyLetter(event, withAlt);
        TMenuItem *p = withAlt ? findByLetter(menu, letter) : nullptr;
        if (p)
        {
            // То же, что делает TMenuView при Alt+латинская буква:
            // меню запускается с выделенным пунктом, kbDown раскрывает его.
            menu->deflt = p;
            TEvent open = event;
            setKey(open, kbDown);
            putEvent(open);
            ushort command = owner->execView(this);
            if (command && commandEnabled(command))
            {
                event.what = evCommand;
                event.message.command = command;
                event.message.infoPtr = nullptr;
                putEvent(event);
            }
            clearEvent(event);
            return;
        }
    }
    TMenuBar::handleEvent(event);
}

// Код клавиши для текстового ввода: младший байт (символ) нулевой,
// старший не совпадает ни с одним скан-кодом Alt-комбинаций.
const ushort kbTextInput = 0xFE00;

void translateCyrillicHotkey(TEvent &event, TView *topView)
{
    if (event.what != evKeyDown)
        return;
    if (event.keyDown.keyCode == kbNoKey && event.keyDown.textLength > 0)
        event.keyDown.keyCode = kbTextInput;
    if (!topView)
        return;
    bool withAlt;
    uint32_t letter = keyLetter(event, withAlt);
    if (!letter)
        return;
    if (auto *mv = dynamic_cast<TMenuView *>(topView))
        translateMenu(event, mv, letter, withAlt);
    else if (topView != TProgram::application)   // строку меню обслуживает TCabMenuBar
        if (auto *g = dynamic_cast<TGroup *>(topView))
            translateDialog(event, g, letter, withAlt);
}
