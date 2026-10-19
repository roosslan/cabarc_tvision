// Заменитель vim для тестов интерфейса: дописывает строку в конец файла
// и сразу завершается. run.ps1 кладёт его как vim.exe в начало PATH.
#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return 2;
    FILE *f = _wfopen(argv[argc - 1], L"ab");
    if (!f)
        return 1;
    fputs("\r\nEDITED-BY-FAKEVIM\r\n", f);
    fclose(f);
    return 0;
}
