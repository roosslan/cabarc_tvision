#pragma once

#include <cstdint>
#include <string>

// Программа-распаковщик самораспаковывающегося архива, встроенная
// в cabine.exe ресурсом.
bool sfxStub(std::string &data, std::string &err);

// Имя самораспаковывающегося архива: расширение .cab заменяется на .exe.
std::string sfxPath(const std::string &path);

// Подготовка к созданию самораспаковывающегося архива: имя меняется на .exe
// (замена существующего файла подтверждается), загружается распаковщик.
bool prepareSfx(std::string &path, std::string &stub);

// Многотомный архив: если рядом уже есть файлы томов (<имя>2.cab...),
// спрашивает, заменять ли их. Без томов (volumeSize == 0) — true.
bool confirmVolumes(const std::string &firstVolume, uint64_t volumeSize);

// Имя архива изменилось в окне (галочка SFX сменила расширение): если файл
// с новым именем уже есть, спрашивает, заменять ли его.
bool confirmChangedTarget(const std::string &original, const std::string &target);
