#pragma once

#include <string>

// Пункты контекстного меню Проводника в HKCU\Software\Classes
// (права администратора не требуются):
//   файлы и директории — «Добавить в CAB-архив»;
//   файлы .cab — «Распаковать здесь» и «Открыть в Cabine».
bool registerShellMenu(const std::string &cabineExe, const std::string &shellExe, std::string &err);
bool unregisterShellMenu(std::string &err);
