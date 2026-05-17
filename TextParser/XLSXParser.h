#pragma once
#include "TextFileParser.h"

#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <optional>

class XLSXParser : public TextFileParser
{
public:
    explicit XLSXParser(std::string filename);
    ~XLSXParser();

    bool open();
    bool load() override;
    bool selectSheet(int index);

    int sheetCount() const;
    std::string sheetName(int index) const;

    size_t rowCount() const override { return rows_; }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override;
    const std::string& value(size_t r, size_t c) const override;

    CellKind cellKind(size_t r, size_t c) const override;
    const DataNode& root() const override { return root_; }

private:

    // ================= ZIP =================
    struct ZipEntry {
        uint16_t method = 0;
        uint32_t csize = 0;
        uint32_t usize = 0;
        uint32_t dataOff = 0;
    };

    struct MappedFile {
        const unsigned char* base = nullptr;
        size_t size = 0;
#ifdef _WIN32
        void* hFile = nullptr;
        void* hMap = nullptr;
#endif
        void close();
    };

    bool mapZip();
    bool buildZipIndex();
    bool extractEntry(const std::string& path, std::string& out) const;

    static uint32_t le32(const unsigned char* p);
    static uint16_t le16(const unsigned char* p);

    // ================= XML =================
    void parseWorkbook(const std::string& xml);
    void parseSharedStrings(const std::string& xml);
    void parseSheet(const std::string& xml);

    // ================= helpers =================
    static bool equalsIgnoreCase(std::string_view a, std::string_view b);
    static bool looksLikeNumber(std::string_view v);

    static inline bool isalpha_fast(char c) {
        unsigned char u = (unsigned char)c;
        return (u | 32) >= 'a' && (u | 32) <= 'z';
    }

    static inline char toupper_fast(char c) {
        return (char)((unsigned char)c & 0xDF);
    }

    inline size_t idx(size_t r, size_t c) const { return r * cols_ + c; }

    void parseStyles(const std::string& xml);
    bool isDateStyle(uint16_t style) const;
private:

    std::string filename_;

    // ZIP
    MappedFile zip_;
    std::unordered_map<std::string, ZipEntry> zipIndex_;

    std::vector<bool> styleIsDate_;
    // workbook
    struct SheetInfo {
        std::string name;
        std::string path;
    };
    std::vector<SheetInfo> sheets_;
    int currentSheet_ = 0;

    // shared strings (BLOB)
    struct StrRef {
        uint32_t off = 0;
        uint32_t len = 0;
    };
    std::string sharedBlob_;
    std::vector<StrRef> sharedRefs_;

    // sheet data
    struct CellRef {
        uint32_t off = 0;
        uint32_t len = 0;
        uint32_t sst = 0;
        uint8_t  kind = 0;
        uint16_t style = 0;
    };

    std::string sheetXML_;
    std::vector<CellRef> cells_;

    size_t rows_ = 0;
    size_t cols_ = 0;

    // cache (optional)
    mutable std::vector<std::optional<std::string>> cache_;
    mutable std::mutex cacheMutex_;

    DataNode root_;
};