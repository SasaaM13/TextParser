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

class CSVParser final : public TextFileParser {
public:
    struct Options {
        char delimiter = ',';
        bool hasHeader = false;

        // FAST quote handling (toggle on '"'). If you KNOW there are no quotes, set false for max speed.
        bool allowQuotes = true;

        // mmap input (recommended)
        bool useMMap = true;

        // If a row has fewer cols -> pad empty views.
        // If a row has more cols -> ignore extra cells (fast & stable grid).
        bool padMissingCells = true;
    };

    explicit CSVParser(std::string filename, Options opts = {});
    ~CSVParser() override = default;

    CSVParser(const CSVParser&) = delete;
    CSVParser& operator=(const CSVParser&) = delete;
    CSVParser(CSVParser&&) = delete;
    CSVParser& operator=(CSVParser&&) = delete;

    bool load() override;

    size_t rowCount() const override { return rows_; }
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

    void resetState();
    bool mapFile(Buffer& buf) const;
    bool readFileBuffered(Buffer& buf) const;

    // Parse one line [b,e) into spans (offset,len) relative to b
    static void splitLineNoQuotes(std::string_view line, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    static void splitLineQuotesFast(std::string_view line, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    // AVX2: find next occurrence of delim or '\n' (returns index in [pos,n], n if none)
    static size_t findNextDelimOrNL_AVX2(const char* s, size_t pos, size_t n, char delim);

    static bool looksLikeNumber(std::string_view v);
    static bool equalsIgnoreCase(std::string_view a, std::string_view b);

private:
    std::string filename_;
    Options opts_;

    size_t rows_ = 0;
    size_t cols_ = 0;

    std::vector<std::string_view> cells_; // flat [rows_ * cols_], stable grid

    // header name -> index (sorted)
    std::vector<std::pair<std::string, size_t>> headerIndex_;

    std::unique_ptr<Buffer> backing_;
};
