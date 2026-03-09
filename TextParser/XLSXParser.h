#pragma once
#include "TextFileParser.h"

#include <string>
#include <vector>
#include <string_view>
#include <cstdint>

class XLSXParser : public TextFileParser
{
public:
    explicit XLSXParser(std::string filename);

    bool open();                                // učita workbook + sheet list + sharedStrings
    bool selectSheet(int index);                // bira sheet po indeksu
    bool load() override;                       // učita aktivni sheet

    int sheetCount() const;
    std::string sheetName(int index) const;
    std::vector<std::string> allSheets() const;

    size_t rowCount() const override { return rows_; }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override;
    const std::string& value(size_t r, size_t c) const override;

    const DataNode& root() const override { return root_; }

    // CSV-like heuristic (fast, stable)
    CellKind cellKind(size_t row, size_t col) const override;

private:
    struct SheetInfo {
        std::string name;
        std::string path; // e.g. xl/worksheets/sheet1.xml
    };

    // ================= ZIP helpers (mmap/MapViewOfFile) =================
    struct MappedFile {
        const unsigned char* base = nullptr;
        size_t size = 0;
#ifdef _WIN32
        void* hFile = nullptr;
        void* hMap = nullptr;
#endif
        void close();
    };

    bool mapZip(MappedFile& mf) const;
    bool extractEntry(const MappedFile& mf, const std::string& innerPath, std::string& out) const;

    static uint32_t le32(const unsigned char* p);
    static uint16_t le16(const unsigned char* p);

    // ================= XML fast parsing =================
    void parseWorkbook(const std::string& xml);
    void parseSharedStrings(const std::string& xml);
    void parseSheetUltraFast(const std::string& xml);

    // ================= CellKind helpers (same as CSV idea) =================
    static bool equalsIgnoreCase(std::string_view a, std::string_view b);
    static bool looksLikeNumber(std::string_view v);

private:
    // ================= state =================
    std::string filename_;
    std::vector<SheetInfo> sheets_;
    int currentSheet_ = 0;

    // sheet data stored as single backing string + refs
    struct CellRef {
        uint32_t off = 0;       // offset into sheetXML_
        uint32_t len = 0;       // length
        uint32_t sst = 0;       // sharedStrings index if kind==CK_String and shared string
        uint8_t  kind = 0;      // stores CK_* enum value (but we don't trust it for cellKind)
    };

    size_t rows_ = 0;
    size_t cols_ = 0;

    std::string sheetXML_;              // backing for all inline cell views (valid after load())
    std::vector<CellRef> cells_;        // flat [rows_ * cols_]
    std::vector<std::string> sharedStrings_;

    // ===== fast char helpers =====
    static inline bool isalpha_fast(char c) {
        unsigned char u = (unsigned char)c;
        return (u | 32) >= 'a' && (u | 32) <= 'z';
    }
    static inline char toupper_fast(char c) {
        return (char)((unsigned char)c & 0xDF);
    }

    inline size_t flatIndex(size_t r, size_t c) const { return r * cols_ + c; }
};
