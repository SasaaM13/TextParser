#pragma once
#include "TextFileParser.h"
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include <unordered_map>

struct XMLValue {
    std::string_view tag;
    std::string_view text;
};
class XMLParser : public TextFileParser
{
public:
    static constexpr std::size_t npos = (std::size_t)-1;

    XMLParser(std::string path = "");
    void setBuffer(const char* data, std::size_t size);

    bool load() override;

    std::size_t elementCount() const noexcept { return elementCount_; }
    std::size_t textNodeCount() const noexcept { return textNodeCount_; }
    const std::string& value(size_t row, size_t col) const override 
    {
        static const std::string empty;
        return empty;
    };
    std::optional<XMLValue> getByIndex(size_t index) const;
    std::vector<XMLValue> getByName(std::string_view name) const;

    std::optional<std::string_view> findFirst(std::string_view tag) const;
    std::vector<std::string_view> findAll(std::string_view tag) const;
    std::optional<std::string_view> getValueAtIndex(size_t index) const;

    const char* rawData() const { return data_; }
    const char* rawEnd()  const { return end_; }
private:
    // Core parsing
    void parseFast();

    // Helpers
    static bool isAllWhitespace(const char* s, std::size_t len);
    std::size_t findSeq(std::size_t from, std::string_view seq) const;
    std::size_t findNextLT(std::size_t pos) const;

    // Whitespace skipper
    inline void skipWS(std::size_t& pos)
    {
        while(pos < size_ &&
            (data_[pos] == ' ' ||
                data_[pos] == '\t' ||
                data_[pos] == '\n' ||
                data_[pos] == '\r'))
            ++pos;
    }

private:
    std::string path_;
    std::vector<char> buffer_;
    const char* data_ = nullptr;
    const char* end_ = nullptr;
    std::size_t size_ = 0;

    std::size_t elementCount_ = 0;
    std::size_t textNodeCount_ = 0;

    std::vector<std::pair<std::string_view, std::string_view>> values_; // tag→text
    std::unordered_map<std::string_view, std::vector<size_t>> indexByName_;
};
