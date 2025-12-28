#pragma once
#include "TextFileParser.h"
#include <string>
#include <vector>

class XLSXParser : public TextFileParser
{
public:
    explicit XLSXParser(std::string filename);

    bool open();                                // učita workbook i listu sheetova
    bool selectSheet(int index);                // bira sheet po indeksu
    bool load() override;                       // učita aktivni sheet

    int sheetCount() const;
    std::string sheetName(int index) const;
    std::vector<std::string> allSheets() const;

    size_t rowCount() const override;
    size_t colCount() const override;
    const std::string& value(size_t r, size_t c) const override;

    const DataNode& root() const override { return root_; }

private:
    struct SheetInfo {
        std::string name;
        std::string path;
    };

    // ==== ZIP helpers (mmap/MapViewOfFile) ====
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

    // ==== XML helpers ====
    void parseWorkbook(const std::string& xml);
    void parseSharedStrings(const std::string& xml);
    void parseSheetFastParallel(const std::string& xml);

    // ==== state ====
    std::string filename_;
    std::vector<SheetInfo> sheets_;
    int currentSheet_ = 0;

    std::vector<std::vector<std::string>> data_;
    std::vector<std::string> sharedStrings_;   // t="s" indeksacija

    // ==== fast char helpers ====
    static inline bool isalpha_fast(char c) {
        unsigned char u = (unsigned char)c;
        return (u | 32) >= 'a' && (u | 32) <= 'z';
    }
    static inline char toupper_fast(char c) {
        return (char)((unsigned char)c & 0xDF);
    }
};
