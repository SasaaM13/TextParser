// TextFileParser.cpp
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
    const size_t rows = rowCount();
    const size_t cols = colCount();

    size_t total = 0;
    if(cols != 0 && rows > (std::numeric_limits<size_t>::max() / cols)) {
        // overflow zaštita: ne alociramo ništa (signalizira "nema cache-a")
        total = 0;
    }
    else {
        total = rows * cols;
    }

    std::lock_guard<std::mutex> lock(cacheMutex_);

    cacheString_.assign(total, std::nullopt);
    cacheInt_.assign(total, std::nullopt);
    cacheDouble_.assign(total, std::nullopt);
    cacheBool_.assign(total, std::nullopt);

    seenInt_.assign(total, 0);
    seenDouble_.assign(total, 0);
    seenBool_.assign(total, 0);
}

// ===================================================
//    Key helper — bounds + overflow safe
// ===================================================
std::optional<size_t> TextFileParser::keyChecked(size_t r, size_t c) const
{
    const size_t rows = rowCount();
    const size_t cols = colCount();
    if(r >= rows || c >= cols) return std::nullopt;

    // overflow check for r*cols + c
    if(cols != 0 && r > (std::numeric_limits<size_t>::max() - c) / cols) {
        return std::nullopt;
    }
    return r * cols + c;
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

    // Fallback na stod (locale-dependent). Ako ti je ulaz uvijek ".", OK.
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
    for(unsigned char c : s) lower.push_back(static_cast<char>(std::tolower(c)));

    const uint8_t f = static_cast<uint8_t>(fmt);

    if(f & static_cast<uint8_t>(BoolFormat::TRUE_FALSE)) {
        if(lower == "true") { out = true;  return true; }
        if(lower == "false") { out = false; return true; }
    }
    if(f & static_cast<uint8_t>(BoolFormat::YES_NO)) {
        if(lower == "yes") { out = true;  return true; }
        if(lower == "no") { out = false; return true; }
    }

    return false;
}

// ===================================================
//    Tipizirani interfejs — koristi brzi valueView()
// ===================================================
std::optional<int> TextFileParser::toInt(size_t r, size_t c)
{
    const auto kOpt = keyChecked(r, c);
    if(!kOpt) return std::nullopt;
    const size_t k = *kOpt;

    std::lock_guard<std::mutex> lock(cacheMutex_);
    if(k >= cacheInt_.size()) return std::nullopt;

    if(seenInt_[k]) return cacheInt_[k];
    seenInt_[k] = 1;

    int v;
    if(parseInt(valueView(r, c), v)) cacheInt_[k] = v;
    else cacheInt_[k] = std::nullopt;

    return cacheInt_[k];
}

std::optional<double> TextFileParser::toDouble(size_t r, size_t c)
{
    const auto kOpt = keyChecked(r, c);
    if(!kOpt) return std::nullopt;
    const size_t k = *kOpt;

    std::lock_guard<std::mutex> lock(cacheMutex_);
    if(k >= cacheDouble_.size()) return std::nullopt;

    if(seenDouble_[k]) return cacheDouble_[k];
    seenDouble_[k] = 1;

    double v;
    if(parseDouble(valueView(r, c), v)) cacheDouble_[k] = v;
    else cacheDouble_[k] = std::nullopt;

    return cacheDouble_[k];
}

std::optional<bool> TextFileParser::toBool(size_t r, size_t c, BoolFormat fmt)
{
    // BUG FIX: cacheBool_ nije validan ako se poziva sa različitim fmt.
    // Minimalno: keširamo samo DefaultBoolFmt, ostalo ide bez cache-a.
    if(static_cast<uint8_t>(fmt) != static_cast<uint8_t>(DefaultBoolFmt)) {
        bool v;
        if(parseBool(valueView(r, c), v, fmt)) return v;
        return std::nullopt;
    }

    const auto kOpt = keyChecked(r, c);
    if(!kOpt) return std::nullopt;
    const size_t k = *kOpt;

    std::lock_guard<std::mutex> lock(cacheMutex_);
    if(k >= cacheBool_.size()) return std::nullopt;

    if(seenBool_[k]) return cacheBool_[k];
    seenBool_[k] = 1;

    bool v;
    if(parseBool(valueView(r, c), v, fmt)) cacheBool_[k] = v;
    else cacheBool_[k] = std::nullopt;

    return cacheBool_[k];
}
