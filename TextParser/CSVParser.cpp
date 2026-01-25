#define _CRT_SECURE_NO_WARNINGS
#include "CSVParser.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <filesystem>

#ifdef _WIN32
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
// ULTRA-FAST CLEANUP (constant time):
// removes trailing '\r' and up to two trailing ';'
// so "TRUE;;\r" becomes "TRUE"
// ============================================================
static inline uint32_t trimCRandSemisLen(const char* base, uint32_t off, uint32_t len)
{
    if(len == 0) return 0;

    uint32_t n = len;

    // remove \r
    if(n && base[off + n - 1] == '\r') --n;

    // remove up to two ';'
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

void CSVParser::resetState() {
    rows_ = cols_ = 0;
    cells_.clear();
    headerIndex_.clear();
    backing_.reset();

    cacheString_.clear();
}

// ==============================
// Buffer release
// ==============================
void CSVParser::Buffer::release() {
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
bool CSVParser::mapFile(Buffer& buf) const {
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

bool CSVParser::readFileBuffered(Buffer& buf) const {
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
// split line helpers
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
// AVX2 scan: next delim or newline
// ==============================
size_t CSVParser::findNextDelimOrNL_AVX2(const char* s, size_t pos, size_t n, char delim) {
#if CSV_HAS_AVX2
    const __m256i vDelim = _mm256_set1_epi8((char)delim);
    const __m256i vNL = _mm256_set1_epi8('\n');

    size_t i = pos;
    for(; i + 32 <= n; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(s + i));
        __m256i eqD = _mm256_cmpeq_epi8(chunk, vDelim);
        __m256i eqN = _mm256_cmpeq_epi8(chunk, vNL);
        __m256i m = _mm256_or_si256(eqD, eqN);
        int mask = _mm256_movemask_epi8(m);
        if(mask) {
            unsigned long idx = 0;
#ifdef _WIN32
            _BitScanForward(&idx, (unsigned long)mask);
#else
            idx = (unsigned long)__builtin_ctz((unsigned)mask);
#endif
            return i + (size_t)idx;
        }
    }
    for(; i < n; ++i) {
        char c = s[i];
        if(c == delim || c == '\n') return i;
    }
    return n;
#else
    for(size_t i = pos; i < n; ++i) {
        char c = s[i];
        if(c == delim || c == '\n') return i;
    }
    return n;
#endif
}

// ==============================
// CellKind helpers
// ==============================
bool CSVParser::equalsIgnoreCase(std::string_view a, std::string_view b) {
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if(ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if(cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if(ca != cb) return false;
    }
    return true;
}

bool CSVParser::looksLikeNumber(std::string_view v) {
    if(v.empty()) return false;

    size_t i = 0;
    if(v[i] == '+' || v[i] == '-') ++i;
    bool anyDigit = false;

    for(; i < v.size(); ++i) {
        char c = v[i];
        if(c >= '0' && c <= '9') { anyDigit = true; continue; }
        break;
    }
    if(i < v.size() && v[i] == '.') {
        ++i;
        for(; i < v.size(); ++i) {
            char c = v[i];
            if(c >= '0' && c <= '9') { anyDigit = true; continue; }
            break;
        }
    }
    if(!anyDigit) return false;

    if(i < v.size() && (v[i] == 'e' || v[i] == 'E')) {
        ++i;
        if(i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
        bool expDigit = false;
        for(; i < v.size(); ++i) {
            char c = v[i];
            if(c >= '0' && c <= '9') { expDigit = true; continue; }
            return false;
        }
        return expDigit;
    }

    return i == v.size();
}

// ==============================
// load(): FAST 2-PASS + offsets/len
// ==============================
bool CSVParser::load() {
    resetState();

    backing_ = std::make_unique<Buffer>();
    bool ok = (opts_.useMMap && mapFile(*backing_));
    if(!ok) ok = readFileBuffered(*backing_);
    if(!ok) return false;

    const char* s = backing_->data;
    const size_t n = backing_->size;

    if(!s || n == 0) {
        notifyLoaded();
        return true;
    }

    // ---- find first line [0..firstEnd)
    size_t firstEnd = 0;
    while(firstEnd < n && s[firstEnd] != '\n') ++firstEnd;

    size_t firstLineEnd = firstEnd;
    if(firstLineEnd > 0 && s[firstLineEnd - 1] == '\r') --firstLineEnd;
    std::string_view firstLine(s, firstLineEnd);

    // ---- decide quote mode
    bool useQuotes = opts_.allowQuotes;
    if(useQuotes && firstLine.find('"') == std::string_view::npos) {
        useQuotes = false;
    }

    // ---- parse header/cols from first line
    std::vector<std::pair<size_t, size_t>> spans;
    spans.reserve(64);

    if(useQuotes) splitLineQuotesFast(firstLine, opts_.delimiter, spans);
    else          splitLineNoQuotes(firstLine, opts_.delimiter, spans);

    cols_ = spans.size();
    if(cols_ == 0) {
        notifyLoaded();
        return true;
    }

    if(opts_.hasHeader) {
        colNames_.clear();
        colNames_.resize(cols_);
        headerIndex_.clear();
        headerIndex_.reserve(cols_);

        for(size_t c = 0; c < cols_; ++c) {
            std::string name(firstLine.substr(spans[c].first, spans[c].second));
            colNames_[c] = name;
            headerIndex_.emplace_back(std::move(name), c);
        }

        std::sort(headerIndex_.begin(), headerIndex_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    else {
        colNames_.clear();
        colNames_.resize(cols_);
        for(size_t c = 0; c < cols_; ++c)
            colNames_[c] = "col" + std::to_string(c + 1);
    }


    // ---- start parsing data after first newline if header
    size_t dataPos = 0;
    if(opts_.hasHeader) {
        dataPos = (firstEnd < n && s[firstEnd] == '\n') ? (firstEnd + 1) : n;
    }
    else {
        dataPos = 0;
    }

    // ============================================================
    // PASS 1: count rows fast
    // ============================================================
    size_t rowCount = 0;
    if(dataPos < n) {
        for(size_t i = dataPos; i < n; ++i) {
            if(s[i] == '\n') ++rowCount;
        }
        if(n > dataPos && s[n - 1] != '\n') ++rowCount;
    }

    rows_ = rowCount;

    // allocate exact
    const size_t totalCells = rows_ * cols_;
    cells_.clear();
    cells_.resize(totalCells);

    // ============================================================
    // PASS 2: fill spans
    // ============================================================
    size_t r = 0;
    size_t c = 0;

    size_t cellStart = dataPos;
    bool inQuotes2 = false;

    auto setCell = [&](uint32_t off, uint32_t len, bool trimLastCol) {
        if(r >= rows_) return;
        if(c >= cols_) { ++c; return; } // ignore extra

        if(trimLastCol && opts_.trimLastColumnCRSemis) {
            len = trimCRandSemisLen(s, off, len);
        }

        cells_[r * cols_ + c] = CellSpan{ off, len };
        ++c;
        };

    auto padRow = [&]() {
        while(c < cols_) {
            cells_[r * cols_ + c] = CellSpan{ 0u, 0u };
            ++c;
        }
        };

    auto finishRow = [&]() {
        padRow();
        ++r;
        c = 0;
        };

    if(!useQuotes) {
        size_t pos = dataPos;

        while(pos < n && r < rows_) {
            size_t next = findNextDelimOrNL_AVX2(s, pos, n, opts_.delimiter);
            if(next >= n) break;

            char ch = s[next];

            if(ch == opts_.delimiter) {
                // normal cell, no trim
                uint32_t off = (uint32_t)cellStart;
                uint32_t len = (uint32_t)(next - cellStart);
                setCell(off, len, false);

                cellStart = next + 1;
                pos = next + 1;
                continue;
            }

            if(ch == '\n') {
                // last cell in row: trim only if last column
                uint32_t off = (uint32_t)cellStart;
                uint32_t len = (uint32_t)(next - cellStart);

                bool lastCol = (c == cols_ - 1);
                setCell(off, len, lastCol);

                finishRow();

                cellStart = next + 1;
                pos = next + 1;
                continue;
            }

            pos = next + 1;
        }

        // last line without \n
        if(r < rows_ && cellStart < n) {
            size_t end = n;
            if(end > cellStart && s[end - 1] == '\r') --end;

            uint32_t off = (uint32_t)cellStart;
            uint32_t len = (uint32_t)(end - cellStart);

            bool lastCol = (c == cols_ - 1);
            setCell(off, len, lastCol);

            finishRow();
        }
    }
    else {
        // quotes mode scalar
        for(size_t pos = dataPos; pos < n && r < rows_; ++pos) {
            char ch = s[pos];

            if(opts_.allowQuotes && ch == '"') {
                inQuotes2 = !inQuotes2;
                continue;
            }

            if(!inQuotes2 && ch == opts_.delimiter) {
                uint32_t off = (uint32_t)cellStart;
                uint32_t len = (uint32_t)(pos - cellStart);
                setCell(off, len, false);

                cellStart = pos + 1;
                continue;
            }

            if(!inQuotes2 && ch == '\n') {
                size_t endPos = pos;
                if(endPos > cellStart && s[endPos - 1] == '\r') --endPos;

                uint32_t off = (uint32_t)cellStart;
                uint32_t len = (uint32_t)(endPos - cellStart);

                bool lastCol = (c == cols_ - 1);
                setCell(off, len, lastCol);

                finishRow();

                cellStart = pos + 1;
                continue;
            }
        }

        // last line without \n
        if(r < rows_ && cellStart < n) {
            size_t end = n;
            if(end > cellStart && s[end - 1] == '\r') --end;

            uint32_t off = (uint32_t)cellStart;
            uint32_t len = (uint32_t)(end - cellStart);

            bool lastCol = (c == cols_ - 1);
            setCell(off, len, lastCol);

            finishRow();
        }
    }

    // clamp if needed
    if(r < rows_) rows_ = r;

    notifyLoaded();
    return true;
}

// ==============================
// ultra-fast access
// ==============================
std::string_view CSVParser::valueView(size_t r, size_t c) const {
    if(r >= rows_ || c >= cols_) return {};

    size_t idx = r * cols_ + c;
    if(idx >= cells_.size()) return {};

    const CellSpan sp = cells_[idx];
    if(sp.len == 0) return {};

    const char* base = backing_ ? backing_->data : nullptr;
    if(!base) return {};

    return std::string_view(base + sp.off, sp.len);
}

const std::string& CSVParser::value(size_t r, size_t c) const {
    static const std::string empty;
    if(r >= rows_ || c >= cols_) return empty;

    size_t k = r * cols_ + c;

    std::scoped_lock lk(cacheMutex_);

    if(cacheString_.empty()) {
        cacheString_.resize(rows_ * cols_);
    }

    if(k >= cacheString_.size()) return empty;
    if(cacheString_[k]) return *cacheString_[k];

    cacheString_[k] = std::string(valueView(r, c));
    return *cacheString_[k];
}

// CellKind heuristic (no allocations)
TextFileParser::CellKind CSVParser::cellKind(size_t r, size_t c) const {
    auto v = valueView(r, c);
    if(v.empty()) return CK_Empty;

    if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan")) return CK_Empty;

    if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
        equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no"))
        return CK_Bool;

    if(looksLikeNumber(v)) return CK_Number;

    return CK_String;
}

// Header lookup
std::optional<size_t> CSVParser::columnIndex(const std::string& name) const {
    auto it = std::lower_bound(headerIndex_.begin(), headerIndex_.end(), name,
        [](const auto& a, const std::string& b) { return a.first < b; });

    if(it != headerIndex_.end() && it->first == name)
        return it->second;
    return std::nullopt;
}
