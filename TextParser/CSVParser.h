#pragma once
#define _CRT_SECURE_NO_WARNINGS

#include "TextFileParser.h"
#include <string>
#include <vector>
#include <string_view>
#include <optional>
#include <memory>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <mutex>

class CSVParser final : public TextFileParser {
public:
    struct Options {
        char delimiter = ',';
        bool hasHeader = false;

        // FAST quote handling (toggle on '"'). If you KNOW there are no quotes, set false for max speed.
        bool allowQuotes = true;

        // mmap input (recommended)
        bool useMMap = true;

        // If a row has fewer cols -> pad empty.
        // If a row has more cols -> ignore extra cells (fast & stable grid).
        bool padMissingCells = true;

        // Extra cleanup for weird inputs: trim trailing '\r' and up to two ';'
        // applied ONLY to last column of each row (fast).
        bool trimLastColumnCRSemis = true;
    };

    explicit CSVParser(std::string filename, Options opts = {});
    ~CSVParser() override = default;

    CSVParser(const CSVParser&) = delete;
    CSVParser& operator=(const CSVParser&) = delete;
    CSVParser(CSVParser&&) = delete;
    CSVParser& operator=(CSVParser&&) = delete;

    bool load() override;

    // base UI expects header included if hasHeader==true
    size_t rowCount() const override { return rows_ + (opts_.hasHeader ? 1 : 0); }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override;
    const std::string& value(size_t r, size_t c) const override;

    // Fast type hint for UI / filters
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
        uint32_t off = 0;
        uint32_t len = 0;
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

    size_t rows_ = 0;  // DATA rows (header not included)
    size_t cols_ = 0;

    std::vector<CellSpan> cells_; // flat [rows_ * cols_], stable grid

    // header name -> index (sorted)
    std::vector<std::pair<std::string, size_t>> headerIndex_;

    std::unique_ptr<Buffer> backing_;

    // lazy materialized strings
    mutable std::vector<std::optional<std::string>> cacheString_;
    mutable std::mutex cacheMutex_;
};
