#include <cstdio>

#include "format.h"
#include "fsutil.h"

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

bool parseSize(const std::string &textIn, uint64_t &bytes)
{
    std::string text = fsu::upper(textIn);
    size_t a = text.find_first_not_of(' '), b = text.find_last_not_of(' ');
    text = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
    bytes = 0;
    if (text.empty() || text.rfind("НЕ ", 0) == 0 || text == "0")
        return true;

    size_t pos = 0;
    double value = 0, frac = 0.1;
    bool digits = false, point = false;
    for (; pos < text.size(); ++pos)
    {
        char c = text[pos];
        if (c >= '0' && c <= '9')
        {
            digits = true;
            if (point)
            {
                value += (c - '0') * frac;
                frac /= 10;
            }
            else
                value = value * 10 + (c - '0');
        }
        else if ((c == ',' || c == '.') && !point)
            point = true;
        else
            break;
    }
    if (!digits)
        return false;
    std::string unit = text.substr(pos);
    unit.erase(0, unit.find_first_not_of(' '));
    double mult;
    if (unit.empty() || unit == "Б" || unit == "B")
        mult = 1;
    else if (unit == "КБ" || unit == "К" || unit == "KB" || unit == "K")
        mult = 1024.0;
    else if (unit == "МБ" || unit == "М" || unit == "MB" || unit == "M")
        mult = 1024.0 * 1024;
    else if (unit == "ГБ" || unit == "Г" || unit == "GB" || unit == "G")
        mult = 1024.0 * 1024 * 1024;
    else
        return false;
    // «1,44 Мб» — дискета: 1440 Кб, как принято для её размера.
    if (mult == 1024.0 * 1024 && value > 1.4399 && value < 1.4401)
        bytes = 1457664;
    else
        bytes = (uint64_t) (value * mult + 0.5);
    return true;
}
