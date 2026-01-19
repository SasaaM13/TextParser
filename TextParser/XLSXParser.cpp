#include "XLSXParser.h"

#include <algorithm>
#include <thread>
#include <future>
#include <cstring>
#include <charconv>
#include <cctype>

#include <zlib.h>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

// ======== SIMD feature detekcija (opciono) ========
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#  include <immintrin.h>
#  define XLSX_AVX2 1
#else
#  define XLSX_AVX2 0
#endif

// ZIP konstante
static const unsigned EOCD_SIG = 0x06054b50U;
static const unsigned CEN_SIG  = 0x02014b50U;
static const unsigned LOC_SIG  = 0x04034b50U;

// === substring finder: mali tokeni, AVX2 ubrzanje za 2/3/4 ===
static inline const char* memmem_small(const char* hay, const char* hayEnd,
    const char* pat, size_t plen)
{
    if(plen == 0) return hay;
    if((size_t)(hayEnd - hay) < plen) return nullptr;

#if XLSX_AVX2
    if(plen == 2 || plen == 3 || plen == 4) {
        __m256i p0 = _mm256_set1_epi8(pat[0]);
        const char* p = hay;
        const size_t V = 32;

        while(p + V <= hayEnd) {
            __m256i v = _mm256_loadu_si256((const __m256i*)p);
            __m256i m = _mm256_cmpeq_epi8(v, p0);
            unsigned mask = (unsigned)_mm256_movemask_epi8(m);

            while(mask) {
#if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                unsigned i = (unsigned)idx;
#else
                unsigned i = __builtin_ctz(mask);
#endif
                const char* c = p + i;
                if(c + (ptrdiff_t)plen <= hayEnd && std::memcmp(c, pat, plen) == 0) return c;
                mask &= (mask - 1);
            }
            p += V;
        }

        for(; p + (ptrdiff_t)plen <= hayEnd; ++p) {
            if(*p == pat[0] && std::memcmp(p, pat, plen) == 0) return p;
        }
        return nullptr;
    }
#endif

    // scalar fallback
    for(const char* p = hay; p + (ptrdiff_t)plen <= hayEnd; ++p) {
        if(*p == pat[0] && std::memcmp(p, pat, plen) == 0) return p;
    }
    return nullptr;
}

static inline const char* find_token2(const char* s, const char* e, const char* t2) {
    return memmem_small(s, e, t2, 2);
}
static inline const char* find_token3(const char* s, const char* e, const char* t3) {
    return memmem_small(s, e, t3, 3);
}
static inline const char* find_token4(const char* s, const char* e, const char* t4) {
    return memmem_small(s, e, t4, 4);
}
static inline const char* find_token5(const char* s, const char* e, const char* t5) {
    return memmem_small(s, e, t5, 5);
}
static inline const char* find_token6(const char* s, const char* e, const char* t6) {
    return memmem_small(s, e, t6, 6);
}

// ================= little-endian helpers =================
uint32_t XLSXParser::le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t XLSXParser::le16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// ================= MappedFile close =================
void XLSXParser::MappedFile::close() {
#ifdef _WIN32
    if(base) UnmapViewOfFile(base);
    if(hMap) CloseHandle((HANDLE)hMap);
    if(hFile) CloseHandle((HANDLE)hFile);
    base = nullptr; size = 0; hMap = hFile = nullptr;
#else
    if(base) munmap((void*)base, size);
    base = nullptr; size = 0;
#endif
}

// ================= map ZIP =================
bool XLSXParser::mapZip(MappedFile& mf) const {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if(!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0) { CloseHandle(hFile); return false; }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if(!hMap) { CloseHandle(hFile); return false; }

    auto view = (const unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if(!view) { CloseHandle(hMap); CloseHandle(hFile); return false; }

    mf.base = view;
    mf.size = (size_t)sz.QuadPart;
    mf.hFile = hFile;
    mf.hMap  = hMap;
    return true;
#else
    int fd = ::open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;

    struct stat sb{};
    if(fstat(fd, &sb) < 0 || sb.st_size <= 0) { ::close(fd); return false; }

    void* mem = mmap(nullptr, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if(mem == MAP_FAILED) return false;

    mf.base = (const unsigned char*)mem;
    mf.size = (size_t)sb.st_size;
    return true;
#endif
}

// ================= ZIP extract entry =================
bool XLSXParser::extractEntry(const MappedFile& mf, const std::string& innerPath, std::string& out) const {
    if(!mf.base || mf.size < 22) return false;

    // EOCD is within last 64KB
    size_t search = std::min<size_t>(mf.size, 65536 + 22);
    size_t start  = mf.size - 22;
    size_t minp   = mf.size - search;

    size_t eocd = (size_t)-1;
    for(size_t p = start; p >= minp; --p) {
        if(*(const unsigned*)(mf.base + p) == EOCD_SIG) { eocd = p; break; }
        if(p == 0) break;
    }
    if(eocd == (size_t)-1) return false;

    const unsigned char* e = mf.base + eocd;
    uint16_t entries = le16(e + 10);
    uint32_t cd_off  = le32(e + 16);
    if(cd_off >= mf.size) return false;

    const unsigned char* cd = mf.base + cd_off;

    for(uint16_t i = 0; i < entries; ++i) {
        if((size_t)(cd - mf.base + 46) > mf.size) break;
        if(*(const unsigned*)cd != CEN_SIG) break;

        uint16_t method = le16(cd + 10);
        uint32_t csize  = le32(cd + 20);
        uint32_t usize  = le32(cd + 24);

        uint16_t nlen = le16(cd + 28);
        uint16_t xlen = le16(cd + 30);
        uint16_t clen = le16(cd + 32);
        uint32_t lhofs = le32(cd + 42);

        if((size_t)(cd - mf.base + 46 + nlen + xlen + clen) > mf.size) break;

        std::string name((const char*)cd + 46, nlen);

        if(name == innerPath) {
            if(lhofs + 30 > mf.size) return false;

            const unsigned char* loc = mf.base + lhofs;
            if(*(const unsigned*)loc != LOC_SIG) return false;

            uint16_t lh_n = le16(loc + 26);
            uint16_t lh_x = le16(loc + 28);

            size_t dataOff = (size_t)lhofs + 30 + lh_n + lh_x;
            if(dataOff + csize > mf.size) return false;

            const unsigned char* comp = mf.base + dataOff;

            if(method == 0) {
                // stored
                out.assign((const char*)comp, (size_t)usize);
                return true;
            } else if(method == 8) {
                // deflate
                out.resize((size_t)usize);

                z_stream zs{};
                zs.next_in   = const_cast<Bytef*>(comp);
                zs.avail_in  = (uInt)csize;
                zs.next_out  = (Bytef*)out.data();
                zs.avail_out = (uInt)usize;

                if(inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
                int ret = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);

                return ret == Z_STREAM_END;
            } else {
                return false;
            }
        }

        cd += 46 + nlen + xlen + clen;
    }

    return false;
}

// ================= ctor =================
XLSXParser::XLSXParser(std::string filename)
    : filename_(std::move(filename)) {}

// ================= open =================
bool XLSXParser::open() {
    sheets_.clear();
    sharedStrings_.clear();
    sheetXML_.clear();
    cells_.clear();
    rows_ = cols_ = 0;

    MappedFile mf{};
    if(!mapZip(mf)) return false;

    std::string workbookXML;
    if(!extractEntry(mf, "xl/workbook.xml", workbookXML)) { mf.close(); return false; }
    parseWorkbook(workbookXML);

    std::string sst;
    if(extractEntry(mf, "xl/sharedStrings.xml", sst)) {
        parseSharedStrings(sst);
    }

    mf.close();
    return !sheets_.empty();
}

// ================= load =================
bool XLSXParser::load() {
    if(sheets_.empty() && !open()) return false;
    if(currentSheet_ < 0 || currentSheet_ >= (int)sheets_.size()) currentSheet_ = 0;

    sheetXML_.clear();
    cells_.clear();
    rows_ = cols_ = 0;

    MappedFile mf{};
    if(!mapZip(mf)) return false;

    std::string xml;
    bool ok = extractEntry(mf, sheets_[currentSheet_].path, xml);
    mf.close();
    if(!ok) return false;

    parseSheetUltraFast(xml);

    root_ = DataNode("sheet", sheets_[currentSheet_].name, 0);
    DataNode meta("meta", "", 1);
    meta.children.emplace_back("rows", std::to_string(rowCount()), 2);
    meta.children.emplace_back("cols", std::to_string(colCount()), 2);
    root_.children.emplace_back(std::move(meta));

    notifyLoaded();
    return true;
}

// ================= sheet selection =================
bool XLSXParser::selectSheet(int index) {
    if(index < 0 || index >= (int)sheets_.size()) return false;
    currentSheet_ = index;
    return true;
}

int XLSXParser::sheetCount() const { return (int)sheets_.size(); }

std::string XLSXParser::sheetName(int index) const {
    if(index < 0 || index >= (int)sheets_.size()) return "";
    return sheets_[index].name;
}

std::vector<std::string> XLSXParser::allSheets() const {
    std::vector<std::string> out;
    out.reserve(sheets_.size());
    for(const auto& s : sheets_) out.push_back(s.name);
    return out;
}

// ================= valueView / value =================
std::string_view XLSXParser::valueView(size_t r, size_t c) const {
    if(r >= rows_ || c >= cols_) return {};
    const CellRef& cr = cells_[flatIndex(r, c)];
    if(cr.kind == 1) {
        if(cr.sst < sharedStrings_.size()) return std::string_view(sharedStrings_[cr.sst]);
        return {};
    }
    if(cr.len == 0) return {};
    return std::string_view(sheetXML_.data() + cr.off, cr.len);
}

const std::string& XLSXParser::value(size_t r, size_t c) const {
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

// ================= workbook.xml parsing =================
// NOTE: ovo je “fast/simple” – mapira sheet1.xml, sheet2.xml... po redu
// (za većinu fajlova radi; ako ima “r:id” mapiranja, možemo dodati kasnije)
void XLSXParser::parseWorkbook(const std::string& xml) {
    sheets_.clear();

    const char* p = xml.data();
    const char* e = p + xml.size();
    const char* cur = p;

    while(true) {
        const char* s = find_token5(cur, e, "<sheet");
        if(!s) break;

        const char* nameAttr = memmem_small(s, e, "name=\"", 6);
        if(!nameAttr) { cur = s + 6; continue; }
        nameAttr += 6;

        const char* q = (const char*)memchr(nameAttr, '"', (size_t)(e - nameAttr));
        if(!q) break;

        std::string name(nameAttr, q - nameAttr);

        int idx = (int)sheets_.size() + 1;
        sheets_.push_back({ name, "xl/worksheets/sheet" + std::to_string(idx) + ".xml" });

        cur = q + 1;
    }
}

// ================= sharedStrings.xml parsing =================
void XLSXParser::parseSharedStrings(const std::string& xml) {
    sharedStrings_.clear();
    sharedStrings_.reserve(2048);

    const char* p = xml.data();
    const char* e = p + xml.size();
    const char* cur = p;

    while(true) {
        const char* si = find_token3(cur, e, "<si");
        if(!si) break;

        const char* siEnd = memmem_small(si, e, "</si>", 5);
        if(!siEnd) break;

        std::string val;
        const char* tcur = si;

        while(true) {
            const char* t1 = find_token2(tcur, siEnd, "<t");
            if(!t1) break;

            const char* gt = (const char*)memchr(t1, '>', (size_t)(siEnd - t1));
            if(!gt) break;

            // ignore self-closing
            if(gt > t1 && *(gt - 1) == '/') { tcur = gt + 1; continue; }

            const char* t2 = memmem_small(gt, siEnd, "</t>", 4);
            if(!t2) break;

            val.append(gt + 1, (size_t)(t2 - (gt + 1)));
            tcur = t2 + 4;
        }

        sharedStrings_.push_back(std::move(val));
        cur = siEnd + 5;
    }
}

TextFileParser::CellKind XLSXParser::cellKind(size_t r, size_t c) const
{
    if (r >= rows_ || c >= cols_) return CellKind::CK_Empty;

    const CellRef& cr = cells_[r * cols_ + c];

    switch (cr.kind) {
    case 0:  return CellKind::CK_Number;
    case 1:  return CellKind::CK_String;
    case 2:  return CellKind::CK_Bool;
    case 3:  return CellKind::CK_Formula;
    case 4:  return CellKind::CK_Error;
    default: return CellKind::CK_Empty;
    }
}

// ================= ULTRA FAST sheet parser =================
//
// Strategija:
// - sheetXML_ = xml (backing string)
// - pronađi sve <row ...> ... </row> (spanovi)
// - iz prvih par redova procijeni max col (po "r=" ili po count <c>)
// - za svaki row, skeniraj <c ...> ... </c> i izvuci:
//     - colIndex iz r="AB12" (slova)
//     - t="s" => shared string
//     - <v>...</v> sadržaj (offset+len) ili sst index
//
void XLSXParser::parseSheetUltraFast(const std::string& xml)
{
    sheetXML_ = xml;
    const char* base = sheetXML_.data();
    const char* end = base + sheetXML_.size();

    rows_ = cols_ = 0;
    cells_.clear();

    // ===================================================
    // 1) Find <row>...</row> spans
    // ===================================================
    struct RowSpan { const char* b; const char* e; };
    std::vector<RowSpan> rows;
    rows.reserve(8192);

    const char* cur = base;
    while (true) {
        const char* r1 = memmem_small(cur, end, "<row", 4);
        if (!r1) break;
        const char* rEnd = memmem_small(r1, end, "</row>", 6);
        if (!rEnd) break;
        rows.push_back({ r1, rEnd + 6 });
        cur = rEnd + 6;
    }
    if (rows.empty()) return;

    rows_ = rows.size();

    // ===================================================
    // 2) Estimate column count (using r="AB12" if exists)
    // ===================================================
    size_t estCols = 0;
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 3); ++i) {
        const char* rb = rows[i].b;
        const char* re = rows[i].e;
        const char* p = rb;

        size_t localMax = 0;
        while (true) {
            const char* c1 = memmem_small(p, re, "<c", 2);
            if (!c1) break;

            const char* gt = (const char*)memchr(c1, '>', (size_t)(re - c1));
            if (!gt) break;

            size_t colIndex = SIZE_MAX;
            const char* rpos = memmem_small(c1, gt, " r=\"", 4);
            if (rpos) {
                rpos += 4;
                unsigned colNum = 0;
                const char* rp = rpos;
                while (rp < gt && isalpha_fast(*rp)) {
                    colNum = colNum * 26 + (toupper_fast(*rp) - 'A' + 1);
                    ++rp;
                }
                if (colNum > 0) colIndex = (size_t)colNum - 1;
            }

            if (colIndex != SIZE_MAX)
                localMax = std::max(localMax, colIndex + 1);
            else
                localMax = std::max(localMax, (size_t)1);

            p = gt + 1;
        }
        estCols = std::max(estCols, localMax);
    }

    if (estCols == 0) estCols = 8;
    cols_ = estCols;

    cells_.assign(rows_ * cols_, CellRef{});

    // ===================================================
    // 3) Parse rows & cells (single-thread, ultra fast)
    // ===================================================
    for (size_t r = 0; r < rows_; ++r) {
        const char* rowStart = rows[r].b;
        const char* rowEnd = rows[r].e;

        const char* ccur = rowStart;
        while (true) {
            const char* c1 = memmem_small(ccur, rowEnd, "<c", 2);
            if (!c1) break;

            const char* gt = (const char*)memchr(c1, '>', (size_t)(rowEnd - c1));
            if (!gt) break;

            bool selfClose = (gt > c1 && *(gt - 1) == '/');

            // ---------------------------------------------------
            // Parse cell type t="x"
            // ---------------------------------------------------
            char cellType = 0; // 's','n','b','e'
            const char* tpos = memmem_small(c1, gt, " t=\"", 4);
            if (tpos && tpos + 5 <= gt) {
                cellType = *(tpos + 4);
            }

            bool hasFormula = false;
            if (memmem_small(gt, rowEnd, "<f>", 3)) {
                hasFormula = true;
            }

            uint8_t kind = CK_String;
            if (hasFormula)                kind = CK_Formula;
            else if (cellType == 's')      kind = CK_String;
            else if (cellType == 'n' ||
                cellType == 0)       kind = CK_Number;
            else if (cellType == 'b')      kind = CK_Bool;
            else if (cellType == 'e')      kind = CK_Error;

            // ---------------------------------------------------
            // r="AB12" → column index
            // ---------------------------------------------------
            size_t colIndex = SIZE_MAX;
            const char* rpos = memmem_small(c1, gt, " r=\"", 4);
            if (rpos) {
                rpos += 4;
                unsigned colNum = 0;
                const char* rp = rpos;
                while (rp < gt && isalpha_fast(*rp)) {
                    colNum = colNum * 26 + (toupper_fast(*rp) - 'A' + 1);
                    ++rp;
                }
                if (colNum > 0) colIndex = (size_t)colNum - 1;
            }

            if (selfClose) {
                if (colIndex != SIZE_MAX && colIndex < cols_) {
                    cells_[r * cols_ + colIndex].kind = CK_Empty;
                }
                ccur = gt + 1;
                continue;
            }

            // ---------------------------------------------------
            // <v>...</v>
            // ---------------------------------------------------
            const char* v1 = memmem_small(gt, rowEnd, "<v>", 3);
            if (!v1) { ccur = gt + 1; continue; }

            const char* v2 = memmem_small(v1, rowEnd, "</v>", 4);
            if (!v2) { ccur = gt + 1; continue; }

            const char* valBeg = v1 + 3;
            const char* valEnd = v2;
            uint32_t off = (uint32_t)(valBeg - base);
            uint32_t len = (uint32_t)(valEnd - valBeg);

            if (colIndex != SIZE_MAX && colIndex < cols_) {
                CellRef& cr = cells_[r * cols_ + colIndex];
                cr.kind = kind;

                if (kind == CK_String && cellType == 's') {
                    unsigned idx = 0;
                    if (len) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
                        auto rr = std::from_chars(valBeg, valEnd, idx);
                        if (rr.ec != std::errc()) idx = 0;
#else
                        idx = (unsigned)std::strtoul(
                            std::string(valBeg, valEnd).c_str(), nullptr, 10);
#endif
                    }
                    cr.sst = idx;
                    cr.off = cr.len = 0;
                }
                else {
                    cr.off = off;
                    cr.len = len;
                    cr.sst = 0;
                }
            }

            ccur = v2 + 4;
        }
    }

    // ===================================================
    // Reset lazy cache
    // ===================================================
    {
        std::scoped_lock lk(cacheMutex_);
        cacheString_.clear();
    }
}

