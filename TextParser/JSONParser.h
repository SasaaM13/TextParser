#pragma once
#include "TextFileParser.h"
#include <string>
#include <vector>
#include <future>
#include <memory>
#include <mutex>
#include <optional>

// ==============================================
//   JSONParser — hijerarhijski + "flat" tabelarni model
//   Ultra-fast: mmap + std::string_view + paralelno + AVX2 (fallback scalar)
// ==============================================
class JSONParser : public TextFileParser {
public:
    struct Options {
        bool preferFastPath = true;      // koristi brzi režim kad je moguće
        bool useMMap = true;      // mem-map fajl
        bool useSIMD = true;      // AVX2 ubrzanja za pretrage
        unsigned threadHint = 0;         // 0 → hw_concurrency
        size_t fastMinSize = 64 * 1024; // min. veličina za fast path
    };

    explicit JSONParser(const std::string& filename, Options opt = {})
        : filename_(filename), opt_(opt) {
    }

    bool load() override;

    // === Tabelarni interfejs (za flat JSON array of objects) ===
    size_t rowCount() const override { return data_.size(); }
    size_t colCount() const override { return data_.empty() ? 0 : data_[0].size(); }
    const std::string& value(size_t r, size_t c) const override {
        static const std::string empty;
        if(r >= data_.size() || c >= data_[r].size()) return empty;
        return data_[r][c];
    }

    // === Hijerarhijski interfejs ===
    const DataNode& root() const override { return root_; }

private:
    // --- Helperi (UTF-8-friendly u meri u kojoj je JSON validan) ---
    static void skip_ws(const std::string_view& s, size_t& i);
    static std::string parse_string(const std::string_view& s, size_t& i);
    static DataNode parse_value(const std::string_view& s, size_t& i,
        const std::string& name = "", size_t level = 0);

    // --- Brzi režim ---
    bool load_fast();      // mmap + paralelno + (opciono) AVX2
    bool load_slow();      // stari, robusni parser

    // --- SIMD pomoćne (fallback na scalar) ---
    static bool has_avx2_runtime();
    static size_t sv_find_char_scalar(std::string_view sv, size_t from, size_t to, char ch);
    static size_t sv_find_char_any_scalar(std::string_view sv, size_t from, size_t to,
        const char* chars, size_t nchar);

    static size_t sv_find_char_avx2(std::string_view sv, size_t from, size_t to, char ch);                  // AVX2
    static size_t sv_find_char_any_avx2(std::string_view sv, size_t from, size_t to,
        const char* chars, size_t nchar);                                   // AVX2

    // --- Polja ---
    std::string filename_;
    Options opt_;
    std::vector<std::vector<std::string>> data_;
    std::mutex mut_; // write guard za data_/colNames_ u fast path-u
};
