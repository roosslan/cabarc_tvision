#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cabfile.h"

// Архивы любого поддерживаемого формата: CAB (Cabinet API, чтение и запись),
// ZIP (minizip-ng) и RAR (UnRAR) — только просмотр, проверка и распаковка.
// Формат определяется по сигнатуре, а не по расширению.

ArchiveFormat archiveFormat(const std::string &path, bool sfx = false);
inline bool isArchive(const std::string &path, bool sfx = false)
{
    return archiveFormat(path, sfx) != ArchiveFormat::Unknown;
}
const char *formatName(ArchiveFormat format);

// Расширения файлов архивов (без учёта регистра): .cab, .zip, .rar.
bool hasArchiveExtension(const std::string &name);

bool archRead(const std::string &path, CabInfo &info, std::string &err);
bool archExtract(const std::string &path, const std::string &destDir,
                 const std::vector<std::string> &names, bool keepPaths, Overwrite ow,
                 CabProgress *progress, std::string &err, CabResult *result = nullptr,
                 const std::string &base = std::string());
bool archTest(const std::string &path, CabProgress *progress, std::string &err,
              CabResult *result = nullptr);

// Запрос пароля зашифрованного архива. retry — предыдущий пароль не подошёл.
// Возврат false — пользователь отказался. Введённые пароли запоминаются
// для архива до конца работы программы.
using PasswordPrompt = std::function<bool(const std::string &archive, bool retry, std::string &password)>;
void setPasswordPrompt(PasswordPrompt prompt);
