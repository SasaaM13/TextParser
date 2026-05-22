#include "XMLParser.h"
#include <fstream>
#include <cstring>

static inline bool isWS(char c)
{
    return c == ' ' ||
        c == '\n' ||
        c == '\r' ||
        c == '\t';
}

inline const char* find3(
    const char* p,
    const char* end,
    char a,
    char b,
    char c)
{
    while(p + 2 < end)
    {
        if(p[0] == a &&
            p[1] == b &&
            p[2] == c)
        {
            return p;
        }
        ++p;
    }

    return nullptr;
}

// ===================================================
// XMLValue
// ===================================================

bool XMLValue::valid() const
{
    return owner_ != nullptr;
}

bool XMLValue::isElement() const
{
    return valid();
}

bool XMLValue::isText() const
{
    return valid() && owner_->nodesP_[idx_].text != nullptr;
}

std::string_view XMLValue::name() const
{
    if(!valid())
        return {};

    auto& n =
        owner_->nodesP_[idx_];

    if(!n.name)
        return {};

    return std::string_view(
        n.name,
        n.nameLen);
}
std::string_view XMLValue::text() const
{
    if(!valid())
        return {};

    auto& n =
        owner_->nodesP_[idx_];

    if(!n.text)
        return {};

    return std::string_view(
        n.text,
        n.textLen);
}
XMLValue XMLValue::firstChild() const
{
    uint32_t child =
        owner_->nodesP_[idx_].firstChild;

    if(child == UINT32_MAX)
        return {};

    return XMLValue(owner_, child);
}

XMLValue XMLValue::nextSibling() const
{
    uint32_t sibling =
        owner_->nodesP_[idx_].nextSibling;

    if(sibling == UINT32_MAX)
        return {};

    return XMLValue(owner_, sibling);
}

XMLValue XMLValue::child(std::string_view wanted) const
{
    if(!valid())
        return {};

    for(auto c = firstChild();
        c.valid();
        c = c.nextSibling())
    {
        if(c.name() == wanted)
            return c;
    }

    return {};
}

std::string_view XMLValue::childText(
    std::string_view wanted) const
{
    auto c = child(wanted);

    if(!c.valid())
        return {};

    auto txt =
        c.firstChild();

    if(txt.valid() &&
        txt.isText())
    {
        return txt.text();
    }

    return {};
}
std::optional<int>
XMLValue::toInt() const
{
    auto s = text();

    if(s.empty())
        return std::nullopt;

    int v = 0;

    auto [p, ec] =
        std::from_chars(
            s.data(),
            s.data() + s.size(),
            v);

    if(ec != std::errc())
        return std::nullopt;

    return v;
}
std::optional<double> XMLValue::toDouble() const
{
    auto s = text();

    if(s.empty())
        return std::nullopt;

    char* end = nullptr;

    double v =
        std::strtod(
            s.data(),
            &end);

    if(end == s.data())
        return std::nullopt;

    return v;
}
std::optional<bool> XMLValue::toBool() const
{
    auto s = text();

    if(s == "true" ||
        s == "1" ||
        s == "yes")
    {
        return true;
    }

    if(s == "false" ||
        s == "0" ||
        s == "no")
    {
        return false;
    }

    return std::nullopt;
}
std::optional<Date>
XMLValue::toDate() const
{
    Date d;

    if(parseDate(text(), d))
        return d;

    return std::nullopt;
}

std::vector<XMLValue>
XMLValue::children(
    std::string_view wanted) const
{
    std::vector<XMLValue> out;

    for(auto c = firstChild();
        c.valid();
        c = c.nextSibling())
    {
        if(c.name() == wanted)
            out.push_back(c);
    }

    return out;
}
// ===================================================
// Parser core
// ===================================================

XMLParser::XMLParser(std::string path, Options opt)
    : path_(std::move(path)), opt_(opt)
{
}

void XMLParser::reset()
{
    nodesP_.clear();
    elementCount_ = 0;
}

bool XMLParser::mapFile()
{
    namespace fs = std::filesystem;

    if(!fs::exists(path_))
        return false;

    size_ = (size_t)fs::file_size(path_);

#ifdef _WIN32
    hFile_ = CreateFileA(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if(hFile_ == INVALID_HANDLE_VALUE)
        return false;
    hMap_ = CreateFileMappingA(hFile_, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    if(!hMap_) return false;

    base_ = (char*)MapViewOfFile(hMap_, FILE_MAP_COPY, 0, 0, 0);
    if(!base_) return false;
#else
    fd_ = open(path_.c_str(), O_RDONLY);
    if(fd_ < 0) return false;

    void* mapped = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_, 0);
    if(mapped == MAP_FAILED) return false;

    base_ = (char*)mapped;
#endif

    data_ = base_;
    end_ = data_ + size_;
    return true;
}

void XMLParser::unmapFile()
{
#ifdef _WIN32
    if(base_) UnmapViewOfFile(base_);
    if(hMap_) CloseHandle(hMap_);
    if(hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
    if(base_) munmap((void*)base_, size_);
    if(fd_ >= 0) close(fd_);
#endif
}

//size_t XMLParser::findNextLT(size_t pos) const
//{
//    if(pos >= size_)
//        return size_t(-1);
//
//    const void* hit = std::memchr(data_ + pos, '<', size_ - pos);
//    if(!hit)
//        return size_t(-1);
//
//    return (size_t)((const char*)hit - data_);
//}

size_t XMLParser::findSeq(size_t from, std::string_view seq) const
{
    const char* p = data_ + from;
    const char* end = data_ + size_;

    while(p + seq.size() <= end)
    {
        if(std::memcmp(p, seq.data(), seq.size()) == 0)
            return (size_t)(p - data_);
        ++p;
    }
    return size_t(-1);
}

// ===================================================
// POINTER DOM (FAST)
// ===================================================
bool XMLParser::parseDOMPointer()
{
    nodesP_.clear();

    const size_t reserveCount =
        size_ / 12 + 1024;

    nodesP_.reserve(
        reserveCount);

    // =========================
    // DOCUMENT ROOT
    // =========================
    XMLNodeP doc{};

    doc.name =
        "#document";

    doc.nameLen =
        10;

    rootIndex_ =
        appendNodeP(doc);

    // =====================================
    // RAW STACK (FASTER THAN VECTOR)
    // =====================================
    uint32_t stack[256];

    uint32_t* sp =
        stack;

    *sp++ =
        rootIndex_;

    size_t pos = 0;

    const char* p =
        data_;

    const char* end =
        data_ + size_;

    while(p < end)
    {
        // =================================
        // FAST '<' SEARCH
        // =================================
        while(p < end &&
            *p != '<')
        {
            ++p;
        }

        if(p >= end)
            break;

        const size_t lt =
            (size_t)
            (p - data_);

        // =================================
        // TEXT BETWEEN TAGS
        // =================================
        if(lt > pos)
        {
            char* txt =
                data_ + pos;

            char* txtEnd =
                data_ + lt;

            // skip whitespace-only nodes
            bool hasRealText =
                false;

            for(char* s = txt;
                s < txtEnd;
                ++s)
            {
                if(!isWS(*s))
                {
                    hasRealText =
                        true;
                    break;
                }
            }

            if(hasRealText)
            {
                XMLNodeP node{};

                node.text =
                    txt;

                node.textLen =
                    (uint32_t)
                    (txtEnd - txt);

                node.parent =
                    *(sp - 1);

                uint32_t idx =
                    appendNodeP(
                        node);

                appendChildP(
                    *(sp - 1),
                    idx);
            }
        }

        pos = lt;

        if(pos + 1 >= size_)
            break;

        // =================================
        // COMMENT <!-- -->
        // =================================
        if(pos + 4 <= size_ &&
            data_[pos + 1] == '!' &&
            data_[pos + 2] == '-' &&
            data_[pos + 3] == '-')
        {
            const char* cEnd =
                find3(
                    data_ + pos + 4,
                    end,
                    '-',
                    '-',
                    '>');

            if(!cEnd)
                return false;

            pos =
                (size_t)
                (cEnd - data_) + 3;

            p =
                data_ + pos;

            continue;
        }

        // =================================
        // CDATA
        // <![CDATA[
        // =================================
        if(pos + 9 <= size_ &&
            data_[pos + 1] == '!' &&
            data_[pos + 2] == '[')
        {
            char* start =
                data_ + pos + 9;

            char* cdataEnd =
                (char*)
                find3(
                    start,
                    end,
                    ']',
                    ']',
                    '>');

            if(!cdataEnd)
                return false;

            if(cdataEnd > start)
            {
                XMLNodeP node{};

                node.text =
                    start;

                node.textLen =
                    (uint32_t)
                    (cdataEnd - start);

                node.parent =
                    *(sp - 1);

                uint32_t idx =
                    appendNodeP(
                        node);

                appendChildP(
                    *(sp - 1),
                    idx);
            }

            pos =
                (size_t)
                (cdataEnd - data_) + 3;

            p =
                data_ + pos;

            continue;
        }

        // =================================
        // XML DECLARATION
        // <?xml ?>
        // =================================
        if(data_[pos + 1]
            == '?')
        {
            const char* declEnd =
                strstr(
                    data_ + pos + 2,
                    "?>");

            if(!declEnd)
                return false;

            pos =
                (size_t)
                (declEnd - data_) + 2;

            p =
                data_ + pos;

            continue;
        }

        // =================================
        // <!DOCTYPE>
        // =================================
        if(data_[pos + 1]
            == '!')
        {
            const void* gt =
                memchr(
                    data_ + pos,
                    '>',
                    size_ - pos);

            if(!gt)
                return false;

            pos =
                (size_t)
                ((const char*)
                    gt - data_) + 1;

            p =
                data_ + pos;

            continue;
        }

        // =================================
        // CLOSING TAG
        // </tag>
        // =================================
        if(data_[pos + 1]
            == '/')
        {
            const void* gt =
                memchr(
                    data_ + pos,
                    '>',
                    size_ - pos);

            if(!gt)
                return false;

            pos =
                (size_t)
                ((const char*)
                    gt - data_) + 1;

            if(sp >
                stack + 1)
            {
                --sp;
            }

            p =
                data_ + pos;

            continue;
        }

        // =================================
        // OPENING TAG
        // =================================
        ++pos;

        while(pos < size_ &&
            isWS(data_[pos]))
        {
            ++pos;
        }

        const size_t
            tagStart =
            pos;

        while(pos < size_ &&
            data_[pos] != '>' &&
            data_[pos] != '/' &&
            !isWS(data_[pos]))
        {
            ++pos;
        }

        if(pos <= tagStart)
            return false;

        XMLNodeP node{};

        node.name =
            data_ +
            tagStart;

        node.nameLen =
            (uint32_t)
            (pos - tagStart);

        node.parent =
            *(sp - 1);

        uint32_t me =
            appendNodeP(
                node);

        appendChildP(
            *(sp - 1),
            me);

        ++elementCount_;

        bool selfClosing =
            false;

        while(pos < size_)
        {
            if(data_[pos]
                == '>')
            {
                ++pos;
                break;
            }

            if(data_[pos] == '/' &&
                pos + 1 < size_ &&
                data_[pos + 1] == '>')
            {
                selfClosing =
                    true;

                pos += 2;
                break;
            }

            ++pos;
        }

        if(!selfClosing)
        {
            *sp++ =
                me;
        }

        p =
            data_ + pos;
    }

    return true;
}
// ===================================================
// LOAD
// ===================================================

bool XMLParser::load()
{
    reset();

    if(!mapFile())
        return false;

    if(opt_.usePointerDom)
        return parseDOMPointer();

    return false;
}