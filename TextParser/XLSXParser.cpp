#include "XLSXParser.h"
#include <cstring>
#include <charconv>
#include <zlib.h>

#include <windows.h>
#include "DateParser.h"

inline Date excelDateToDate(int serial)
{
    int z = serial + 693594;
    int era =  (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe =  z - era * 146097;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;

    int y = (int)yoe + era * 400;

    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);

    return { y,(int)m, (int)d};
}

static const unsigned EOCD_SIG = 0x06054b50U;
static const unsigned CEN_SIG = 0x02014b50U;
static const unsigned LOC_SIG = 0x04034b50U;

XLSXParser::XLSXParser(std::string filename) : filename_(std::move(filename))
{
}

XLSXParser::~XLSXParser()
{
    zip_.close();
}

void XLSXParser::MappedFile::close()
{
    if(base)
        UnmapViewOfFile(base);
    if(hMap)
        CloseHandle((HANDLE)hMap);
    if(hFile)
        CloseHandle((HANDLE)hFile);

    base = nullptr;
    hMap = nullptr;
    hFile = nullptr;
    size = 0;
}

bool XLSXParser::mapZip()
{
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if(hFile == INVALID_HANDLE_VALUE) 
        return false;

    LARGE_INTEGER sz{};
    GetFileSizeEx(hFile, &sz);

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!hMap)
        return false;

    auto view = (const unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    zip_.base = view;
    zip_.size = (size_t)sz.QuadPart;
    zip_.hFile = hFile;
    zip_.hMap = hMap;
    return true;
}

bool XLSXParser::buildZipIndex()
{
    zipIndex_.clear();
    size_t size = zip_.size;
    const unsigned char* base = zip_.base;

    size_t search = std::min<size_t>(size, 65536 + 22);
    size_t start = size - 22;
    size_t minp = size - search;

    size_t eocd = -1;

    for(size_t p = start; ; --p)
    {
        if(le32(base + p) == EOCD_SIG) { eocd = p; break; }
        if(p == minp) break;
    }

    if(eocd == (size_t)-1)
        return false;

    const unsigned char* e = base + eocd;
    uint16_t entries = le16(e + 10);
    uint32_t cd_off = le32(e + 16);

    const unsigned char* cd = base + cd_off;

    for(uint16_t i = 0; i < entries; ++i)
    {
        if(le32(cd) != CEN_SIG)
            return false;

        uint16_t method = le16(cd + 10);
        uint32_t csize = le32(cd + 20);
        uint32_t usize = le32(cd + 24);

        uint16_t nlen = le16(cd + 28);
        uint16_t xlen = le16(cd + 30);
        uint16_t clen = le16(cd + 32);
        uint32_t lhofs = le32(cd + 42);

        std::string name((const char*)cd + 46, nlen);

        const unsigned char* loc = base + lhofs;
        uint16_t lh_n = le16(loc + 26);
        uint16_t lh_x = le16(loc + 28);

        size_t dataOff = lhofs + 30 + lh_n + lh_x;

        zipIndex_[name] = { method, csize, usize, (uint32_t)dataOff };

        cd += 46 + nlen + xlen + clen;
    }

    return true;
}

bool XLSXParser::extractEntry(const std::string& path, std::string& out) const
{
    auto it = zipIndex_.find(path);
    if(it == zipIndex_.end())
        return false;

    const ZipEntry& z = it->second;
    const unsigned char* comp = zip_.base + z.dataOff;

    if(z.method == 0)
    {
        out.assign((const char*)comp, z.usize);
        return true;
    }

    if(z.method == 8)
    {
        out.resize(z.usize);

        z_stream zs{};
        zs.next_in = (Bytef*)comp;
        zs.avail_in = z.csize;
        zs.next_out = (Bytef*)out.data();
        zs.avail_out = z.usize;

        inflateInit2(&zs, -MAX_WBITS);
        int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);

        return r == Z_STREAM_END;
    }

    return false;
}

bool XLSXParser::open()
{
    if(!mapZip())
        return false;

    if(!buildZipIndex())
        return false;
    std::string wb;

    if(!extractEntry("xl/workbook.xml",wb))
        return false;
    parseWorkbook(wb);
    std::string sst;

    if(extractEntry("xl/sharedStrings.xml",sst))
        parseSharedStrings(sst);

    std::string styles;

    if(extractEntry("xl/styles.xml",styles))
        parseStyles(styles);

    return true;
}

void XLSXParser::parseStyles(const std::string& xml)
{
    styleIsDate_.clear();
    std::unordered_map<int, bool> customFmtIsDate;

    const char* p = xml.data();
    const char* end = p + xml.size();

    while((p = strstr(p, "<numFmt")) && p < end)
    {
        const char* idPos = strstr(p, "numFmtId=\"");
        const char* codePos = strstr(p, "formatCode=\"");
        if(idPos && codePos)
        {
            int id = std::atoi(idPos + 11);
            codePos += 12;

            const char* q = strchr(codePos, '"');

            if(q)
            {
                std::string fmt(codePos, q - codePos);

                bool isDate = false;

                for(char& c : fmt)
                    c = (char)tolower((unsigned char)c);

                fmt.erase(std::remove(fmt.begin(), fmt.end(),'\\'), fmt.end());
                if(fmt.find('d') != std::string::npos ||
                    fmt.find('m') != std::string::npos ||
                    fmt.find('y') != std::string::npos)
                {
                    isDate = true;
                }
                customFmtIsDate[id] = isDate;
            }
        }

        ++p;
    }
    p = strstr(xml.data(),"<cellXfs");
    if(!p)
        return;
    p = strchr(p, '>');
    if(!p)
        return;
    ++p;
    const char* xfsEnd = strstr(p,"</cellXfs>");

    if(!xfsEnd)
        return;

    while((p = strstr(p, "<xf")) && p < xfsEnd)
    {
        bool isDate = false;
        const char* numFmt =strstr(p, "numFmtId=\"");

        if(numFmt)
        {
            int id = std::atoi(numFmt + 11);
            switch(id)
            {
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 45:
            case 46:
            case 47:
                isDate = true;
                break;

            default:
            {
                auto it = customFmtIsDate.find(id);

                if(it != customFmtIsDate.end())
                    isDate = it->second;
                break;
            }
            }
        }
        styleIsDate_.push_back(isDate);
        ++p;
    }
}

bool XLSXParser::isDateStyle(uint16_t style) const
{
    return style <  styleIsDate_.size() && styleIsDate_[style];
}

bool XLSXParser::load()
{
    if(sheets_.empty() && !open())
        return false;
    std::string xml;
    if(!extractEntry(sheets_[currentSheet_].path, xml))
        return false;
    parseSheet(xml);
    return true;
}

void XLSXParser::parseWorkbook(const std::string& xml)
{
    sheets_.clear();
    const char* p = xml.data();
    const char* e = p + xml.size();

    while(true) 
    {
        const char* s = strstr(p, "<sheet");
        if(!s)
            break;
        const char* n = strstr(s, "name=\"");
        if(!n) 
            break;
        n += 6;
        const char* q = strchr(n, '"');
        if(!q)
            break;
        std::string name(n, q - n);

        int idx = (int)sheets_.size() + 1;
        sheets_.push_back({ name, "xl/worksheets/sheet" + std::to_string(idx) + ".xml" });

        p = q;
    }
}

void XLSXParser::parseSharedStrings(const std::string& xml)
{
    sharedBlob_.clear();
    sharedRefs_.clear();

    const char* p = xml.data();

    while(true)
    {
        const char* si = strstr(p, "<si");
        if(!si)
            break;
        const char* end = strstr(si, "</si>");
        if(!end)
            break;

        uint32_t off = sharedBlob_.size();

        const char* t = si;
        while(true) 
        {
            const char* t1 = strstr(t, "<t");
            if(!t1 || t1 >= end)
                break;
            const char* gt = strchr(t1, '>');
            const char* t2 = strstr(gt, "</t>");
            sharedBlob_.append(gt + 1, t2 - gt - 1);
            t = t2 + 4;
        }

        sharedRefs_.push_back({ off, (uint32_t)(sharedBlob_.size() - off) });
        p = end + 5;
    }
}

void XLSXParser::parseSheet(const std::string& xml)
{
    sheetXML_ =xml;
    const char* base = sheetXML_.data();
    const char* end = base + sheetXML_.size();
    rows_ = 0;
    cols_ = 0;

    const char* p = base;
    while(p < end)
    {
        if(*p == '<')
        {
            if(p + 4 < end &&  p[1] == 'r' && p[2] == 'o' &&
                p[3] == 'w' && (p[4] == ' ' || p[4] == '>'))
            {
                ++rows_;
            }
            if(p + 4 < end && p[1] == 'c' && p[2] == ' ')
            {
                const char* s = p;

                while(s < end && *s != '>')
                {
                    if(*s == ' ' && s + 4 < end && s[1] == 'r' &&
                        s[2] == '=' && s[3] == '"')
                    {
                        s += 4;
                        int col = 0;

                        while(s < end && *s >= 'A' && *s <= 'Z')
                        {
                            col = col * 26 + (*s - 'A' + 1);
                            ++s;
                        }

                        if(col > (int)cols_)
                            cols_ = col;
                        
                        break;
                    }
                    ++s;
                }
            }
        }
        ++p;
    }
    if(rows_ == 0)
    {
        cells_.clear();
        return;
    }

    if(cols_ == 0)
        cols_ = 1;

    cells_.assign( rows_ * cols_, CellRef{});

    p = base;

    size_t currentRow = 0;

    while(p < end)
    {
        while(p < end && !(*p == '<' && p + 4 < end &&
                p[1] == 'r' && p[2] == 'o' &&  p[3] == 'w' &&
                (p[4] == ' ' ||  p[4] == '>')))
        {
            ++p;
        }

        if(p >= end)
            break;

        const char* rowEnd = strstr(p, "</row>");
        if(!rowEnd)
            break;
        const char* c = p;

        while(c < rowEnd)
        {
            while(c < rowEnd && !(*c == '<' && c + 2 < rowEnd &&
                    c[1] == 'c' && (c[2] == ' ' || c[2] == '>')))
            {
                ++c;
            }
            if(c >= rowEnd)
                break;

            const char* tagEnd = c;

            while(tagEnd < rowEnd && *tagEnd != '>')
                ++tagEnd;            
            if(tagEnd >= rowEnd)
                break;

            bool selfClosing =(*(tagEnd - 1) == '/');

            int col = 0;
            bool isShared = false;
            bool isBool = false;
            bool isInline = false;
            uint16_t style = 0;
            const char* s = c;

            while(s < tagEnd)
            {
                if(*s == ' ' && s + 4 < tagEnd &&
                    s[1] == 'r' && s[2] == '=' && s[3] == '"')
                {
                    s += 4;
                    while(s < tagEnd && *s >= 'A' && *s <= 'Z')
                    {
                        col = col * 26 + (*s - 'A' + 1);
                        ++s;
                    }
                    continue;
                }

                if(*s == ' ' && s + 4 < tagEnd && s[1] == 't' &&
                    s[2] == '=' && s[3] == '"')
                {
                    s += 4;
                    if(*s == 's')
                        isShared = true;
                    else if(*s == 'b')
                        isBool = true;
                    else if(std::strncmp(s, "inlineStr", 9) == 0)
                        isInline = true;
                    continue;
                }
                if(*s == ' ' && s + 4 < tagEnd &&
                    s[1] == 's' && s[2] == '=' && s[3] == '"')
                {
                    s += 4;
                    style = (uint16_t)std::atoi(s);
                    continue;
                }
                ++s;
            }
            if(col <= 0)
            {
                c = tagEnd + 1;
                continue;
            }

            size_t currentCol = (size_t) (col - 1);
            if(!selfClosing)
            {
                const char* cEnd = strstr(tagEnd, "</c>");
                if(!cEnd || cEnd > rowEnd)
                    break;
                const char* v1 = nullptr;
                const char* v2 = nullptr;
                if(isInline)
                {
                    const char* t = strstr(tagEnd, "<t>");
                    if(t && t < cEnd)
                    {
                        v1 = t + 3;
                        v2 = strstr(v1, "</t>");
                    }
                }
                else
                {
                    const char* v = strstr(tagEnd, "<v>");
                    if(v && v < cEnd)
                    {
                        v1 = v + 3;
                        v2 = strstr(v1, "</v>");
                    }
                }

                if(v1 && v2 && currentRow < rows_ && currentCol < cols_)
                {
                    CellRef& cr = cells_[idx(currentRow, currentCol)];
                    cr.style = style;
                    if(isShared)
                    {
                        cr.kind = CK_String;
                        cr.sst = (uint32_t)std::strtoul(v1, nullptr, 10);
                    }
                    else if(isBool)
                    {
                        cr.kind = CK_Bool;
                        cr.off = (uint32_t)(v1 - base);
                        cr.len = (uint32_t) (v2 - v1);
                    }
                    else
                    {
                        cr.off = (uint32_t) (v1 - base);
                        cr.len = (uint32_t) (v2 - v1);
                        cr.kind = looksLikeNumber(std::string_view(v1, v2 - v1)) ? CK_Number : CK_String;
                    }
                }

                c = cEnd + 4;
            }
            else 
                c =  tagEnd + 1;
        }

        ++currentRow;
        p = rowEnd + 6;
    }

    colNames_.clear();
    colNames_.reserve(cols_);

    if(rows_ > 0)
    {
        for(size_t c = 0; c < cols_; ++c)
        {
            auto v = valueView(0,c);
            if(v.empty())
                colNames_.push_back("Column" + std::to_string(c + 1));
            else
                colNames_.emplace_back(v);
        }
    }
}

std::string_view XLSXParser::valueView(size_t r, size_t c) const
{
    if(r >= rows_ || c >= cols_) 
        return {};

    const CellRef& cr = cells_[idx(r, c)];
    if(cr.kind == CK_String && cr.sst < sharedRefs_.size())
    {
        auto& s = sharedRefs_[cr.sst];
        return std::string_view(sharedBlob_.data() + s.off, s.len);
    }

    return std::string_view(sheetXML_.data() + cr.off, cr.len);
}

const std::string& XLSXParser::value(size_t r, size_t c) const
{
    static thread_local std::string tmp;
    auto v = valueView(r,c);
    tmp.assign(v.data(), v.size());
    return tmp;
}

uint32_t XLSXParser::le32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

uint16_t XLSXParser::le16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

bool XLSXParser::equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if(a.size() != b.size()) 
        return false;
    for(size_t i = 0; i < a.size(); ++i)
        if((a[i] | 32) != (b[i] | 32))
            return false;
    return true;
}

TextFileParser::CellKind XLSXParser::cellKind(size_t r,size_t c) const
{
    auto v = value(r, c);

    if(v.empty())
        return CK_Empty;

    if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan"))
        return CK_Empty;

    if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
        equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no"))
    {
        return CK_Bool;
    }

    Date date;
    if(parseDate(v, date))
        return CK_Date;
    if(looksLikeNumber(v))
        return CK_Number;
    return CK_String;
}
bool XLSXParser::looksLikeNumber(std::string_view v)
{
    if(v.empty())
        return false;

    bool digit = false;
    bool dot = false;
    bool exp = false;

    for(size_t i = 0; i < v.size(); ++i)
    {
        char c = v[i];
        if(c >= '0' && c <= '9')
        {
            digit = true;
            continue;
        }

        if(c == '.')
        {
            if(dot || exp) 
                return false;
            dot = true;
            continue;
        }

        if(c == 'e' || c == 'E')
        {
            if(exp || !digit) 
                return false;
            exp = true;
            digit = false;
            continue;
        }

        if(c == '-' || c == '+')
        {
            if(i == 0) 
                continue;
            if(v[i - 1] == 'e' || v[i - 1] == 'E')
                continue;
            return false;
        }

        return false;
    }

    return digit;
}