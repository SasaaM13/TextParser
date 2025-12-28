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
//   Buffer (mmap ili fallback)
// ===================================================
void CSVParser::Buffer::release() {
#ifdef _WIN32
    if(data && mmapped) UnmapViewOfFile(data);
    if(mapHandle) CloseHandle(mapHandle);
    if(fileHandle) CloseHandle(fileHandle);
    data = nullptr; size = 0; mmapped = false;
#else
    if(data && mmapped) {
        munmap((void*)data, size);
        if(fd >= 0) ::close(fd);
    }
    data = nullptr; size = 0; mmapped = false; fd = -1;
#endif
    owned.clear();
}

bool CSVParser::mapFile(Buffer& buf) const {
#ifdef _WIN32
    HANDLE fh = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(fh == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fsize{};
    if(!GetFileSizeEx(fh, &fsize)) { CloseHandle(fh); return false; }
    if(fsize.QuadPart == 0) { CloseHandle(fh); buf.data = nullptr; buf.size = 0; return true; }

    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!mh) { CloseHandle(fh); return false; }

    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if(!view) { CloseHandle(mh); CloseHandle(fh); return false; }

    buf.data = static_cast<const char*>(view);
    buf.size = static_cast<size_t>(fsize.QuadPart);
    buf.mmapped = true;
    buf.fileHandle = fh;
    buf.mapHandle = mh;
    return true;
#else
    int fd = ::open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;
    struct stat sb {};
    if(fstat(fd, &sb) < 0) { ::close(fd); return false; }

    size_t size = static_cast<size_t>(sb.st_size);
    if(size == 0) { ::close(fd); buf.data = nullptr; buf.size = 0; return true; }

    void* mem = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mem == MAP_FAILED) { ::close(fd); return false; }

    buf.data = static_cast<const char*>(mem);
    buf.size = size;
    buf.mmapped = true;
    buf.fd = fd;
    return true;
#endif
}

bool CSVParser::readFileBuffered(Buffer& buf) const {
    std::FILE* f = std::fopen(filename_.c_str(), "rb");
    if(!f) return false;
    size_t size = 0;
    try { size = std::filesystem::file_size(filename_); }
    catch(...) { size = 0; }
    if(size == 0) { std::fclose(f); return true; }

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
    if(!buf.data || buf.size == 0) return;

    const char* s = buf.data;
    const size_t n = buf.size;

    size_t start = 0;
    for(size_t i = 0; i < n; ++i) {
        char c = s[i];
        if(c == '\n') {
            size_t end = i;
            if(end > start && s[end - 1] == '\r') --end;
            lines.push_back({ start, end });
            start = i + 1;
        }
    }

    if(start < n) {
        size_t end = n;
        if(end > start && s[end - 1] == '\r') --end;
        lines.push_back({ start, end });
    }
}

// ===================================================
//   Fast row splitting
// ===================================================
void CSVParser::splitRowFast(std::string_view row, char delim, bool allowQuotes,
    std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = row.data();
    size_t n = row.size();
    size_t start = 0;
    bool in_quotes = false;
    for(size_t i = 0; i < n; ++i) {
        char c = p[i];
        if(allowQuotes && c == '\"') in_quotes = !in_quotes;
        else if(c == delim && !in_quotes) {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

// ===================================================
//   Parallel split
// ===================================================
void CSVParser::parallelSplitLines(const Buffer& buf, const std::vector<LineRange>& lines)
{
    if(lines.empty()) { rows_ = 0; cols_ = 0; return; }

    size_t startRow = 0;
    std::vector<std::string> header;
    std::unordered_map<std::string, size_t> headerIx;
    std::vector<std::pair<size_t, size_t>> spans; spans.reserve(64);

    if(opts_.hasHeader) 
    {
        std::string_view hdr(buf.data + lines[0].begin, lines[0].end - lines[0].begin);
        splitRowFast(hdr, opts_.delimiter, opts_.allowQuotes, spans);
        header.reserve(spans.size());
        for(size_t i = 0; i < spans.size(); ++i) {
            auto [ofs, len] = spans[i];
            std::string_view v = hdr.substr(ofs, len);
            header.emplace_back(v.begin(), v.end());
            headerIx[header.back()] = i;
        }
        startRow = 1;
        cols_ = spans.size();
    }
    if(!opts_.hasHeader) {
        std::string_view first(buf.data + lines[0].begin, lines[0].end - lines[0].begin);
        splitRowFast(first, opts_.delimiter, opts_.allowQuotes, spans);
        cols_ = spans.size();
    }
    if(opts_.maxCols > 0) cols_ = std::min<size_t>(cols_, static_cast<size_t>(opts_.maxCols));

    rows_ = lines.size() - startRow;
    if(opts_.maxRows > 0) rows_ = std::min<size_t>(rows_, static_cast<size_t>(opts_.maxRows));

    if(rows_ == 0 || cols_ == 0) {
        cells_.clear(); colNames_.clear(); headerIndex_.clear(); return;
    }
    cells_.assign(rows_ * cols_, std::string_view{});
    headerIndex_.clear(); colNames_.clear();

    if(opts_.hasHeader) { colNames_ = header; headerIndex_ = headerIx; }
    else { colNames_.resize(cols_); for(size_t c = 0; c < cols_; ++c) colNames_[c] = "col" + std::to_string(c + 1); }

    size_t threads = std::clamp<size_t>(opts_.threadHint ? opts_.threadHint : 1, 1, 64);
    threads = std::min<size_t>(threads, rows_);

    std::vector<std::future<void>> futs;
    futs.reserve(threads);

    auto worker = [&, startRow](size_t rbegin, size_t rend) {
        std::vector<std::pair<size_t, size_t>> localSpans;
        localSpans.reserve(64);
        for(size_t rr = rbegin; rr < rend; ++rr) {
            size_t lineIdx = startRow + rr;
            std::string_view row(buf.data + lines[lineIdx].begin, lines[lineIdx].end - lines[lineIdx].begin);
            splitRowFast(row, opts_.delimiter, opts_.allowQuotes, localSpans);
            size_t ncols = std::min<size_t>(cols_, localSpans.size());
            size_t base = rr * cols_;
            for(size_t c = 0; c < ncols; ++c) {
                auto span = localSpans[c];
                cells_[base + c] = row.substr(span.first, span.second);
            }
        }
        };

    size_t chunk = rows_ / threads;
    size_t rem = rows_ % threads;
    size_t r = 0;
    for(size_t t = 0; t < threads; ++t) {
        size_t add = chunk + (t < rem ? 1 : 0);
        size_t rbegin = r;
        size_t rend = r + add;
        r = rend;
        futs.emplace_back(std::async(std::launch::async, worker, rbegin, rend));
    }
    for(auto& f : futs) f.get();
}

// ===================================================
//   load()
// ===================================================
bool CSVParser::load() {
    backing_ = std::make_unique<Buffer>();
    bool ok = false;
    if(opts_.useMMap) ok = mapFile(*backing_);
    if(!ok) ok = readFileBuffered(*backing_);
    if(!ok) return false;

    std::vector<LineRange> lines;
    buildLineRanges(*backing_, lines);

    if(lines.empty()) {
        rows_ = cols_ = 0;
        cells_.clear(); colNames_.clear(); headerIndex_.clear();
        root_ = DataNode("csv", filename_, 0);
        notifyLoaded();
        return true;
    }

    parallelSplitLines(*backing_, lines);
    root_ = DataNode("csv", filename_, 0);
    DataNode headerNode("header", "", 1);
    for(size_t c = 0; c < cols_; ++c)
        headerNode.children.emplace_back(colNames_[c], "", 2);
    root_.children.push_back(std::move(headerNode));
    notifyLoaded();
    return true;
}

std::string_view CSVParser::valueView(size_t r, size_t c) const {
    if(r >= rows_ || c >= cols_) return {};
    return cells_[flatIndex(r, c)];
}

const std::string& CSVParser::value(size_t r, size_t c) const {
    if(r >= rows_ || c >= cols_) {
        static const std::string empty;
        return empty;
    }
    size_t k = flatIndex(r, c);
    {
        std::scoped_lock lk(cacheMutex_);
        if(cacheString_.empty()) cacheString_.resize(rows_ * cols_);
        if(cacheString_[k].has_value()) return cacheString_[k].value();
    }
    std::string s(valueView(r, c));
    {
        std::scoped_lock lk(cacheMutex_);
        cacheString_[k] = std::move(s);
        return cacheString_[k].value();
    }
}
