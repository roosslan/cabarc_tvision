#include <cstdio>

#include "format.h"

std::string formatNumber(uint64_t n)
{
    std::string digits = std::to_string(n), result;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (count && count % 3 == 0)
            result.insert(result.begin(), ' ');
        result.insert(result.begin(), *it);
        ++count;
    }
    return result;
}

static std::string decimal(double value, const char *unit)
{
    char buf[32];
    snprintf(buf, sizeof(buf), value < 10 ? "%.1f %s" : "%.0f %s", value, unit);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos)
        s[dot] = ',';
    return s;
}

std::string formatSize(uint64_t n)
{
    if (n < 1024)
        return std::to_string(n) + " Б";
    if (n < 1024 * 1024)
        return decimal(n / 1024.0, "Кб");
    if (n < 1024ull * 1024 * 1024)
        return decimal(n / (1024.0 * 1024), "Мб");
    return decimal(n / (1024.0 * 1024 * 1024), "Гб");
}

std::string formatDateTime(uint16_t date, uint16_t time)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u.%02u.%04u %02u:%02u",
             date & 0x1F, (date >> 5) & 0x0F, 1980 + (date >> 9),
             time >> 11, (time >> 5) & 0x3F);
    return buf;
}

std::string formatAttr(uint16_t attribs)
{
    std::string s = "----";
    if (attribs & 0x01) s[0] = 'R';
    if (attribs & 0x02) s[1] = 'H';
    if (attribs & 0x04) s[2] = 'S';
    if (attribs & 0x20) s[3] = 'A';
    return s;
}

std::string formatRatio(uint64_t packed, uint64_t total)
{
    if (total == 0)
        return "—";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %%", packed * 100.0 / total);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos)
        s[dot] = ',';
    return s;
}

std::string pluralFiles(uint64_t n)
{
    uint64_t mod100 = n % 100, mod10 = n % 10;
    const char *word = "файлов";
    if (mod100 < 11 || mod100 > 14)
    {
        if (mod10 == 1)
            word = "файл";
        else if (mod10 >= 2 && mod10 <= 4)
            word = "файла";
    }
    return formatNumber(n) + " " + word;
}
