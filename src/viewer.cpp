#define Uses_TKeys
#define Uses_TEvent
#define Uses_TRect
#define Uses_TWindow
#define Uses_TFrame
#define Uses_TScroller
#define Uses_TScrollBar
#define Uses_TDrawBuffer
#include <tvision/tv.h>

#include <algorithm>
#include <cstring>

#include "fsutil.h"
#include "viewer.h"

namespace {

const unsigned kCodePages[] = {65001, 0 /* CP_ACP */, 1 /* CP_OEMCP */};
const char *const kEncodingNames[] = {"UTF-8", "Windows", "DOS"};

} // namespace

class TTextViewer : public TScroller
{
public:
    TTextViewer(const TRect &bounds, TScrollBar *h, TScrollBar *v) :
        TScroller(bounds, h, v),
        hBar(h),
        vBar(v)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        eventMask |= evMouseWheel;
    }

    void setText(const std::string &text)
    {
        lines.clear();
        int width = 0;
        std::string line;
        auto flush = [&]() {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            width = std::max(width, strwidth(line));
            lines.push_back(std::move(line));
            line.clear();
        };
        for (char c : text)
        {
            if (c == '\n')
                flush();
            else if (c == '\t')
                line.append(8 - strwidth(line) % 8, ' ');
            else
                line += c;
        }
        if (!line.empty() || lines.empty())
            flush();
        setLimit(width, (int) lines.size());
        scrollTo(0, 0);
        drawView();
    }

    void draw() override
    {
        TColorAttr c = getColor(1);
        for (int y = 0; y < size.y; ++y)
        {
            TDrawBuffer b;
            b.moveChar(0, ' ', c, size.x);
            size_t i = size_t(delta.y + y);
            if (i < lines.size())
                b.moveStr(0, lines[i], c, size.x, delta.x);
            writeLine(0, y, size.x, 1, b);
        }
    }

    void handleEvent(TEvent &ev) override
    {
        if (ev.what == evMouseWheel)
        {
            TScrollBar *bar = (ev.mouse.wheel == mwLeft || ev.mouse.wheel == mwRight) ? hBar : vBar;
            if (bar)
                bar->handleEvent(ev);
            clearEvent(ev);
            return;
        }
        TScroller::handleEvent(ev);
    }

private:
    std::vector<std::string> lines;
    TScrollBar *hBar;
    TScrollBar *vBar;
};

TViewerWindow::TViewerWindow(const TRect &bounds, const std::string &title, std::string &&text) :
    TWindowInit(&TWindow::initFrame),
    TWindow(bounds, title, wnNoNumber),
    title_(title),
    data(std::move(text))
{
    // Просмотр во весь экран; Esc или Alt+F3 закрывает окно.
    flags &= ~(wfMove | wfGrow | wfZoom);
    growMode = gfGrowHiX | gfGrowHiY;
    TScrollBar *h = standardScrollBar(sbHorizontal | sbHandleKeyboard);
    TScrollBar *v = standardScrollBar(sbVertical | sbHandleKeyboard);
    viewer = new TTextViewer(getExtent().grow(-1, -1), h, v);
    insert(viewer);

    // UTF-16 с BOM преобразуется сразу; остальное — по признаку корректного UTF-8.
    if (data.size() >= 2 && (unsigned char) data[0] == 0xFF && (unsigned char) data[1] == 0xFE)
    {
        std::wstring w((data.size() - 2) / 2, L'\0');
        memcpy(&w[0], data.data() + 2, w.size() * 2);
        data = fsu::narrow(w);
    }
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0)
        data.erase(0, 3);
    encoding = fsu::isValidUtf8(data) ? 0 : 1;
    applyEncoding();
}

void TViewerWindow::applyEncoding()
{
    viewer->setText(encoding == 0 ? data : fsu::fromCodePage(data, kCodePages[encoding]));
    updateTitle();
}

// Длинный путь укорачивается слева, имя файла и подсказка остаются видны.
void TViewerWindow::updateTitle()
{
    std::string hint = std::string(" [") + kEncodingNames[encoding] + " — F8, закрыть — Esc]";
    std::string path = title_;
    int maxW = std::max(10, size.x - 14 - strwidth(hint));
    if (strwidth(path) > maxW)
    {
        while (!path.empty() && strwidth(path) > maxW - 1)
        {
            size_t n = 1;
            while (n < path.size() && ((unsigned char) path[n] & 0xC0) == 0x80)
                ++n;
            path.erase(0, n);
        }
        path = "…" + path;
    }
    delete[] (char *) title;
    title = newStr(path + hint);
    if (frame)
        frame->drawView();
}

void TViewerWindow::changeBounds(const TRect &bounds)
{
    TWindow::changeBounds(bounds);
    updateTitle();
}

void TViewerWindow::handleEvent(TEvent &event)
{
    TWindow::handleEvent(event);
    if (event.what != evKeyDown)
        return;
    if (event.keyDown.keyCode == kbF8)
    {
        encoding = (encoding + 1) % 3;
        applyEncoding();
        clearEvent(event);
    }
    else if (event.keyDown.keyCode == kbEsc)
    {
        // Закрытие через очередь: окно не уничтожается внутри своего обработчика.
        event.what = evCommand;
        event.message.command = cmClose;
        event.message.infoPtr = this;
        putEvent(event);
        clearEvent(event);
    }
}
