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

// ==============================
// ctor / reset
// ==============================
CSVParser::CSVParser(std::string filename, Options opts)
    : filename_(std::move(filename)), opts_(opts) {
}

void CSVParser::resetState() {
    rows_ = cols_ = 0;
    cells_.clear();
    colNames_.clear();
    headerIndex_.clear();
    root_ = DataNode("csv", filename_, 0);

    // IMPORTANT: do NOT pre-size per-cell caches here (too slow).
    cacheString_.clear();
    cacheInt_.clear();
    cacheDouble_.clear();
    cacheBool_.clear();
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
        if(c == '"') inq = !inq;          // FAST toggle (not RFC)
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
    // tail
    for(; i < n; ++i) {
        char c = s[i];
        if(c == delim || c == '\n') return i;
    }
    return n;
#else
    // scalar fallback
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
    // very fast heuristic: optional sign, digits, optional dot+digits, optional exp
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
// load(): fast + stable (rows/cols) + zero-copy
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

    // ---- decide quote mode for speed
    bool useQuotes = opts_.allowQuotes;
    if(useQuotes) {
        // if first line has no quotes and you know your data has none, you can disable globally.
        // We'll keep it enabled only if we actually see quotes in first line; this helps a lot.
        // (If later lines have quotes and first doesn't, this may mis-parse; enable allowQuotes=true to force.)
        if(firstLine.find('"') == std::string_view::npos) {
            useQuotes = false;
        }
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
        colNames_.resize(cols_);
        headerIndex_.clear();
        headerIndex_.reserve(cols_);
        for(size_t c = 0; c < cols_; ++c) {
            colNames_[c] = std::string(firstLine.substr(spans[c].first, spans[c].second));
            headerIndex_.emplace_back(colNames_[c], c);
        }
        std::sort(headerIndex_.begin(), headerIndex_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    else {
        colNames_.resize(cols_);
        for(size_t c = 0; c < cols_; ++c) colNames_[c] = "col" + std::to_string(c + 1);
    }

    // ---- start parsing data after first newline if header; otherwise include first line as data
    size_t pos = 0;
    if(opts_.hasHeader) {
        pos = (firstEnd < n && s[firstEnd] == '\n') ? (firstEnd + 1) : n;
    }
    else {
        pos = 0;
    }

    // ---- reserve (cheap heuristic, no extra pass)
    // Typical: average cell maybe 8-16 bytes; we can reserve in chunks.
    // Reserve at least few thousand cells.
    size_t approxCells = std::max<size_t>(4096, (n / 12));
    cells_.reserve(approxCells);

    // ---- one-pass parse of data with fixed cols_
    // We will fill exactly cols_ cells per row (pad missing, ignore extra).
    size_t rowCellCount = 0;
    size_t rowStartCellIndex = 0;

    auto padRowIfNeeded = [&]() {
        if(!opts_.padMissingCells) return;
        if(rowCellCount < cols_) {
            size_t missing = cols_ - rowCellCount;
            for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
            rowCellCount = cols_;
        }
        };

    // parsing loop
    size_t cellStart = pos;
    bool inQuotes = false;

    auto commitCell = [&](size_t cellEnd, std::string_view rowBase) {
        if(rowCellCount < cols_) {
            cells_.emplace_back(rowBase.data() + (cellStart - (size_t)(rowBase.data() - s)),
                cellEnd - cellStart);
        }
        // else ignore extras
        ++rowCellCount;
        };

    // We need rowBase as a view into [rowBegin,rowEnd) for substring creation.
    // For max speed, we compute spans using pointers, not substr on row view.
    size_t rowBegin = pos;

    auto finishRow = [&](size_t rowEnd) {
        // Trim CR
        size_t trimmedEnd = rowEnd;
        if(trimmedEnd > rowBegin && s[trimmedEnd - 1] == '\r') --trimmedEnd;

        // Commit last cell in this row if we haven't already overflown columns
        size_t cellEnd = trimmedEnd;
        if(rowCellCount < cols_) {
            cells_.emplace_back(s + cellStart, cellEnd - cellStart);
        }
        ++rowCellCount;

        // If we have less than cols_, pad empty
        if(opts_.padMissingCells) {
            if(rowCellCount < cols_) {
                size_t missing = cols_ - rowCellCount;
                for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
                rowCellCount = cols_;
            }
        }
        else {
            // If not padding and row has fewer, UI grid won't be stable; we still keep stable layout:
            // pad anyway to preserve r*cols indexing.
            if(rowCellCount < cols_) {
                size_t missing = cols_ - rowCellCount;
                for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
                rowCellCount = cols_;
            }
        }

        // If row had more than cols_, we ignored extras; but we must also ensure we wrote exactly cols_ cells.
        // With ignore-extras, we might have written cols_ then stopped adding; but rowCellCount still increments.
        // Normalize rowCellCount to cols_ for stable math.
        rowCellCount = cols_;

        ++rows_;
        rowCellCount = 0;
        rowStartCellIndex = cells_.size();
        };

    // If no-quotes, use AVX2 jump-to-next-special for speed.
    if(!useQuotes) {
        while(pos < n) {
            size_t next = findNextDelimOrNL_AVX2(s, pos, n, opts_.delimiter);
            if(next >= n) break;

            char c = s[next];
            if(c == opts_.delimiter) {
                if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, next - cellStart);
                ++rowCellCount;
                cellStart = next + 1;
                pos = next + 1;
                continue;
            }

            // newline
            if(c == '\n') {
                // last cell ends at next (exclusive), with CR trim handled in finishRow
                // commit last cell:
                if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, next - cellStart);
                ++rowCellCount;

                // pad/normalize row
                if(rowCellCount < cols_) {
                    size_t missing = cols_ - rowCellCount;
                    for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
                }
                // ignore extras already handled by not pushing beyond cols_
                rows_++;
                rowCellCount = 0;

                // next row
                pos = next + 1;
                rowBegin = pos;
                cellStart = pos;
                continue;
            }

            pos = next + 1;
        }

        // last line (no trailing \n)
        if(cellStart < n) {
            // commit last cell
            if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, n - cellStart);
            ++rowCellCount;
            if(rowCellCount < cols_) {
                size_t missing = cols_ - rowCellCount;
                for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
            }
            if(rowCellCount > 0) rows_++;
        }
    }
    else {
        // quotes mode (scalar, fast toggle)
        for(pos = cellStart; pos < n; ++pos) {
            char ch = s[pos];
            if(opts_.allowQuotes && ch == '"') {
                inQuotes = !inQuotes;
                continue;
            }

            if(!inQuotes && ch == opts_.delimiter) {
                if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, pos - cellStart);
                ++rowCellCount;
                cellStart = pos + 1;
                continue;
            }

            if(!inQuotes && ch == '\n') {
                size_t end = pos;
                if(end > rowBegin && s[end - 1] == '\r') --end;

                if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, end - cellStart);
                ++rowCellCount;

                if(rowCellCount < cols_) {
                    size_t missing = cols_ - rowCellCount;
                    for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
                }

                rows_++;
                rowCellCount = 0;
                rowBegin = pos + 1;
                cellStart = pos + 1;
                continue;
            }
        }

        // last line
        if(cellStart < n) {
            if(rowCellCount < cols_) cells_.emplace_back(s + cellStart, n - cellStart);
            ++rowCellCount;
            if(rowCellCount < cols_) {
                size_t missing = cols_ - rowCellCount;
                for(size_t k = 0; k < missing; ++k) cells_.emplace_back(std::string_view{});
            }
            if(rowCellCount > 0) rows_++;
        }
    }

    // If no header, include first row we parsed earlier (since pos=0). If header, we skipped it.
    // In non-header case we parsed from pos=0 and included header row as data; OK.
    // In header case we started after first line; OK.

    root_ = DataNode("csv", filename_, 0);
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
    return cells_[idx];
}

// Lazy string materialization (only if needed)
const std::string& CSVParser::value(size_t r, size_t c) const {
    static const std::string empty;
    if(r >= rows_ || c >= cols_) return empty;

    size_t k = r * cols_ + c;

    std::scoped_lock lk(cacheMutex_);

    // LAZY allocate only when needed (this is what you wanted)
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

    // null-ish
    if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan")) return CK_Empty;

    // bool-ish
    if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
        equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no") ||
        v == "0" || v == "1")
        return CK_Bool;

    // number-ish
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
