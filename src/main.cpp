#include "app.h"
#include "fsutil.h"
#include "shellreg.h"
#include "version.h"

namespace {

const char *const kUsage =
    "Cabine " CABINE_VERSION " — файловый менеджер и архиватор CAB\n"
    CABINE_COPYRIGHT "\n"
    "\n"
    "cabine.exe                        панель на месте, где программу закрыли\n"
    "cabine.exe <директория | архив>   панель в указанном месте\n"
    "cabine.exe -a <путь> [<путь>...]  добавить файлы и директории в архив <имя>.cab\n"
    "                                  рядом с ними (с сохранением структуры)\n"
    "cabine.exe -x <архив> [...]       распаковать архив в его же директорию\n"
    "cabine.exe --register             добавить пункты в контекстное меню Проводника\n"
    "cabine.exe --unregister           убрать пункты из контекстного меню Проводника\n"
    "\n"
    "Вместо пути можно указать @<файл> — список путей в UTF-8, по одному в строке\n"
    "(файл удаляется после чтения).\n";

// @файл — список путей; создаётся cabine-shell.exe и удаляется после чтения.
void expandList(const std::string &arg, std::vector<std::string> &out)
{
    if (arg.size() < 2 || arg[0] != '@')
    {
        out.push_back(arg);
        return;
    }
    std::string file = arg.substr(1), data;
    if (!fsu::readFile(file, data, 64 * 1024 * 1024))
        return;
    fsu::removeFile(file);
    size_t start = 0;
    while (start < data.size())
    {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos)
            nl = data.size();
        std::string line = data.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            out.push_back(line);
        start = nl + 1;
    }
}

int shellMenu(bool add)
{
    std::string exe = fsu::exePath();
    std::string shell = fsu::joinPath(fsu::dirName(exe), "cabine-shell.exe");
    std::string err;
    if (add && !fsu::fileExists(shell))
    {
        fsu::printConsole("Не найден " + shell + "\n");
        return 1;
    }
    bool ok = add ? registerShellMenu(exe, shell, err) : unregisterShellMenu(err);
    fsu::printConsole(!ok ? err + "\n"
                      : add ? "Пункты контекстного меню Проводника добавлены.\n"
                            : "Пункты контекстного меню Проводника удалены.\n");
    return ok ? 0 : 1;
}

} // namespace

int main()
{
    // Аргументы читаются в Юникоде: argv содержит пути в кодировке ANSI.
    std::vector<std::string> args = fsu::commandLineArgs();
    BatchMode batch = BatchMode::None;
    std::vector<std::string> paths;

    if (!args.empty())
    {
        const std::string &opt = args[0];
        if (opt == "-h" || opt == "--help" || opt == "/?")
        {
            fsu::printConsole(kUsage);
            return 0;
        }
        if (opt == "--register" || opt == "--unregister")
            return shellMenu(opt == "--register");
        if (opt == "-a" || opt == "-x")
        {
            batch = opt == "-a" ? BatchMode::Add : BatchMode::Extract;
            for (size_t i = 1; i < args.size(); ++i)
                expandList(args[i], paths);
            if (paths.empty())
            {
                fsu::printConsole(kUsage);
                return 2;
            }
        }
        else
            for (const std::string &a : args)
                expandList(a, paths);
    }

    fsu::setConsoleTitleAndIcon("Cabine " CABINE_VERSION);
    TCabineApp *app = new TCabineApp;
    if (batch != BatchMode::None)
        app->setBatch(batch, paths);
    else
        for (const std::string &path : paths)
            app->openLater(path);
    app->run();
    app->cleanup();
    TObject::destroy(app);
    return 0;
}
