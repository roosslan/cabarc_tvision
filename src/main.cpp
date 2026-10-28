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
    "cabine.exe -ai <путь> [...]       то же, но имя архива предлагается в окне\n"
    "cabine.exe -x <архив> [...]       распаковать архив (CAB, ZIP, RAR) в его же директорию\n"
    "cabine.exe -e <архив> [...]       распаковать архив в поддиректорию с его именем\n"
    "cabine.exe -ei <архив> [...]      распаковать архив в директорию, заданную в окне\n"
    "cabine.exe -c <архив> [...]       преобразовать архив (ZIP, RAR, 7Z, TAR...) в <имя>.cab\n"
    "                                  рядом с ним, сжатие максимальное; исходный не удаляется\n"
    "cabine.exe --register             добавить пункты в контекстное меню Проводника\n"
    "                                  и директорию программы в Path пользователя\n"
    "cabine.exe --unregister           убрать пункты меню и директорию из Path\n"
    "\n"
    "Вместо пути можно указать @<файл> — список путей в UTF-8, по одному в строке\n"
    "(файл удаляется после чтения).\n";

// Путь с пробелами, переданный без кавычек, Windows делит на несколько
// аргументов: «D:\dru-xrpc (1).cab» приходит как «D:\dru-xrpc» и «(1).cab».
// Несуществующий аргумент склеивается через пробел со следующими, пока
// не получится существующий файл или директория.
std::vector<std::string> joinSplitPaths(const std::vector<std::string> &args)
{
    auto exists = [](const std::string &p) { return fsu::fileExists(p) || fsu::dirExists(p); };
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i].empty() || args[i][0] == '@' || exists(args[i]))
        {
            out.push_back(args[i]);
            continue;
        }
        std::string joined = args[i];
        size_t j = i + 1;
        for (; j < args.size(); ++j)
        {
            joined += " " + args[j];
            if (exists(joined))
                break;
        }
        if (j < args.size())
        {
            out.push_back(joined);
            i = j;
        }
        else
            out.push_back(args[i]);
    }
    return out;
}

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
    bool ok = add ? registerShellMenu(exe, shell, err) : unregisterShellMenu(exe, err);
    fsu::printConsole(!ok ? err + "\n"
                      : add ? "Пункты контекстного меню Проводника добавлены.\n"
                            : "Пункты контекстного меню Проводника удалены.\n");
    if (!ok)
        return 1;
    // Директория программы — в Path пользователя: cabine запускается из любой консоли.
    std::string dir = fsu::dirName(exe);
    ok = add ? addToUserPath(dir, err) : removeFromUserPath(dir, err);
    fsu::printConsole(!ok ? err + "\n"
                      : add ? "Директория " + dir + " добавлена в Path пользователя "
                              "(в новых окнах консоли cabine запускается по имени).\n"
                            : "Директория " + dir + " убрана из Path пользователя.\n");
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
        if (opt == "-a" || opt == "-ai" || opt == "-x" || opt == "-e" || opt == "-ei" || opt == "-c")
        {
            batch = opt == "-a"  ? BatchMode::Add
                  : opt == "-ai" ? BatchMode::AddAsk
                  : opt == "-x"  ? BatchMode::Extract
                  : opt == "-e"  ? BatchMode::ExtractToDir
                  : opt == "-c"  ? BatchMode::Convert
                                 : BatchMode::ExtractAsk;
            std::vector<std::string> rest(args.begin() + 1, args.end());
            for (const std::string &a : joinSplitPaths(rest))
                expandList(a, paths);
            if (paths.empty())
            {
                fsu::printConsole(kUsage);
                return 2;
            }
        }
        else
            for (const std::string &a : joinSplitPaths(args))
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
