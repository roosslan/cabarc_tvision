#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#define Uses_TWindow
#include <tvision/tv.h>

#include "cabfile.h"

struct AddOptions;

enum class SortKey { None, Name, Ext, Date, Size };
enum class ColumnId { Name, Size, Date, Attr };

struct Column
{
    ColumnId id;
    int x;
    int width;
};

// Раскладка колонок списка по доступной ширине.
std::vector<Column> layoutColumns(int width);

enum class ItemKind { Parent, Dir, File, Drive };

// Строка панели: файл или директория на диске либо в архиве.
struct PanelItem
{
    std::string name;
    ItemKind kind = ItemKind::File;
    uint64_t size = 0;
    uint16_t date = 0;
    uint16_t time = 0;
    uint16_t attribs = 0;
    int entry = -1;             // индекс записи архива для файла в архиве
    std::wstring wName, wExt;   // ключи сортировки
};

class THeaderView;
class TPanelList;
class TInfoBar;

// Панель файлового менеджера. Показывает директорию на диске или директорию
// внутри CAB-архива; вход в архив и выход из него — как в обычную директорию.
class TPanelWindow : public TWindow
{
public:
    explicit TPanelWindow(const TRect &bounds);

    void handleEvent(TEvent &event) override;
    void sizeLimits(TPoint &min, TPoint &max) override;
    void changeBounds(const TRect &bounds) override;

    bool openDirectory(const std::string &dir, const std::string &focus = std::string());
    bool openArchive(const std::string &path, const std::string &innerDir = std::string(),
                     const std::string &focus = std::string());
    void goUp();
    void refresh();
    void sortBy(SortKey key);
    void sortByColumn(ColumnId column);
    void saveState() const;

    bool inArchive() const { return archiveMode; }
    bool readOnly() const { return !parents.empty(); }

    std::vector<PanelItem> items;   // в порядке отображения
    std::vector<char> marked;       // пометки по индексам items
    SortKey sortKey = SortKey::Name;
    bool descending = false;
    bool archiveMode = false;
    CabInfo info;                   // открытый архив
    std::string fsDir;              // директория на диске (в архиве — директория архива)
    std::string inner;              // директория внутри архива, "" — корень

private:
    // Родительский архив при входе во вложенный.
    struct Frame
    {
        std::string archive, inner, focus, display;
    };

    void rebuild(const std::string &focus, const std::unordered_set<std::string> &marks);
    void resort();
    void loadArchiveItems();
    bool loadDirectoryItems(const std::string &dir, std::vector<PanelItem> &out);
    void setArchive(CabInfo &&newInfo);
    void updateTitle();
    std::string location() const;
    void focusName(const std::string &name);
    std::unordered_set<std::string> markedNames() const;

    int focusedIndex() const;
    std::vector<int> targets() const;
    std::vector<std::string> entryNames(const std::vector<int> &indexes) const;
    std::string archiveName(const PanelItem &item) const;
    std::string focusedCab() const;
    std::string extractToTemp(const std::string &entryName);
    void marksChanged();

    void enterItem(int index);
    void enterNested(const std::string &tempPath, const std::string &name);
    void addFiles();
    void extractFiles();
    void deleteFiles();
    void deleteFromDisk();
    void viewFile();
    void testArchive();
    void showProperties();
    void markAll(bool mark);
    void invertMarks();
    void markByMask(bool mark);

    std::vector<Frame> parents;
    std::vector<std::string> upperNames;   // имена записей архива в верхнем регистре
    std::string archiveDisplay;            // путь архива для заголовка
    std::string extractDir;
    THeaderView *header;
    TPanelList *list;
    TInfoBar *infoBar;
};

// Раскрывает элементы диалога добавления в список файлов для архива.
bool collectSources(const AddOptions &opt, std::vector<CabSource> &sources);
