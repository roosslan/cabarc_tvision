#pragma once

#include <string>

// Программа-распаковщик самораспаковывающегося архива, встроенная
// в cabine.exe ресурсом.
bool sfxStub(std::string &data, std::string &err);

// Имя самораспаковывающегося архива: расширение .cab заменяется на .exe.
std::string sfxPath(const std::string &path);

// Подготовка к созданию самораспаковывающегося архива: имя меняется на .exe
// (замена существующего файла подтверждается), загружается распаковщик.
bool prepareSfx(std::string &path, std::string &stub);
