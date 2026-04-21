#pragma once
#define _CRT_SECURE_NO_WARNINGS

#include "TextFileParser.h"
#include "DateParser.h"

#include <string>
#include <vector>
#include <string_view>
#include <optional>
#include <memory>
#include <utility>
#include <algorithm>
#include <cstdint>

// ===================================================
//   CSVParser
// ===================================================
class CSVParser final : public TextFileParser {
public:
    struct Options {
        char delimiter = ',';
        bool hasHeader = false;
        bool allowQuotes = true;
        bool useMMap = true;
        bool padMissingCells = true;
        bool trimLastColumnCRSemis = true;
    };

    explicit CSVParser(std::string filename, Options opts = {});
    ~CSVParser() override = default;

    CSVParser(const CSVParser&) = delete;
    CSVParser& operator=(const CSVParser&) = delete;

    bool load() override;

    size_t rowCount() const override { return rows_ + (opts_.hasHeader ? 1 : 0); }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override;
    const std::string& value(size_t r, size_t c) const override;

    CellKind cellKind(size_t r, size_t c) const override;

    std::optional<size_t> columnIndex(const std::string& name) const;

private:
    struct Buffer {
        const char* data = nullptr;
        size_t size = 0;
        bool mmapped = false;
#ifdef _WIN32
        void* hFile = nullptr;
        void* hMap = nullptr;
#else
        int fd = -1;
#endif
        std::vector<char> owned;
        void release();
        ~Buffer() { release(); }
    };

    struct CellSpan {
        uint32_t off;
        uint32_t len;
    };

    void resetState();
    bool mapFile(Buffer& buf) const;
    bool readFileBuffered(Buffer& buf) const;

    static void splitLineNoQuotes(std::string_view line, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    static void splitLineQuotesFast(std::string_view line, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    static size_t findNextDelimOrNL_AVX2(const char* s, size_t pos, size_t n, char delim);

    static bool looksLikeNumber(std::string_view v);
    static bool equalsIgnoreCase(std::string_view a, std::string_view b);

private:
    std::string filename_;
    Options opts_;

    size_t rows_ = 0;
    size_t cols_ = 0;

    std::vector<CellSpan> cells_;
    std::vector<std::pair<std::string_view, size_t>> headerIndex_;
    std::unique_ptr<Buffer> backing_;
};
