#pragma once

#include <string>
#include <vector>

#define Uses_TWindow
#define Uses_TScroller
#include <tvision/tv.h>

class TTextViewer;

// Окно просмотра текстового файла. F8 переключает кодировку.
class TViewerWindow : public TWindow
{
public:
    TViewerWindow(const TRect &bounds, const std::string &title, std::string &&data);

    void handleEvent(TEvent &event) override;
    void changeBounds(const TRect &bounds) override;

private:
    void applyEncoding();
    void updateTitle();

    std::string title_;
    std::string data;
    int encoding = 0;   // 0 — UTF-8, 1 — Windows (ANSI), 2 — DOS (OEM)
    TTextViewer *viewer;
};
