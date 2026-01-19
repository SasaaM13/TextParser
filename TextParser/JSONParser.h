#pragma once
#include "TextFileParser.h"

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <mutex>
#include <filesystem>
#include <thread>
#include <future>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

class JSONParser final : public TextFileParser {
public:
    struct Options {
        bool preferFastPath = true;
        bool useMMap = true;
        bool parallel = true;
        unsigned threadHint = 0;          // 0 => hw_concurrency
        size_t fastMinSize = 64 * 1024;
        size_t maxColumnsHint = 256;      // safety cap (anti pathological)
        bool allowBareValues = true;      // broj, true/false, null
    };

    explicit JSONParser(std::string filename, Options opt = {})
        : filename_(std::move(filename)), opt_(opt) {
    }
    ~JSONParser() {unmapFile();}
    bool load() override;

    // table iface
    size_t rowCount() const override { return rows_; }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override {
        if(r >= rows_ || c >= cols_) return {};
        return dataViews_[r * cols_ + c];
    }

    // kompatibilnost: lenjo pravi std::string u base cacheString_
    const std::string& value(size_t r, size_t c) const override;

    // hierarchy (fallback)
    const DataNode& root() const override { return root_; }

private:
    // ===== mmap handling =====
    bool mapFile();
    void unmapFile();

    // ===== detection & parse =====
    bool parseFast();          // mmap + views
    bool parseSlowFallback();  // ako hoćeš, može ostati tvoj postojeći slow

    enum class Mode { Unknown, FlatArray, NDJSON, SingleObject };

    static inline bool is_ws(unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }
    static size_t skip_ws(std::string_view sv, size_t i);
    static size_t skip_bom_and_ws(std::string_view sv);

    // JSON token helpers (BRZI, minimalni)
    static bool parse_json_string_view(std::string_view sv, size_t& i, std::string_view& out);
    static bool parse_json_value_view(std::string_view sv, size_t& i, std::string_view& out); // string or bare token
    static bool expect_char(std::string_view sv, size_t& i, char ch);

    // Top-level modes
    Mode detectMode(std::string_view sv) const;
    bool parseFlatArray(std::string_view sv);
    bool parseNDJSON(std::string_view sv);
    bool parseSingleObjectToRoot(std::string_view sv); // optional

    // Work units
    struct ObjSpan { size_t begin; size_t end; }; // [begin,end) in sv
    static bool find_object_spans_in_array(std::string_view sv, size_t arrayBegin, size_t arrayEnd,
        std::vector<ObjSpan>& out, bool parallelHint);
    static bool find_object_span_in_line(std::string_view line, ObjSpan& out);

    // Parse one object { "k":"v", ... } into row views
    bool parseObjectRow(std::string_view obj, std::vector<std::string_view>& keysTmp,
        std::vector<std::string_view>& valsTmp,
        std::vector<std::string_view>& rowOut);

    // columns mapping (keys -> index)
    void buildColumnsFromFirstObject(const std::vector<std::string_view>& keysTmp);

    // Lazy materialization
    const std::string& materialize(size_t r, size_t c) const;

private:
    std::string filename_;
    Options opt_;

    // mapped data
    const char* base_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMap_ = nullptr;
#else
    int fd_ = -1;
#endif

    // table storage (views)
    std::vector<std::string_view> dataViews_;
    size_t rows_ = 0;
    size_t cols_ = 0;

    // columns
    // colNames_ is inherited (std::vector<std::string>)
    // We'll store col names as strings, but build them once.

    // internal temp
    mutable std::string empty_ = {};
};
