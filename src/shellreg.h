#pragma once

#include <string>

// Подменю «Cabine» контекстного меню Проводника в HKCU\Software\Classes
// (права администратора не требуются):
//   файлы .cab, .zip, .rar, .7z, .gz, .tgz, .tar — «Открыть в Cabine», «Распаковать здесь»,
//     «Извлечь в ...», «Добавить в CAB-архив», «Добавить в архив ...»;
//   остальные файлы и директории — «Добавить в CAB-архив», «Добавить в архив ...».
bool registerShellMenu(const std::string &cabineExe, const std::string &shellExe, std::string &err);
bool unregisterShellMenu(std::string &err);

// Ассоциация файлов архивов с Cabine (двойной щелчок открывает архив в панели).
// ext — расширение без точки: "cab", "zip", "rar", "7z", "tar", "tgz", "gz". ProgID — Cabine.<ext>.
// Записывается в HKCU\Software\Classes; прежнее значение восстанавливается при отключении.
bool isAssociated(const std::string &ext);
bool setAssociation(const std::string &ext, bool enable, const std::string &cabineExe, std::string &err);
// Выбор программы через «Открыть с помощью» (UserChoice) имеет приоритет
// над ассоциацией и программно не меняется.
bool userChoiceOverrides(const std::string &ext);
