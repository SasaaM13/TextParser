#include "XLSXParser.h"
#include <zlib.h>
#include <algorithm>
#include <thread>
#include <future>
#include <cstring>
#include <cctype>
#include <charconv>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

// ======== SIMD feature detekcija ========
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#  include <immintrin.h>
#  define XLSX_AVX2 1
#else
#  define XLSX_AVX2 0
#endif

// ================= ZIP mmap =================
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
    mf.base = view; mf.size = (size_t)sz.QuadPart; mf.hFile = hFile; mf.hMap = hMap;
    return true;
#else
    int fd = ::open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;
    struct stat sb {};
    if(fstat(fd, &sb) < 0 || sb.st_size <= 0) { ::close(fd); return false; }
    void* mem = mmap(nullptr, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mem == MAP_FAILED) { ::close(fd); return false; }
    mf.base = (const unsigned char*)mem; mf.size = (size_t)sb.st_size;
    ::close(fd);
    return true;
#endif
}

uint32_t XLSXParser::le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t XLSXParser::le16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// ZIP konstante
static const unsigned EOCD_SIG = 0x06054b50U;
static const unsigned CEN_SIG = 0x02014b50U;
static const unsigned LOC_SIG = 0x04034b50U;

// === substring finder: mali tokeni (2–6 znakova), AVX2 ubrzanje ===
static inline const char* memmem_small(const char* hay, const char* hayEnd,
    const char* pat, size_t plen) {
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
                unsigned i = __builtin_ctz(mask);
                const char* c = p + i;
                if(c + (ptrdiff_t)plen <= hayEnd && std::memcmp(c, pat, plen) == 0) return c;
                mask &= (mask - 1);
            }
            p += V;
        }
        // rep
        for(; p + (ptrdiff_t)plen <= hayEnd; ++p) {
            if(*p == pat[0] && std::memcmp(p, pat, plen) == 0) return p;
        }
        return nullptr;
    }
#endif
    // scalar fallback
    const char* p = hay;
    for(; p + (ptrdiff_t)plen <= hayEnd; ++p) {
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

// === ZIP ekstrakcija (store/deflate) ===
bool XLSXParser::extractEntry(const MappedFile& mf, const std::string& innerPath, std::string& out) const {
    if(!mf.base || mf.size < 22) return false;

    // nađi EOCD (64KB od kraja)
    size_t search = std::min<size_t>(mf.size, 65536 + 22);
    size_t start = mf.size - 22;
    size_t minp = mf.size - search;

    size_t eocd = (size_t)-1;
    for(size_t p = start; p >= minp; --p) {
        if(*(const unsigned*)(mf.base + p) == EOCD_SIG) { eocd = p; break; }
        if(p == 0) break;
    }
    if(eocd == (size_t)-1) return false;

    const unsigned char* e = mf.base + eocd;
    uint16_t entries = le16(e + 10);
    uint32_t cd_off = le32(e + 16);
    if(cd_off >= mf.size) return false;

    const unsigned char* cd = mf.base + cd_off;
    for(uint16_t i = 0; i < entries; ++i) {
        if((size_t)(cd - mf.base + 46) > mf.size) break;
        if(*(const unsigned*)cd != CEN_SIG) break;

        uint16_t nlen = le16(cd + 28);
        uint16_t xlen = le16(cd + 30);
        uint16_t clen = le16(cd + 32);
        uint16_t method = le16(cd + 10);
        uint32_t csize = le32(cd + 20);
        uint32_t usize = le32(cd + 24);
        uint32_t lhofs = le32(cd + 42);

        if((size_t)(cd - mf.base + 46 + nlen + xlen + clen) > mf.size) break;
        std::string name((const char*)cd + 46, nlen);

        if(name == innerPath) {
            if(lhofs + 30 > mf.size) return false;
            const unsigned char* loc = mf.base + lhofs;
            if(*(const unsigned*)loc != LOC_SIG) return false;

            uint16_t lh_n = le16(loc + 26);
            uint16_t lh_x = le16(loc + 28);
            size_t dataOff = lhofs + 30 + lh_n + lh_x;
            if(dataOff + csize > mf.size) return false;

            const unsigned char* comp = mf.base + dataOff;

            if(method == 0) { // stored
                out.assign((const char*)comp, usize);
                return true;
            }
            else if(method == 8) { // deflate
                out.resize(usize);
                z_stream zs{};
                zs.next_in = const_cast<Bytef*>(comp);
                zs.avail_in = (uInt)csize;
                zs.next_out = (Bytef*)out.data();
                zs.avail_out = (uInt)usize;
                if(inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
                int ret = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                if(ret != Z_STREAM_END) return false;
                return true;
            }
            else {
                return false; // unsupported method
            }
        }
        cd += 46 + nlen + xlen + clen;
    }
    return false;
}

// ================= Konstruktor / open/load =================
XLSXParser::XLSXParser(std::string filename) : filename_(std::move(filename)) {}

bool XLSXParser::open() {
    sheets_.clear();
    data_.clear();
    sharedStrings_.clear();

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

bool XLSXParser::load() {
    if(sheets_.empty() && !open()) return false;
    if(currentSheet_ < 0 || currentSheet_ >= (int)sheets_.size()) currentSheet_ = 0;

    data_.clear();
    MappedFile mf{};
    if(!mapZip(mf)) return false;

    std::string xml;
    bool ok = extractEntry(mf, sheets_[currentSheet_].path, xml);
    mf.close();
    if(!ok) return false;

    parseSheetFastParallel(xml);

    // root (light)
    root_ = DataNode("sheet", sheets_[currentSheet_].name, 0);
    DataNode meta("meta", "", 1);
    meta.children.emplace_back("rows", std::to_string(rowCount()), 2);
    meta.children.emplace_back("cols", std::to_string(colCount()), 2);
    root_.children.emplace_back(std::move(meta));

    notifyLoaded();
    return true;
}

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
    for(auto& s : sheets_) out.push_back(s.name);
    return out;
}

size_t XLSXParser::rowCount() const { return data_.size(); }
size_t XLSXParser::colCount() const { return data_.empty() ? 0 : data_[0].size(); }
const std::string& XLSXParser::value(size_t r, size_t c) const {
    static const std::string empty;
    if(r >= data_.size() || c >= data_[r].size()) return empty;
    return data_[r][c];
}

// ================= workbook.xml (lista sheetova) =================
void XLSXParser::parseWorkbook(const std::string& xml) {
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

// ================= sharedStrings.xml =================
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

// ================= sheetN.xml — paralelno, AVX2 skeniranje tagova =================
void XLSXParser::parseSheetFastParallel(const std::string& xml) {
    data_.clear();

    const char* base = xml.data();
    const char* end = base + xml.size();

    // 1) Pronađi sve <row ...> .. </row>
    struct RowSpan { const char* b; const char* e; };
    std::vector<RowSpan> rows;
    rows.reserve(8192);

    const char* cur = base;
    while(true) {
        const char* r1 = memmem_small(cur, end, "<row", 4);
        if(!r1) break;
        const char* rEnd = memmem_small(r1, end, "</row>", 6);
        if(!rEnd) break;
        rows.push_back({ r1, rEnd + 6 });
        cur = rEnd + 6;
    }
    if(rows.empty()) return;

    // dimenzije (grubo): pogledaj prva 2–3 reda
    size_t estCols = 0;
    for(size_t i = 0; i < std::min<size_t>(rows.size(), 3); ++i) {
        size_t cnt = 0;
        const char* x = rows[i].b;
        while((x = memmem_small(x, rows[i].e, "<c", 2)) && x < rows[i].e) { ++cnt; x += 2; }
        estCols = std::max(estCols, cnt);
    }

    data_.resize(rows.size());
    for(auto& r : data_) r.reserve(estCols ? estCols : 8);

    // 2) Parallel parse (N = min(hw, rows))
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::min<unsigned>(hw ? hw : 4u, (unsigned)rows.size());

    auto worker = [&](size_t i0, size_t i1) {
        for(size_t i = i0; i < i1; ++i) {
            const char* rowStart = rows[i].b;
            const char* rowEnd = rows[i].e;
            std::vector<std::string> row;

            const char* ccur = rowStart;
            while(true) {
                const char* c1 = memmem_small(ccur, rowEnd, "<c", 2);
                if(!c1 || c1 >= rowEnd) break;
                const char* gt = (const char*)memchr(c1, '>', (size_t)(rowEnd - c1));
                if(!gt || gt >= rowEnd) break;

                bool selfClose = (gt > c1 && *(gt - 1) == '/');

                // t="s" ?
                bool isShared = false;
                const char* tpos = memmem_small(c1, gt, " t=\"", 4);
                if(tpos) { tpos += 4; if(tpos + 1 < gt && *tpos == 's' && *(tpos + 1) == '"') isShared = true; }

                // r="AB12" -> kolona
                size_t colIndex = SIZE_MAX;
                const char* rpos = memmem_small(c1, gt, " r=\"", 4);
                if(rpos) {
                    rpos += 4;
                    unsigned colNum = 0;
                    const char* rp = rpos;
                    while(rp < gt && isalpha_fast(*rp)) {
                        colNum = colNum * 26 + (toupper_fast(*rp) - 'A' + 1);
                        ++rp;
                    }
                    if(colNum > 0) colIndex = (size_t)colNum - 1;
                }

                std::string value;
                if(!selfClose) {
                    const char* v1 = memmem_small(gt, rowEnd, "<v>", 3);
                    if(v1) {
                        const char* v2 = memmem_small(v1, rowEnd, "</v>", 4);
                        if(v2 && v2 < rowEnd) {
                            value.assign(v1 + 3, (size_t)(v2 - (v1 + 3)));

                            if(isShared && !value.empty()) {
                                unsigned idx = 0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
                                auto r = std::from_chars(value.data(), value.data() + value.size(), idx);
                                if(r.ec == std::errc())
#else
                                idx = (unsigned)std::strtoul(value.c_str(), nullptr, 10);
#endif
                                if(idx < sharedStrings_.size()) value = sharedStrings_[idx];
                            }
                            ccur = v2 + 4;
                        }
                        else {
                            ccur = gt + 1;
                        }
                    }
                    else {
                        ccur = gt + 1;
                    }
                }
                else {
                    ccur = gt + 1;
                }

                if(colIndex == SIZE_MAX) {
                    row.push_back(std::move(value));
                }
                else {
                    if(row.size() <= colIndex) row.resize(colIndex + 1);
                    row[colIndex] = std::move(value);
                }
            }

            data_[i] = std::move(row);
        }
        };

    size_t chunk = rows.size() / threads;
    size_t rem = rows.size() % threads;
    std::vector<std::future<void>> futs;
    futs.reserve(threads);

    size_t s = 0;
    for(unsigned t = 0; t < threads; ++t) {
        size_t add = chunk + (t < rem ? 1 : 0);
        size_t e = s + add;
        futs.emplace_back(std::async(std::launch::async, worker, s, e));
        s = e;
    }
    for(auto& f : futs) f.get();
}
