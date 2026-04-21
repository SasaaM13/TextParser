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
    cacheString_.clear();
    cacheInt_.clear();
    cacheDouble_.clear();
    cacheBool_.clear();

    seenInt_.clear();
    seenDouble_.clear();
    seenBool_.clear();
}
void TextFileParser::ensureStringCacheSize() const
{
    const size_t rows = rowCount();
    const size_t cols = colCount();

    if(cols == 0 || rows == 0) return;
    if(rows > std::numeric_limits<size_t>::max() / cols) return;

    const size_t total = rows * cols;
    if(cacheString_.size() != total)
        cacheString_.resize(total);
}

void TextFileParser::ensureIntCacheSize() const
{
    const size_t rows = rowCount();
    const size_t cols = colCount();

    if(cols == 0 || rows == 0) return;
    if(rows > std::numeric_limits<size_t>::max() / cols) return;

    const size_t total = rows * cols;
    if(cacheInt_.size() != total) cacheInt_.resize(total);
    if(seenInt_.size() != total) seenInt_.assign(total, 0);
}

void TextFileParser::ensureDoubleCacheSize() const
{
    const size_t rows = rowCount();
    const size_t cols = colCount();

    if(cols == 0 || rows == 0) return;
    if(rows > std::numeric_limits<size_t>::max() / cols) return;

    const size_t total = rows * cols;
    if(cacheDouble_.size() != total) cacheDouble_.resize(total);
    if(seenDouble_.size() != total) seenDouble_.assign(total, 0);
}

void TextFileParser::ensureBoolCacheSize() const
{
    const size_t rows = rowCount();
    const size_t cols = colCount();

    if(cols == 0 || rows == 0) return;
    if(rows > std::numeric_limits<size_t>::max() / cols) return;

    const size_t total = rows * cols;
    if(cacheBool_.size() != total) cacheBool_.resize(total);
    if(seenBool_.size() != total) seenBool_.assign(total, 0);
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

    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

static bool ieq(std::string_view a, const char* b)
{
    size_t n = a.size();
    for(size_t i = 0; i < n; ++i)
    {
        char ca = a[i];
        char cb = b[i];

        if(cb == '\0') return false;

        if(ca >= 'A' && ca <= 'Z') ca += 32;
        if(ca != cb) return false;
    }
    return b[n] == '\0';
}


bool TextFileParser::parseBool(std::string_view s, bool& out, BoolFormat fmt)
{
    if(s.empty()) return false;

    const uint8_t f = static_cast<uint8_t>(fmt);

    if(f & static_cast<uint8_t>(BoolFormat::TRUE_FALSE)) {
        if(ieq(s, "true")) { out = true;  return true; }
        if(ieq(s, "false")) { out = false; return true; }
    }

    if(f & static_cast<uint8_t>(BoolFormat::YES_NO)) {
        if(ieq(s, "yes")) { out = true;  return true; }
        if(ieq(s, "no")) { out = false; return true; }
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
    ensureIntCacheSize();
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
    ensureDoubleCacheSize();
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
    ensureBoolCacheSize();
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if(k >= cacheBool_.size()) return std::nullopt;

    if(seenBool_[k]) return cacheBool_[k];
    seenBool_[k] = 1;

    bool v;
    if(parseBool(valueView(r, c), v, fmt)) cacheBool_[k] = v;
    else cacheBool_[k] = std::nullopt;

    return cacheBool_[k];
}
