#define _CRT_SECURE_NO_WARNINGS
#include "CSVParser.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <windows.h>

static inline size_t trimCRandSemisLen(const char* base, size_t off, size_t len)
{
    if(len == 0) 
        return 0;
    size_t n = len;

    if(n && base[off + n - 1] == '\r')
        --n;
    if(n && base[off + n - 1] == ';')
        --n;
    if(n && base[off + n - 1] == ';')
        --n;
    return n;
}

CSVParser::CSVParser(std::string filename, Options opts) : filename_(std::move(filename)), opts_(opts) 
{
}

void CSVParser::resetState()
{
    rows_ = cols_ = 0;
    cells_.clear();
    headerIndex_.clear();
    backing_.reset();
}

void CSVParser::Buffer::release()
{
    if(data && mmapped)
        UnmapViewOfFile(data);
    if(hMap)
        CloseHandle((HANDLE)hMap);
    if(hFile)
        CloseHandle((HANDLE)hFile);
    data = nullptr; 
    size = 0; 
    mmapped = false;
    hFile = nullptr; 
    hMap = nullptr;
    owned.clear();
}

bool CSVParser::mapFile(Buffer& buf) const
{
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER sz{};
    if(!GetFileSizeEx(hFile, &sz)) 
    { 
        CloseHandle(hFile); 
        return false;
    }
    if(sz.QuadPart == 0)
    {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!hMap)
    {
        CloseHandle(hFile);
        return false;
    }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if(!view)
    { 
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false; }

    buf.data = (const char*)view;
    buf.size = (size_t)sz.QuadPart;
    buf.mmapped = true;
    buf.hFile = hFile;
    buf.hMap = hMap;
    return true;
}

bool CSVParser::readFileBuffered(Buffer& buf) const
{
    std::FILE* f = std::fopen(filename_.c_str(), "rb");
    if(!f) 
        return false;
    size_t size = 0;
    try 
    { 
        size = (size_t)std::filesystem::file_size(filename_);
    }
    catch(...)
    {
        size = 0; 
    }

    buf.owned.resize(size);
    size_t rd = std::fread(buf.owned.data(), 1, size, f);
    std::fclose(f);

    if(rd == 0)
        return false;

    buf.data = buf.owned.data();
    buf.size = rd;
    buf.mmapped = false;
    return true;
}

void CSVParser::splitLineNoQuotes(std::string_view line, char delim, std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = line.data();
    size_t n = line.size();
    size_t start = 0;

    for(size_t i = 0; i < n; ++i) 
    {
        if(p[i] == delim) 
        {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

void CSVParser::splitLineQuotesFast(std::string_view line, char delim, std::vector<std::pair<size_t, size_t>>& spans)
{
    spans.clear();
    const char* p = line.data();
    size_t n = line.size();
    size_t start = 0;
    bool inq = false;

    for(size_t i = 0; i < n; ++i) 
    {
        char c = p[i];
        if(c == '"')
            inq = !inq;
        else if(c == delim && !inq)
        {
            spans.emplace_back(start, i - start);
            start = i + 1;
        }
    }
    spans.emplace_back(start, n - start);
}

size_t CSVParser::findNextDelim(const char* s, size_t pos, size_t n, char delim)
{
    for(size_t i = pos; i < n; ++i)
        if(s[i] == delim || s[i] == '\n')
            return i;
    return n;
}

bool CSVParser::equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if(a.size() != b.size()) 
        return false;
    for(size_t i = 0; i < a.size(); ++i)
    {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if(ca >= 'A' && ca <= 'Z') ca += 32;
        if(cb >= 'A' && cb <= 'Z') cb += 32;
        if(ca != cb)
            return false;
    }
    return true;
}

bool CSVParser::looksLikeNumber(std::string_view v)
{
    if(v.empty()) 
        return false;
    size_t i = 0;
    if(v[i] == '+' || v[i] == '-')
        ++i;

    bool any = false;
    for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i)
        any = true;

    if(i < v.size() && v[i] == '.')
    {
        ++i;
        for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i)
            any = true;
    }

    if(!any)
        return false;

    if(i < v.size() && (v[i] == 'e' || v[i] == 'E')) 
    {
        ++i;
        if(i < v.size() && (v[i] == '+' || v[i] == '-'))
            ++i;
        bool exp = false;
        for(; i < v.size() && std::isdigit((unsigned char)v[i]); ++i)
            exp = true;
        return exp && i == v.size();
    }

    return i == v.size();
}

static inline size_t count_newlines(const char* s, size_t n)
{
    size_t cnt = 0;
    for(size_t i = 0; i < n; ++i)
        cnt += (s[i] == '\n');
    return cnt;
}

bool CSVParser::load()
{
    resetState();
    backing_ = std::make_unique<Buffer>();
    bool ok = opts_.useMMap && mapFile(*backing_);
    if(!ok)
        ok = readFileBuffered(*backing_);
    if(!ok)
        return false;

    const char* s = backing_->data;
    const size_t n = backing_->size;
	if(!s || n == 0)
	{
		notifyLoaded();
		return true;
	}

    const size_t nlCount = count_newlines(s, n);
    const size_t estimatedRows = nlCount;
    size_t firstEnd = 0;
    while(firstEnd < n && s[firstEnd] != '\n')
        ++firstEnd;

    size_t firstLineEnd = firstEnd;
    if(firstLineEnd && s[firstLineEnd - 1] == '\r')
        --firstLineEnd;

    std::string_view firstLine(s, firstLineEnd);

    bool useQuotes = opts_.allowQuotes && firstLine.find('"') != std::string_view::npos;

    std::vector<std::pair<size_t, size_t>> spans;
    if(useQuotes) 
        splitLineQuotesFast(firstLine, opts_.delimiter, spans);
    else 
        splitLineNoQuotes(firstLine, opts_.delimiter, spans);

    cols_ = spans.size();
    if(cols_ == 0)
    { 
        notifyLoaded();
        return true;
    }

    if(opts_.hasHeader) 
    {
        colNames_.resize(cols_);
        headerIndex_.clear();
        headerIndex_.reserve(cols_);

        for(size_t c = 0; c < cols_; ++c) 
        {
            auto sv = firstLine.substr(spans[c].first, spans[c].second);
            colNames_[c].assign(sv.data(), sv.size());
            headerIndex_.emplace_back(std::string_view(colNames_[c]), c);
        }

        std::sort(headerIndex_.begin(), headerIndex_.end(),
            [](auto a, auto b) { return a.first < b.first; });
    }
    else
    {
        colNames_.resize(cols_);
        for(size_t c = 0; c < cols_; ++c)
            colNames_[c] = "col" + std::to_string(c + 1);
    }

    size_t dataPos = opts_.hasHeader && firstEnd < n ? firstEnd + 1 : 0;

    cells_.clear();
    cells_.resize(estimatedRows * cols_);
    size_t writeIdx = 0;
    size_t r = 0, c = 0;
    size_t cellStart = dataPos;
    if(!useQuotes) 
    {
        size_t pos = dataPos;

        while(pos < n)
        {
            size_t next = findNextDelim(s, pos, n, opts_.delimiter);
            if(next >= n)
                break;
            char ch = s[next];

            uint32_t off = (uint32_t)cellStart;
            uint32_t len = (uint32_t)(next - cellStart);

            if(opts_.trimLastColumnCRSemis && c == cols_ - 1)
                len = (uint32_t)trimCRandSemisLen(s, off, len);

            if(c < cols_)
                cells_[writeIdx + c] = CellSpan{ off, len };
            ++c;

            if(ch == '\n')
            {
                writeIdx += cols_;
                ++r;
                c = 0;
            }
            cellStart = next + 1;
            pos = next + 1;
        }

    }
    if(cellStart < n)
    {
        uint32_t off = (uint32_t)cellStart;
        uint32_t len = (uint32_t)(n - cellStart);

        if(opts_.trimLastColumnCRSemis && c == cols_ - 1)
            len = (uint32_t)trimCRandSemisLen(s, off, len);
        if(c < cols_)
            cells_[writeIdx + c] = CellSpan{ off, len };
        ++c;
        writeIdx += cols_;
        ++r;
        c = 0;
    }
    cells_.resize(r * cols_);
    rows_ = r;
    notifyLoaded();
    return true;
}

std::string_view CSVParser::valueView(size_t r, size_t c) const
{
    if(r >= rows_ || c >= cols_)
        return {};
    if(cols_ && r > SIZE_MAX / cols_)
        return {};

    size_t idx = r * cols_ + c;

    if(idx >= cells_.size())
        return {};

    const auto& sp = cells_[idx];
    if(sp.len == 0 || !backing_)
        return {};

    return std::string_view(backing_->data + sp.off, sp.len);
}

const std::string& CSVParser::value(size_t r, size_t c) const
{
    static const std::string empty;
    if(r >= rows_ || c >= cols_)
        return empty;
    if(cols_ && r > SIZE_MAX / cols_)
        return empty;

    ensureStringCacheSize();
    const size_t k = r * cols_ + c;
    if(k >= cacheString_.size())
        return empty;

    auto& slot = cacheString_[k];
    if(!slot.has_value())
        slot.emplace(valueView(r, c));

    return *slot;
}


TextFileParser::CellKind CSVParser::cellKind(size_t r, size_t c) const
{
    auto v = valueView(r, c);
    if(v.empty()) 
        return CK_Empty;

    const char ch0 = v.front();
    const char cl0 = (ch0 >= 'A' && ch0 <= 'Z') ? (char)(ch0 + 32) : ch0;

    if(cl0 == 'n')
    {
        if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan"))
            return CK_Empty;
        if(looksLikeNumber(v))
            return CK_Number;
        return CK_String;
    }

    if(cl0 == 't' || cl0 == 'f' || cl0 == 'y')
    {
        if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
            equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no"))
            return CK_Bool;
    }

    if((ch0 >= '0' && ch0 <= '9') || ch0 == '-' || ch0 == '+' || ch0 == '.')
    {
        Date d;
        if(parseDate(v, d))
            return CK_Date;
        if(looksLikeNumber(v))
            return CK_Number;
        return CK_String;
    }

    Date d;
    if(parseDate(v, d))
        return CK_Date;

    return CK_String;
}

std::optional<size_t> CSVParser::columnIndex(const std::string& name) const
{
    std::string_view key(name);
    auto it = std::lower_bound(headerIndex_.begin(), headerIndex_.end(), key,
        [](const auto& a, std::string_view b) { return a.first < b; });

    if(it != headerIndex_.end() && it->first == key)
        return it->second;
    return std::nullopt;
}
