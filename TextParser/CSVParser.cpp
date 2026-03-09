#define _CRT_SECURE_NO_WARNINGS
#include "CSVParser.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#include <immintrin.h>
#define CSV_HAS_AVX2 1
#else
#define CSV_HAS_AVX2 0
#endif

// ============================================================
// ULTRA-FAST CLEANUP
// ============================================================
static inline size_t trimCRandSemisLen(const char* base, size_t off, size_t len)
{
    if(len == 0) return 0;
    size_t n = len;

    if(n && base[off + n - 1] == '\r') --n;
    if(n && base[off + n - 1] == ';') --n;
    if(n && base[off + n - 1] == ';') --n;

    return n;
}

// ==============================
// ctor / reset
// ==============================
CSVParser::CSVParser(std::string filename, Options opts)
    : filename_(std::move(filename)), opts_(opts) {
}

void CSVParser::resetState()
{
    rows_ = cols_ = 0;
    cells_.clear();
    headerIndex_.clear();
    backing_.reset();
}

// ==============================
// Buffer release
// ==============================
void CSVParser::Buffer::release()
{
#ifdef _WIN32
    if(data && mmapped) UnmapViewOfFile(data);
    if(hMap) CloseHandle((HANDLE)hMap);
    if(hFile) CloseHandle((HANDLE)hFile);
    data = nullptr; size = 0; mmapped = false;
    hFile = nullptr; hMap = nullptr;
#else
    if(data && mmapped) munmap((void*)data, size);
    if(fd >= 0) close(fd);
    data = nullptr; size = 0; mmapped = false; fd = -1;
#endif
    owned.clear();
}

// ==============================
// mmap / buffered read
// ==============================
bool CSVParser::mapFile(Buffer& buf) const
{
#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if(!GetFileSizeEx(hFile, &sz)) { CloseHandle(hFile); return false; }
    if(sz.QuadPart == 0) { CloseHandle(hFile); return false; }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!hMap) { CloseHandle(hFile); return false; }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if(!view) { CloseHandle(hMap); CloseHandle(hFile); return false; }

    buf.data = (const char*)view;
    buf.size = (size_t)sz.QuadPart;
    buf.mmapped = true;
    buf.hFile = hFile;
    buf.hMap = hMap;
    return true;
#else
    int fd = open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;

    struct stat sb {};
    if(fstat(fd, &sb) < 0) { close(fd); return false; }
    if(sb.st_size == 0) { close(fd); return false; }

    void* mem = mmap(nullptr, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mem == MAP_FAILED) { close(fd); return false; }

    buf.data = (const char*)mem;
    buf.size = (size_t)sb.st_size;
    buf.mmapped = true;
    buf.fd = fd;
    return true;
#endif
}

bool CSVParser::readFileBuffered(Buffer& buf) const
{
    std::FILE* f = std::fopen(filename_.c_str(), "rb");
    if(!f) return false;

    size_t size = 0;
    try { size = (size_t)std::filesystem::file_size(filename_); }
    catch(...) { size = 0; }

    buf.owned.resize(size);
    size_t rd = std::fread(buf.owned.data(), 1, size, f);
    std::fclose(f);

    if(rd == 0) return false;

    buf.data = buf.owned.data();
    buf.size = rd;
    buf.mmapped = false;
    return true;
}

// ==============================
// split helpers
// ==============================
void CSVParser::splitLineNoQuotes(std::string_view line, char delim,
    std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = line.data();
    size_t n = line.size();
    size_t start = 0;

    for(size_t i = 0; i < n; ++i) {
        if(p[i] == delim) {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

void CSVParser::splitLineQuotesFast(std::string_view line, char delim,
    std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = line.data();
    size_t n = line.size();
    size_t start = 0;
    bool inq = false;

    for(size_t i = 0; i < n; ++i) {
        char c = p[i];
        if(c == '"') inq = !inq;
        else if(c == delim && !inq) {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

// ==============================
// AVX2 scan
// ==============================
size_t CSVParser::findNextDelimOrNL_AVX2(const char* s, size_t pos, size_t n, char delim)
{
#if CSV_HAS_AVX2
    const __m256i vDelim = _mm256_set1_epi8(delim);
    const __m256i vNL = _mm256_set1_epi8('\n');

    size_t i = pos;
    for(; i + 32 <= n; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(s + i));
        __m256i m = _mm256_or_si256(
            _mm256_cmpeq_epi8(chunk, vDelim),
            _mm256_cmpeq_epi8(chunk, vNL));
        int mask = _mm256_movemask_epi8(m);
        if(mask) {
#ifdef _WIN32
            unsigned long idx;
            _BitScanForward(&idx, (unsigned long)mask);
#else
            unsigned idx = __builtin_ctz((unsigned)mask);
#endif
            return i + idx;
        }
    }
    for(; i < n; ++i)
        if(s[i] == delim || s[i] == '\n') return i;
    return n;
#else
    for(size_t i = pos; i < n; ++i)
        if(s[i] == delim || s[i] == '\n') return i;
    return n;
#endif
}

// ==============================
// heuristics
// ==============================
bool CSVParser::equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if(ca >= 'A' && ca <= 'Z') ca += 32;
        if(cb >= 'A' && cb <= 'Z') cb += 32;
        if(ca != cb) return false;
    }
    return true;
}

bool CSVParser::looksLikeNumber(std::string_view v)
{
    if(v.empty()) return false;
    size_t i = 0;
    if(v[i] == '+' || v[i] == '-') ++i;

    bool any = false;
    for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i) any = true;

    if(i < v.size() && v[i] == '.') {
        ++i;
        for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i) any = true;
    }

    if(!any) return false;

    if(i < v.size() && (v[i] == 'e' || v[i] == 'E')) {
        ++i;
        if(i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
        bool exp = false;
        for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i) exp = true;
        return exp && i == v.size();
    }

    return i == v.size();
}

// ==============================
// load()
// ==============================
bool CSVParser::load()
{
    resetState();

    backing_ = std::make_unique<Buffer>();
    bool ok = opts_.useMMap && mapFile(*backing_);
    if(!ok) ok = readFileBuffered(*backing_);
    if(!ok) return false;

    const char* s = backing_->data;
    const size_t n = backing_->size;
    if(!s || n == 0) { notifyLoaded(); return true; }

    size_t firstEnd = 0;
    while(firstEnd < n && s[firstEnd] != '\n') ++firstEnd;

    size_t firstLineEnd = firstEnd;
    if(firstLineEnd && s[firstLineEnd - 1] == '\r') --firstLineEnd;

    std::string_view firstLine(s, firstLineEnd);

    bool useQuotes = opts_.allowQuotes && firstLine.find('"') != std::string_view::npos;

    std::vector<std::pair<size_t, size_t>> spans;
    if(useQuotes) splitLineQuotesFast(firstLine, opts_.delimiter, spans);
    else splitLineNoQuotes(firstLine, opts_.delimiter, spans);

    cols_ = spans.size();
    if(cols_ == 0) { notifyLoaded(); return true; }

    if(opts_.hasHeader) {
        colNames_.resize(cols_);
        headerIndex_.clear();
        for(size_t c = 0; c < cols_; ++c) {
            std::string name(firstLine.substr(spans[c].first, spans[c].second));
            colNames_[c] = name;
            headerIndex_.emplace_back(name, c);
        }
        std::sort(headerIndex_.begin(), headerIndex_.end(),
            [](auto& a, auto& b) { return a.first < b.first; });
    }
    else {
        colNames_.resize(cols_);
        for(size_t c = 0; c < cols_; ++c)
            colNames_[c] = "col" + std::to_string(c + 1);
    }

    size_t dataPos = opts_.hasHeader && firstEnd < n ? firstEnd + 1 : 0;

    size_t rowCount = 0;
    for(size_t i = dataPos; i < n; ++i)
        if(s[i] == '\n') ++rowCount;
    if(n > dataPos && s[n - 1] != '\n') ++rowCount;

    rows_ = rowCount;

    if(cols_ && rows_ > std::numeric_limits<size_t>::max() / cols_)
        return false;

    cells_.resize(rows_ * cols_);

    size_t r = 0, c = 0;
    size_t cellStart = dataPos;
    bool inQuotes2 = false;

    auto setCell = [&](size_t off, size_t len, bool trim) {
        if(r >= rows_ || c >= cols_) { ++c; return; }
        if(trim && opts_.trimLastColumnCRSemis)
            len = trimCRandSemisLen(s, off, len);
        cells_[r * cols_ + c] = { off, len };
        ++c;
        };

    auto finishRow = [&]() {
        while(c < cols_) cells_[r * cols_ + c++] = { 0,0 };
        ++r; c = 0;
        };

    if(!useQuotes) {
        size_t pos = dataPos;
        while(pos < n && r < rows_) {
            size_t next = findNextDelimOrNL_AVX2(s, pos, n, opts_.delimiter);
            if(next >= n) break;

            if(s[next] == opts_.delimiter) {
                setCell(cellStart, next - cellStart, false);
                cellStart = next + 1;
            }
            else {
                setCell(cellStart, next - cellStart, c == cols_ - 1);
                finishRow();
                cellStart = next + 1;
            }
            pos = next + 1;
        }
    }

    notifyLoaded();
    return true;
}

// ==============================
// access
// ==============================
std::string_view CSVParser::valueView(size_t r, size_t c) const
{
    if(r >= rows_ || c >= cols_) return {};
    if(cols_ && r > SIZE_MAX / cols_) return {};

    size_t idx = r * cols_ + c;
    const auto& sp = cells_[idx];
    if(sp.len == 0 || !backing_) return {};
    return std::string_view(backing_->data + sp.off, sp.len);
}

const std::string& CSVParser::value(size_t r, size_t c) const
{
    static const std::string empty;

    if(r >= rows_ || c >= cols_)
        return empty;

    if(cols_ && r > SIZE_MAX / cols_)
        return empty;

    size_t k = r * cols_ + c;

    std::scoped_lock lk(cacheMutex_);

    if(cacheString_.empty())
        cacheString_.resize(rows_ * cols_);

    if(k >= cacheString_.size())
        return empty;

    if(cacheString_[k])
        return *cacheString_[k];

    cacheString_[k] = std::string(valueView(r, c));
    return *cacheString_[k];
}


// ==============================
// type inference
// ==============================
TextFileParser::CellKind CSVParser::cellKind(size_t r, size_t c) const
{
    auto v = valueView(r, c);
    if(v.empty()) return CK_Empty;

    if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan"))
        return CK_Empty;

    if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
        equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no"))
        return CK_Bool;

    Date d;
    if(parseDate(v, d)) return CK_Date;

    if(looksLikeNumber(v)) return CK_Number;

    return CK_String;
}

// ==============================
// header lookup
// ==============================
std::optional<size_t> CSVParser::columnIndex(const std::string& name) const
{
    auto it = std::lower_bound(headerIndex_.begin(), headerIndex_.end(), name,
        [](auto& a, const std::string& b) { return a.first < b; });

    if(it != headerIndex_.end() && it->first == name)
        return it->second;
    return std::nullopt;
}
