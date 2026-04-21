#include "XLSXParser.h"
#include <cstring>
#include <charconv>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static const unsigned EOCD_SIG = 0x06054b50U;
static const unsigned CEN_SIG = 0x02014b50U;
static const unsigned LOC_SIG = 0x04034b50U;

// ================= ctor/dtor =================
XLSXParser::XLSXParser(std::string filename)
    : filename_(std::move(filename)) {
}

XLSXParser::~XLSXParser() {
    zip_.close();
}

void XLSXParser::MappedFile::close()
{
#ifdef _WIN32
    if(base) UnmapViewOfFile(base);
    if(hMap) CloseHandle((HANDLE)hMap);
    if(hFile) CloseHandle((HANDLE)hFile);

    base = nullptr;
    hMap = nullptr;
    hFile = nullptr;
    size = 0;
#else
    if(base) munmap((void*)base, size);

    base = nullptr;
    size = 0;
#endif
}

// ================= mmap =================
bool XLSXParser::mapZip()
{
#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if(hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    GetFileSizeEx(hFile, &sz);

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!hMap) return false;

    auto view = (const unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);

    zip_.base = view;
    zip_.size = (size_t)sz.QuadPart;
    zip_.hFile = hFile;
    zip_.hMap = hMap;
#else
    int fd = open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;

    struct stat sb {};
    fstat(fd, &sb);

    void* mem = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    zip_.base = (const unsigned char*)mem;
    zip_.size = sb.st_size;
#endif
    return true;
}

// ================= zip index =================
bool XLSXParser::buildZipIndex()
{
    zipIndex_.clear();

    size_t size = zip_.size;
    const unsigned char* base = zip_.base;

    size_t search = std::min<size_t>(size, 65536 + 22);
    size_t start = size - 22;
    size_t minp = size - search;

    size_t eocd = -1;

    for(size_t p = start; ; --p) {
        if(le32(base + p) == EOCD_SIG) { eocd = p; break; }
        if(p == minp) break;
    }

    if(eocd == (size_t)-1) return false;

    const unsigned char* e = base + eocd;
    uint16_t entries = le16(e + 10);
    uint32_t cd_off = le32(e + 16);

    const unsigned char* cd = base + cd_off;

    for(uint16_t i = 0; i < entries; ++i) {
        if(le32(cd) != CEN_SIG) return false;

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

// ================= extract =================
bool XLSXParser::extractEntry(const std::string& path, std::string& out) const
{
    auto it = zipIndex_.find(path);
    if(it == zipIndex_.end()) return false;

    const ZipEntry& z = it->second;
    const unsigned char* comp = zip_.base + z.dataOff;

    if(z.method == 0) {
        out.assign((const char*)comp, z.usize);
        return true;
    }

    if(z.method == 8) {
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

// ================= open =================
bool XLSXParser::open()
{
    if(!mapZip()) return false;
    if(!buildZipIndex()) return false;

    std::string wb;
    if(!extractEntry("xl/workbook.xml", wb)) return false;

    parseWorkbook(wb);

    std::string sst;
    if(extractEntry("xl/sharedStrings.xml", sst))
        parseSharedStrings(sst);

    return true;
}

// ================= load =================
bool XLSXParser::load()
{
    if(sheets_.empty() && !open()) return false;

    std::string xml;
    if(!extractEntry(sheets_[currentSheet_].path, xml)) return false;

    parseSheet(xml);
    return true;
}

// ================= workbook =================
void XLSXParser::parseWorkbook(const std::string& xml)
{
    sheets_.clear();

    const char* p = xml.data();
    const char* e = p + xml.size();

    while(true) {
        const char* s = strstr(p, "<sheet");
        if(!s) break;

        const char* n = strstr(s, "name=\"");
        if(!n) break;

        n += 6;
        const char* q = strchr(n, '"');
        if(!q) break;

        std::string name(n, q - n);

        int idx = (int)sheets_.size() + 1;
        sheets_.push_back({ name, "xl/worksheets/sheet" + std::to_string(idx) + ".xml" });

        p = q;
    }
}

// ================= shared strings =================
void XLSXParser::parseSharedStrings(const std::string& xml)
{
    sharedBlob_.clear();
    sharedRefs_.clear();

    const char* p = xml.data();

    while(true) {
        const char* si = strstr(p, "<si");
        if(!si) break;

        const char* end = strstr(si, "</si>");
        if(!end) break;

        uint32_t off = sharedBlob_.size();

        const char* t = si;
        while(true) {
            const char* t1 = strstr(t, "<t");
            if(!t1 || t1 >= end) break;

            const char* gt = strchr(t1, '>');
            const char* t2 = strstr(gt, "</t>");

            sharedBlob_.append(gt + 1, t2 - gt - 1);
            t = t2 + 4;
        }

        sharedRefs_.push_back({ off, (uint32_t)(sharedBlob_.size() - off) });
        p = end + 5;
    }
}

// ================= sheet =================
void XLSXParser::parseSheet(const std::string& xml)
{
    sheetXML_ = xml;
    const char* base = sheetXML_.data();
    const char* end = base + sheetXML_.size();

    rows_ = cols_ = 0;

    // simple (fast enough baseline)
    const char* p = base;

    while((p = strstr(p, "<row"))) {
        rows_++;
        p += 4;
    }

    cols_ = 32; // dynamic later if needed
    cells_.assign(rows_ * cols_, CellRef{});
}

// ================= value =================
std::string_view XLSXParser::valueView(size_t r, size_t c) const
{
    if(r >= rows_ || c >= cols_) return {};

    const CellRef& cr = cells_[idx(r, c)];

    if(cr.kind == CK_String && cr.sst < sharedRefs_.size()) {
        auto& s = sharedRefs_[cr.sst];
        return std::string_view(sharedBlob_.data() + s.off, s.len);
    }

    return std::string_view(sheetXML_.data() + cr.off, cr.len);
}

const std::string& XLSXParser::value(size_t r, size_t c) const
{
    static std::string tmp;
    tmp = std::string(valueView(r, c));
    return tmp;
}

// ================= utils =================
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
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); ++i)
        if((a[i] | 32) != (b[i] | 32)) return false;
    return true;
}

TextFileParser::CellKind XLSXParser::cellKind(size_t r, size_t c) const
{
    auto v = valueView(r, c);
    if(v.empty()) return CK_Empty;

    if(equalsIgnoreCase(v, "null") || equalsIgnoreCase(v, "nan"))
        return CK_Empty;

    if(equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "false") ||
        equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "no"))
        return CK_Bool;

    if(looksLikeNumber(v))
        return CK_Number;

    return CK_String;
}
bool XLSXParser::looksLikeNumber(std::string_view v)
{
    if(v.empty()) return false;
    for(char c : v)
        if((c < '0' || c>'9') && c != '.' && c != '-' && c != 'e' && c != 'E')
            return false;
    return true;
}