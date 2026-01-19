#pragma once
#define _CRT_SECURE_NO_WARNINGS

#include "TextFileParser.h"
#include <string>
#include <vector>
#include <optional>
#include <string_view>
#include <filesystem>
#include <thread>
#include <mutex>
#include <memory>

// ===================================================
//   UltraFast CSV Parser
// ===================================================

class CSVParser : public TextFileParser
{
public:
    struct Options {
        char delimiter = ',';
        int  maxRows = -1;
        int  maxCols = -1;
        bool hasHeader = false;
        bool allowQuotes = true;
        bool useMMap = true;
        size_t threadHint = std::thread::hardware_concurrency();
    };

    explicit CSVParser(std::string filename, Options opts = {});

    bool load() override;

    size_t rowCount() const override { return rows_; }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override;
    const std::string& value(size_t r, size_t c) const override;

    std::optional<size_t> columnIndex(const std::string& name) const;

private:
    struct Buffer {
        const char* data = nullptr;
        size_t size = 0;
        bool mmapped = false;
#ifdef _WIN32
        void* fileHandle = nullptr;
        void* mapHandle = nullptr;
#else
        int fd = -1;
#endif
        std::vector<char> owned;
        void release();
        ~Buffer() { release(); }
    };

    struct LineRange { size_t begin, end; };

    bool mapFile(Buffer& buf) const;
    bool readFileBuffered(Buffer& buf) const;

    void buildLineRanges(const Buffer& buf, std::vector<LineRange>& lines) const;
    void parallelSplitLines(const Buffer& buf, const std::vector<LineRange>& lines);

    static void splitRowNoQuotes(std::string_view row, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    static void splitRowWithQuotes(std::string_view row, char delim,
        std::vector<std::pair<size_t, size_t>>& spans);

    inline size_t flatIndex(size_t r, size_t c) const { return r * cols_ + c; }

private:
    std::string filename_;
    Options opts_;

    size_t rows_ = 0;
    size_t cols_ = 0;

    std::vector<std::string_view> cells_; // flat [row * col]
    std::vector<std::string> colNames_;
    std::vector<std::pair<std::string, size_t>> headerIndex_;

    std::unique_ptr<Buffer> backing_;
};
