#define _CRT_SECURE_NO_WARNINGS
#include "CSVParser.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cassert>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

// ===================================================
//   ctor
// ===================================================
CSVParser::CSVParser(std::string filename, Options opts)
    : filename_(std::move(filename)), opts_(opts) {
}

// ===================================================
//   Buffer
// ===================================================
void CSVParser::Buffer::release() {
#ifdef _WIN32
    if (data && mmapped) UnmapViewOfFile(data);
    if (mapHandle) CloseHandle(mapHandle);
    if (fileHandle) CloseHandle(fileHandle);
    data = nullptr; size = 0; mmapped = false;
#else
    if (data && mmapped) {
        munmap((void*)data, size);
        if (fd >= 0) ::close(fd);
    }
    data = nullptr; size = 0; mmapped = false; fd = -1;
#endif
    owned.clear();
}

// ===================================================
//   File mapping / reading
// ===================================================
bool CSVParser::mapFile(Buffer& buf) const {
#ifdef _WIN32
    HANDLE fh = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(fh, &sz)) { CloseHandle(fh); return false; }

    if (sz.QuadPart == 0) {
        CloseHandle(fh);
        buf.data = nullptr; buf.size = 0;
        return true;
    }

    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) { CloseHandle(fh); return false; }

    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(mh); CloseHandle(fh); return false; }

    buf.data = static_cast<const char*>(view);
    buf.size = static_cast<size_t>(sz.QuadPart);
    buf.mmapped = true;
    buf.fileHandle = fh;
    buf.mapHandle = mh;
    return true;
#else
    int fd = ::open(filename_.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat sb {};
    if (fstat(fd, &sb) < 0) { ::close(fd); return false; }

    if (sb.st_size == 0) {
        ::close(fd);
        buf.data = nullptr; buf.size = 0;
        return true;
    }

    void* mem = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED) { ::close(fd); return false; }

    buf.data = static_cast<const char*>(mem);
    buf.size = sb.st_size;
    buf.mmapped = true;
    buf.fd = fd;
    return true;
#endif
}

bool CSVParser::readFileBuffered(Buffer& buf) const {
    std::FILE* f = std::fopen(filename_.c_str(), "rb");
    if (!f) return false;

    size_t size = 0;
    try { size = std::filesystem::file_size(filename_); }
    catch (...) { size = 0; }

    buf.owned.resize(size);
    size_t rd = std::fread(buf.owned.data(), 1, size, f);
    std::fclose(f);

    buf.data = buf.owned.data();
    buf.size = rd;
    buf.mmapped = false;
    return rd == size;
}

// ===================================================
//   Line detection
// ===================================================
void CSVParser::buildLineRanges(const Buffer& buf, std::vector<LineRange>& lines) const {
    lines.clear();
    if (!buf.data || buf.size == 0) return;

    const char* s = buf.data;
    size_t n = buf.size;
    size_t start = 0;

    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            lines.push_back({ start, end });
            start = i + 1;
        }
    }

    if (start < n) {
        size_t end = n;
        if (end > start && s[end - 1] == '\r') --end;
        lines.push_back({ start, end });
    }
}

// ===================================================
//   Row splitting
// ===================================================
void CSVParser::splitRowNoQuotes(std::string_view row, char delim,
    std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = row.data();
    size_t n = row.size();
    size_t start = 0;

    for (size_t i = 0; i < n; ++i) {
        if (p[i] == delim) {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

void CSVParser::splitRowWithQuotes(std::string_view row, char delim,
    std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = row.data();
    size_t n = row.size();
    size_t start = 0;
    bool inq = false;

    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c == '"') inq = !inq;
        else if (c == delim && !inq) {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

// ===================================================
//   Parallel parse
// ===================================================
void CSVParser::parallelSplitLines(const Buffer& buf, const std::vector<LineRange>& lines)
{
    size_t startRow = 0;
    std::vector<std::pair<size_t, size_t>> spans;

    if (opts_.hasHeader) {
        std::string_view hdr(buf.data + lines[0].begin,
            lines[0].end - lines[0].begin);
        if (opts_.allowQuotes)
            splitRowWithQuotes(hdr, opts_.delimiter, spans);
        else
            splitRowNoQuotes(hdr, opts_.delimiter, spans);

        cols_ = spans.size();
        colNames_.resize(cols_);
        headerIndex_.clear();
        for (size_t i = 0; i < cols_; ++i) {
            colNames_[i] = std::string(hdr.substr(spans[i].first, spans[i].second));
            headerIndex_.emplace_back(colNames_[i], i);
        }
        std::sort(headerIndex_.begin(), headerIndex_.end(),
            [](auto& a, auto& b) { return a.first < b.first; });

        startRow = 1;
    }
    else {
        std::string_view first(buf.data + lines[0].begin,
            lines[0].end - lines[0].begin);
        splitRowNoQuotes(first, opts_.delimiter, spans);
        cols_ = spans.size();
        colNames_.resize(cols_);
        for (size_t c = 0; c < cols_; ++c)
            colNames_[c] = "col" + std::to_string(c + 1);
    }

    if (opts_.maxCols > 0) cols_ = std::min<size_t>(cols_, (size_t)opts_.maxCols);

    rows_ = lines.size() - startRow;
    if (opts_.maxRows > 0) rows_ = std::min<size_t>(rows_, (size_t)opts_.maxRows);

    cells_.assign(rows_ * cols_, {});

    size_t threads = std::clamp<size_t>(opts_.threadHint ? opts_.threadHint : 1, 1, 64);
    threads = std::min<size_t>(threads, rows_);

    auto worker = [&](size_t r0, size_t r1) {
        std::vector<std::pair<size_t, size_t>> local;
        for (size_t r = r0; r < r1; ++r) {
            size_t idx = startRow + r;
            std::string_view row(buf.data + lines[idx].begin,
                lines[idx].end - lines[idx].begin);

            if (opts_.allowQuotes)
                splitRowWithQuotes(row, opts_.delimiter, local);
            else
                splitRowNoQuotes(row, opts_.delimiter, local);

            size_t n = std::min<size_t>(cols_, local.size());
            size_t base = r * cols_;
            for (size_t c = 0; c < n; ++c)
                cells_[base + c] = row.substr(local[c].first, local[c].second);
        }
        };

    std::vector<std::thread> th;
    th.reserve(threads);

    size_t chunk = rows_ / threads;
    size_t rem = rows_ % threads;
    size_t cur = 0;

    for (size_t t = 0; t < threads; ++t) {
        size_t add = chunk + (t < rem ? 1 : 0);
        th.emplace_back(worker, cur, cur + add);
        cur += add;
    }

    for (auto& t : th) t.join();
}

// ===================================================
//   load()
// ===================================================
bool CSVParser::load() {
    backing_ = std::make_unique<Buffer>();
    bool ok = opts_.useMMap && mapFile(*backing_);
    if (!ok) ok = readFileBuffered(*backing_);
    if (!ok) return false;

    std::vector<LineRange> lines;
    buildLineRanges(*backing_, lines);

    if (lines.empty()) {
        rows_ = cols_ = 0;
        cells_.clear();
        colNames_.clear();
        headerIndex_.clear();
        root_ = DataNode("csv", filename_, 0);
        notifyLoaded();
        return true;
    }

    parallelSplitLines(*backing_, lines);
    root_ = DataNode("csv", filename_, 0);
    notifyLoaded();
    return true;
}

// ===================================================
//   Access
// ===================================================
std::string_view CSVParser::valueView(size_t r, size_t c) const {
    if (r >= rows_ || c >= cols_) return {};
    return cells_[flatIndex(r, c)];
}

const std::string& CSVParser::value(size_t r, size_t c) const {
    static const std::string empty;
    if (r >= rows_ || c >= cols_) return empty;

    size_t k = flatIndex(r, c);
    {
        std::scoped_lock lk(cacheMutex_);
        if (cacheString_.empty()) cacheString_.resize(rows_ * cols_);
        if (cacheString_[k]) return *cacheString_[k];
    }
    std::string s(valueView(r, c));
    {
        std::scoped_lock lk(cacheMutex_);
        cacheString_[k] = std::move(s);
        return *cacheString_[k];
    }
}

// ===================================================
//   Header lookup
// ===================================================
std::optional<size_t> CSVParser::columnIndex(const std::string& name) const {
    auto it = std::lower_bound(headerIndex_.begin(), headerIndex_.end(), name,
        [](auto& a, auto& b) { return a.first < b; });
    if (it != headerIndex_.end() && it->first == name)
        return it->second;
    return std::nullopt;
}
