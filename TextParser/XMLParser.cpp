#include "XMLParser.h"
#include <fstream>
#include <cstring>
#include <algorithm>

// ======= fast ctz32 =======
static inline uint32_t fast_ctz32(uint32_t x)
{
    if(x == 0) return 32u;

#if defined(_MSC_VER)
    unsigned long r = 0;
    _BitScanForward(&r, x);
    return (uint32_t)r;
#else
    return (uint32_t)__builtin_ctz(x);
#endif
}

// ===================================================
//  Konstruktor
// ===================================================

XMLParser::XMLParser(std::string path)
    : path_(std::move(path))
{
}

// ===================================================
//  setBuffer
// ===================================================

void XMLParser::setBuffer(const char* data, std::size_t size)
{
    buffer_.assign(data, data + size);
    data_ = buffer_.data();
    size_ = buffer_.size();
    end_ = data_ + size_;
}

// ===================================================
//  isAllWhitespace
// ===================================================

bool XMLParser::isAllWhitespace(const char* s, std::size_t len)
{
    for(std::size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if(c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return false;
    }
    return true;
}

// ===================================================
//  findSeq
// ===================================================

std::size_t XMLParser::findSeq(std::size_t from, std::string_view seq) const
{
    if(seq.empty() || from >= size_) return npos;

    const char* base = data_;
    const char* p = base + from;
    const char* end = base + size_;

    const char* pat = seq.data();
    std::size_t plen = seq.size();

    const char first = pat[0];

    while(p + plen <= end)
    {
        const void* hit = std::memchr(p, first, (size_t)(end - p));
        if(!hit) return npos;

        const char* h = (const char*)hit;
        if(h + plen > end) return npos;

        if(std::memcmp(h, pat, plen) == 0)
            return (size_t)(h - base);

        p = h + 1;
    }
    return npos;
}

// ===================================================
//  findNextLT — SIMD ubrzana pretraga '<'
// ===================================================

std::size_t XMLParser::findNextLT(std::size_t pos) const
{
    if(pos >= size_) return npos;

#if defined(__AVX2__) || defined(_MSC_VER)
    const __m256i ltvec = _mm256_set1_epi8('<');
    const char* base = data_;
    std::size_t i = pos;

    for(; i + 32 <= size_; i += 32)
    {
        __m256i block = _mm256_loadu_si256((__m256i const*)(base + i));
        __m256i cmp = _mm256_cmpeq_epi8(block, ltvec);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);

        if(mask)
            return i + fast_ctz32(mask);
    }

    for(; i < size_; ++i)
        if(base[i] == '<') return i;

    return npos;

#else
    for(std::size_t i = pos; i < size_; ++i)
        if(data_[i] == '<') return i;

    return npos;
#endif
}

// ===================================================
//  parseFast — ONE-PASS SIMD STREAM XML PARSER
// ===================================================

void XMLParser::parseFast()
{
    elementCount_ = 0;
    textNodeCount_ = 0;
    root_ = DataNode("#document", "", 0);

    if(size_ == 0) return;

    std::size_t pos = 0;
    if(size_ >= 3 &&
        (unsigned char)data_[0] == 0xEF &&
        (unsigned char)data_[1] == 0xBB &&
        (unsigned char)data_[2] == 0xBF)
    {
        pos = 3;
    }

    std::size_t lastTagEnd = pos;

    while(true)
    {
        std::size_t ltPos = findNextLT(pos);
        if(ltPos == npos)
        {
            if(lastTagEnd < size_)
            {
                std::size_t len = size_ - lastTagEnd;
                if(len && !isAllWhitespace(data_ + lastTagEnd, len))
                    ++textNodeCount_;
            }
            break;
        }

        if(ltPos > lastTagEnd)
        {
            std::size_t len = ltPos - lastTagEnd;
            if(len && !isAllWhitespace(data_ + lastTagEnd, len))
                ++textNodeCount_;
        }

        // find '>'
        std::size_t gtPos = ltPos + 1;
        while(gtPos < size_ && data_[gtPos] != '>') ++gtPos;
        if(gtPos >= size_) break;

        const char* t = data_ + ltPos;
        std::size_t tlen = gtPos - ltPos + 1;

        // ========== COMMENT ========
        if(tlen >= 4 && std::memcmp(t, "<!--", 4) == 0)
        {
            std::size_t endC = findSeq(ltPos, "-->");
            if(endC == npos)
            {
                pos = gtPos + 1;
                lastTagEnd = pos;
                continue;
            }
            pos = endC + 3;
            skipWS(pos);
            lastTagEnd = pos;
            continue;
        }

        // ========== CDATA ==========
        if(tlen >= 9 && std::memcmp(t, "<![CDATA[", 9) == 0)
        {
            std::size_t endCD = findSeq(ltPos + 9, "]]>");
            if(endCD == npos) break;

            std::size_t contentStart = ltPos + 9;
            std::size_t contentLen = endCD - contentStart;

            if(contentLen && !isAllWhitespace(data_ + contentStart, contentLen))
                ++textNodeCount_;

            pos = endCD + 3;
            skipWS(pos);
            lastTagEnd = pos;
            continue;
        }

        // ========== PROCESSING INSTRUCTION (<? ... ?>) ========
        if(t[1] == '?')
        {
            std::size_t endPI = findSeq(ltPos + 2, "?>");
            if(endPI == npos) break;

            pos = endPI + 2;
            skipWS(pos);
            lastTagEnd = pos;
            continue;
        }

        // ========== CLOSING TAG </tag> ==========
        if(t[1] == '/')
        {
            pos = gtPos + 1;
            skipWS(pos);
            lastTagEnd = pos;
            continue;
        }

        // ========== SELF CLOSING <tag .../> ==========
        if(tlen >= 3 && t[tlen - 2] == '/')
        {
            ++elementCount_;
            pos = gtPos + 1;
            skipWS(pos);
            lastTagEnd = pos;
            continue;
        }

        // ========== NORMAL OPENING <tag> ==========
        ++elementCount_;
        pos = gtPos + 1;
        skipWS(pos);
        lastTagEnd = pos;
    }
}

std::optional<XMLValue> XMLParser::getByIndex(size_t index) const
{
    if(!data_ || size_ == 0)
        return std::nullopt;

    const char* p = data_;
    const char* end = data_ + size_;

    size_t found = 0;

    while(p < end)
    {
        // find '<'
        const char* lt = (const char*)memchr(p, '<', end - p);
        if(!lt) break;

        // find '>'
        const char* gt = (const char*)memchr(lt, '>', end - lt);
        if(!gt) break;

        // extract tag name
        const char* nameStart = lt + 1;
        while(nameStart < gt && (*nameStart == '/' || *nameStart == '?' || *nameStart == ' '))
            nameStart++;

        const char* nameEnd = nameStart;
        while(nameEnd < gt &&
            *nameEnd != ' ' && *nameEnd != '/' && *nameEnd != '>')
            nameEnd++;

        if(nameStart == nameEnd) {
            p = gt + 1;
            continue;
        }

        std::string_view name(nameStart, nameEnd - nameStart);

        // extract text
        const char* textStart = gt + 1;
        const char* nextLT = (const char*)memchr(textStart, '<', end - textStart);
        if(!nextLT) nextLT = end;

        std::string_view text(textStart, nextLT - textStart);

        // trim whitespace
        size_t s = 0, e = text.size();
        while(s < e && isspace((unsigned char)text[s])) s++;
        while(e > s && isspace((unsigned char)text[e - 1])) e--;

        // Only count entries with non-empty text
        if(e > s)
        {
            if(found == index)
            {
                return XMLValue{ name, text.substr(s, e - s) };
            }
            found++;
        }

        p = nextLT;
    }

    return std::nullopt;
}

std::vector<XMLValue> XMLParser::getByName(std::string_view name) const
{
    std::vector<XMLValue> result;
    auto it = indexByName_.find(name);
    if(it == indexByName_.end()) return result;

    for(size_t idx : it->second)
    {
        auto [tag, text] = values_[idx];
        result.push_back(XMLValue{ tag, text });
    }
    return result;
}

std::optional<std::string_view> XMLParser::findFirst(std::string_view tag) const
{
    std::string open = "<" + std::string(tag);
    const char* p = data_;

    while(true)
    {
        const char* hit = (const char*)memchr(p, '<', end_ - p);
        if(!hit) return std::nullopt;

        // provjeri da li tag odgovara
        if(std::memcmp(hit + 1, tag.data(), tag.size()) == 0)
        {
            // skip to end of opening tag ">"
            const char* gt = (const char*)memchr(hit, '>', end_ - hit);
            if(!gt) return std::nullopt;

            const char* textStart = gt + 1;

            // find next '<'
            const char* nextLT = (const char*)memchr(textStart, '<', end_ - textStart);
            if(!nextLT) return std::nullopt;

            size_t len = nextLT - textStart;
            return std::string_view(textStart, len);
        }

        p = hit + 1;
    }
}


std::vector<std::string_view> XMLParser::findAll(std::string_view tag) const
{
    std::vector<std::string_view> out;
    const char* p = data_;

    while(true)
    {
        const char* hit = (const char*)memchr(p, '<', end_ - p);
        if(!hit) break;

        if(std::memcmp(hit + 1, tag.data(), tag.size()) == 0)
        {
            const char* gt = (const char*)memchr(hit, '>', end_ - hit);
            if(!gt) break;

            const char* textStart = gt + 1;
            const char* nextLT = (const char*)memchr(textStart, '<', end_ - textStart);
            if(!nextLT) break;

            out.emplace_back(textStart, nextLT - textStart);
            p = nextLT;
        }
        else {
            p = hit + 1;
        }
    }
    return out;
}

std::optional<std::string_view> XMLParser::getValueAtIndex(size_t index) const
{
    size_t counter = 0;
    const char* p = data_;

    while(true)
    {
        const char* lt = (const char*)memchr(p, '<', end_ - p);
        if(!lt) return std::nullopt;

        const char* gt = (const char*)memchr(lt, '>', end_ - lt);
        if(!gt) return std::nullopt;

        const char* textStart = gt + 1;
        const char* nextLT = (const char*)memchr(textStart, '<', end_ - textStart);
        if(!nextLT) return std::nullopt;

        if(nextLT > textStart)  // non-empty
        {
            if(counter == index)
            {
                return std::string_view(textStart, nextLT - textStart);
            }
            counter++;
        }
        p = nextLT;
    }
}


// ===================================================
//  load()
// ===================================================

bool XMLParser::load()
{
    if(buffer_.empty())
    {
        if(path_.empty()) return false;

        std::ifstream f(path_, std::ios::binary);
        if(!f) return false;

        f.seekg(0, std::ios::end);
        std::size_t len = (std::size_t)f.tellg();
        if(len == 0) return false;

        buffer_.resize(len);
        f.seekg(0, std::ios::beg);
        f.read(buffer_.data(), len);
    }

    data_ = buffer_.data();
    size_ = buffer_.size();
    end_ = data_ + size_;

    parseFast();
    notifyLoaded();
    return true;
}
