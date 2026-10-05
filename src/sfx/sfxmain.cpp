// Программа-распаковщик самораспаковывающегося архива Cabine.
// К исполняемому файлу дописан CAB-архив: он находится сразу за образом
// программы (по заголовку PE) и распаковывается в выбранную директорию.

#define Uses_TEvent
#define Uses_TRect
#define Uses_TProgram
#define Uses_TApplication
#define Uses_TDeskTop
#include <tvision/tv.h>

#include "../cabfile.h"
#include "../dialogs.h"
#include "../format.h"
#include "../fsutil.h"
#include "../hotkeys.h"

namespace {

// Сколько показывать «Готово» после распаковки.
const unsigned kDoneHoldMs = 800;

class TSfxApp : public TApplication
{
public:
    TSfxApp() :
        TProgInit(nullptr, nullptr, &TSfxApp::initDeskTop)
    {
    }

    static TDeskTop *initDeskTop(TRect r) { return new TDeskTop(r); }

    void getEvent(TEvent &event) override
    {
        TApplication::getEvent(event);
        translateCyrillicHotkey(event, TopView());
    }

    void idle() override
    {
        TApplication::idle();
        if (started)
            return;
        started = true;
        exitCode = work();
        TEvent e;
        e.what = evCommand;
        e.message.command = cmQuit;
        e.message.infoPtr = nullptr;
        putEvent(e);
    }

    int exitCode = 1;

private:
    int work();

    bool started = false;
};

int TSfxApp::work()
{
    std::string self = fsu::exePath();
    CabInfo info;
    std::string err;
    if (!cabRead(self, info, err) || info.offset == 0)
    {
        showError("В файле не найден архив CAB\n" + self);
        return 1;
    }
    if (info.hasPrev || info.hasNext)
    {
        showError("Многотомные архивы не поддерживаются");
        return 1;
    }

    std::string about = "Архив: " + fsu::baseName(self) + "\n" + pluralFiles(info.entries.size()) +
                        ", " + formatSize(info.totalSize);
    ExtractOptions opt;
    opt.dest = fsu::dirName(self);
    if (!sfxDialog(about, opt))
        return 2;

    std::vector<std::string> none;
    bool ok = runWithProgress("Распаковка", [&](CabProgress *p, std::string &e) {
        return cabExtract(self, opt.dest, none, opt.keepPaths, opt.overwrite, p, e);
    }, kDoneHoldMs);
    return ok ? 0 : 1;
}

} // namespace

int main()
{
    fsu::setConsoleTitleAndIcon(fsu::baseName(fsu::exePath()));
    TSfxApp *app = new TSfxApp;
    app->run();
    int code = app->exitCode;
    TObject::destroy(app);
    return code;
}
