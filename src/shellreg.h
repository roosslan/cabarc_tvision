#pragma once

#include <string>

// Подменю «Cabine» контекстного меню Проводника в HKCU\Software\Classes
// (права администратора не требуются):
//   файлы .cab, .zip, .rar — «Открыть в Cabine», «Распаковать здесь»,
//     «Извлечь в директорию с именем архива», «Добавить в CAB-архив»;
//   остальные файлы и директории — «Добавить в CAB-архив».
bool registerShellMenu(const std::string &cabineExe, const std::string &shellExe, std::string &err);
bool unregisterShellMenu(std::string &err);

// Ассоциация файлов .cab с Cabine (двойной щелчок открывает архив в панели).
// Записывается в HKCU\Software\Classes; прежнее значение восстанавливается при отключении.
bool isCabAssociated();
bool setCabAssociation(bool enable, const std::string &cabineExe, std::string &err);
// Выбор программы для .cab через «Открыть с помощью» (UserChoice) имеет приоритет
// над ассоциацией и программно не меняется.
bool cabUserChoiceOverrides();
