#include "TextFileParser.h"
#include <cctype>
#include <cstdlib>
#include <charconv>
#include <string_view>

// ===================================================
//    Notification: poziva se nakon uspešnog učitavanja
// ===================================================
void TextFileParser::notifyLoaded()
{
    const size_t total = rowCount() * colCount();
    cacheString_.assign(total, std::nullopt); // lazy string cache
    cacheInt_.assign(total, std::nullopt);
    cacheDouble_.assign(total, std::nullopt);
    cacheBool_.assign(total, std::nullopt);
}

// ===================================================
//    Parsers — brze konverzije iz string_view
// ===================================================

bool TextFileParser::parseInt(std::string_view s, int& out)
{
    if(s.empty()) return false;
    const char* first = s.data();
    const char* last = first + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return (ec == std::errc() && ptr == last);
}

bool TextFileParser::parseDouble(std::string_view s, double& out)
{
    if(s.empty()) return false;
    // std::from_chars za double još nije univerzalno brz svuda, fallback:
    try {
        std::string tmp(s);
        size_t pos = 0;
        double v = std::stod(tmp, &pos);
        if(pos == tmp.size()) { out = v; return true; }
    }
    catch(...) {}
    return false;
}

bool TextFileParser::parseBool(std::string_view s, bool& out, BoolFormat fmt)
{
    if(s.empty()) return false;

    std::string lower;
    lower.reserve(s.size());
    for(unsigned char c : s) lower.push_back(std::tolower(c));

    const uint8_t f = static_cast<uint8_t>(fmt);

    if(f & static_cast<uint8_t>(BoolFormat::TRUE_FALSE)) {
        if(lower == "true") { out = true;  return true; }
        if(lower == "false") { out = false; return true; }
    }
    if(f & static_cast<uint8_t>(BoolFormat::YES_NO)) {
        if(lower == "yes") { out = true;  return true; }
        if(lower == "no") { out = false; return true; }
    }
    /*if(f & static_cast<uint8_t>(BoolFormat::ONE_ZERO)) {
        if(s == "1") { out = true;  return true; }
        if(s == "0") { out = false; return true; }
    }*/
    return false;
}

// ===================================================
//    Tipizirani interfejs — koristi brzi valueView()
// ===================================================

std::optional<int> TextFileParser::toInt(size_t r, size_t c)
{
    const size_t k = key(r, c);
    if(k >= cacheInt_.size()) return std::nullopt;
    if(cacheInt_[k].has_value()) return cacheInt_[k];

    int v;
    if(parseInt(valueView(r, c), v)) return cacheInt_[k] = v;
    return cacheInt_[k] = std::nullopt;
}

std::optional<double> TextFileParser::toDouble(size_t r, size_t c)
{
    const size_t k = key(r, c);
    if(k >= cacheDouble_.size()) return std::nullopt;
    if(cacheDouble_[k].has_value()) return cacheDouble_[k];

    double v;
    if(parseDouble(valueView(r, c), v)) return cacheDouble_[k] = v;
    return cacheDouble_[k] = std::nullopt;
}

std::optional<bool> TextFileParser::toBool(size_t r, size_t c, BoolFormat fmt)
{
    const size_t k = key(r, c);
    if(k >= cacheBool_.size()) return std::nullopt;
    if(cacheBool_[k].has_value()) return cacheBool_[k];

    bool v;
    if(parseBool(valueView(r, c), v, fmt)) return cacheBool_[k] = v;
    return cacheBool_[k] = std::nullopt;
}
