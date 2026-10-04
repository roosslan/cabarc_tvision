#pragma once

// Команды приложения. Значения меньше 256, чтобы их можно было запрещать.
enum : unsigned short
{
    cmNewArchive    = 100,
    cmOpenArchive,
    cmOptions,
    cmAbout,

    // Команды панели (разрешены, только когда она активна).
    cmPanelFirst    = 110,
    cmAddFiles      = cmPanelFirst,
    cmExtract,
    cmDeleteFiles,
    cmViewFile,
    cmOpenFile,
    cmTestArchive,
    cmArchiveInfo,
    cmReload,
    cmSelectAll,
    cmUnselectAll,
    cmInvertSel,
    cmSelectMask,
    cmUnselectMask,
    cmSortName,
    cmSortExt,
    cmSortDate,
    cmSortSize,
    cmSortNone,
    cmGoUp,
    cmPanelLast     = cmGoUp,

    // Внутренние команды диалогов.
    cmAddItemFile   = 140,
    cmAddItemDir,
    cmAddItemMask,
    cmRemoveItem,
    cmBrowseDir,
    cmYesAll,
    cmNoAll,

    // Уведомления.
    cmMarksChanged  = 150,
};
