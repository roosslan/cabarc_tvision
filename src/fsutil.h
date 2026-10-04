#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Вспомогательные функции для работы с файловой системой Windows.
// Все строки в UTF-8 (так же, как в Turbo Vision); внутри они
// преобразуются в UTF-16 для вызова W-функций Win32.
namespace fsu {

std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);
std::string fromCodePage(const std::string &s, unsigned codePage);
std::string upper(const std::string &s);
// Сравнение имён как в Проводнике: без учёта регистра, числа по значению.
// Возвращает <0, 0 или >0.
int compareNatural(const std::wstring &a, const std::wstring &b);
bool isAscii(const std::string &s);
bool isValidUtf8(const std::string &s);

bool fileExists(const std::string &path);
bool dirExists(const std::string &path);
bool makeDirs(const std::string &path);
bool removeFile(const std::string &path);
bool removeTree(const std::string &path);
bool moveReplace(const std::string &from, const std::string &to);
void clearReadOnly(const std::string &path);
uint64_t fileSize(const std::string &path);
bool readFile(const std::string &path, std::string &data, uint64_t maxSize);

std::string currentDir();
bool setCurrentDir(const std::string &path);
std::string fullPath(const std::string &path);
std::string joinPath(const std::string &dir, const std::string &name);
std::string dirName(const std::string &path);
std::string baseName(const std::string &path);
std::string extension(const std::string &name);
std::string stripExt(const std::string &name);
std::string withSlash(const std::string &dir);
// Приводит имя из архива к безопасному относительному пути:
// убирает диск, ведущие разделители, компоненты "." и "..".
std::string safeRelPath(const std::string &name);
std::string makeTempDir();
std::string lastErrorText();

// Сравнение имени с масками вида "*.txt;*.doc" без учёта регистра.
bool wildMatch(const std::string &masks, const std::string &name);

struct DirItem
{
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    uint16_t date = 0;      // формат MS-DOS, местное время
    uint16_t time = 0;
    uint16_t attribs = 0;   // FILE_ATTRIBUTE_* (младшие биты)
};

// Содержимое директории без "." и ".." (служебные скрытые системные
// объекты пропускаются).
bool listDirectory(const std::string &dir, std::vector<DirItem> &out);
std::vector<std::string> logicalDrives();   // "C:\", "D:\", ...
bool isRootDir(const std::string &dir);
bool isAbsolutePath(const std::string &path);
uint64_t freeSpace(const std::string &dir);
// Ближайшая существующая директория из указанной (иначе текущая).
std::string existingDir(const std::string &dir);

struct FoundFile
{
    std::string path;   // полный путь на диске
    std::string name;   // имя для архива
};

// Раскрывает элемент списка добавления (файл, директорию или маску)
// в список файлов.
bool collectFiles(const std::string &item, bool recurse, bool keepPaths,
                  std::vector<FoundFile> &out, std::string &err);

bool shellOpen(const std::string &path, std::string &err);
std::vector<std::string> commandLineArgs();
std::string exePath();
void printConsole(const std::string &text);   // вывод UTF-8 в консоль
// Удаление файлов и директорий (с содержимым) в корзину.
bool recycle(const std::vector<std::string> &paths, std::string &err);

// Настройки приложения в %APPDATA%\Cabine\cabine.ini.
std::string loadSetting(const std::string &key, const std::string &def);
void saveSetting(const std::string &key, const std::string &value);

} // namespace fsu
