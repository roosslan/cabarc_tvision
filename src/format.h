#pragma once

#include <cstdint>
#include <string>

// Форматирование значений для отображения.
std::string formatNumber(uint64_t n);                       // "1 234 567"
std::string formatSize(uint64_t n);                         // "1,2 Мб"
std::string formatDateTime(uint16_t date, uint16_t time);   // "15.03.2024 14:22"
std::string formatAttr(uint16_t attribs);                   // "RHSA"
std::string formatRatio(uint64_t packed, uint64_t total);   // "37,1 %"
std::string pluralFiles(uint64_t n);                        // "1 файл", "3 файла", "5 файлов"

// Размер с единицами: "100 Мб", "1,44 Мб", "2 Гб", "512 Кб", "4096" (байты).
// Пустая строка и «Не делить» — 0. Возвращает false при неверной записи.
bool parseSize(const std::string &text, uint64_t &bytes);
