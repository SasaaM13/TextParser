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

#include <windows.h>
class JSONParser;

enum class JSONType
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct JSONNode
{
    uint32_t type;
    uint32_t startIndex; 
    uint32_t elementSize; 
    std::string_view str; 
    std::string_view raw; 
    double num;
    bool boolVal;
};

class JSONValue
{
public:
    JSONValue() = default;
    JSONValue(const JSONParser* o, size_t i) : owner(o), idx(i) {}

    bool valid() const { return owner && idx != SIZE_MAX; }

    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool() const;
    double asDouble() const;
    std::string_view asStringView() const;

    size_t size() const;
    size_t index() const { return idx; }
    JSONValue operator[](size_t i) const;
    JSONValue operator[](std::string_view key) const;

private:
    const JSONParser* owner = nullptr;
    size_t idx = SIZE_MAX;
};

class JSONParser final : public TextFileParser
{
public:
    struct Options
    {
        bool parallel = true;
        unsigned threadHint = 0;         
        size_t maxColumnsHint = 256;      
    };

    explicit JSONParser(std::string filename, Options opt = {}) : filename_(std::move(filename)), opt_(opt) {}
    ~JSONParser() {unmapFile();}
    bool load() override;

    size_t rowCount() const override { return rows_; }
    size_t colCount() const override { return cols_; }

    std::string_view valueView(size_t r, size_t c) const override
    {
        if(r >= rows_ || c >= cols_) return {};
        return dataViews_[r * cols_ + c];
    }
    JSONValue rootValue() const { return JSONValue(this, rootIndex_); }
    size_t totalFields() const { return totalFields_; }
    const std::string& value(size_t r, size_t c) const override;

    const DataNode& root() const override { return root_; };
    const std::vector<JSONNode>& nodes() const {return nodes_;};
    size_t rootIndex() const { return rootIndex_; }
    const std::vector<uint32_t>& childrenArena() const { return arenaChildren; }
    const std::vector<std::pair<std::string_view, uint32_t>>& membersArena() const { return arenaMembers; }
private:
    bool mapFile();
    void unmapFile();
    bool loadDOM();
    size_t parseValueDOM(std::string_view sv, size_t& i);
    size_t parseObjectDOM(std::string_view sv, size_t& i);
    size_t parseArrayDOM(std::string_view sv, size_t& i);
    size_t parseStringDOM(std::string_view sv, size_t& i);
    size_t parseNumberDOM(std::string_view sv, size_t& i);
    size_t parseLiteralDOM(std::string_view sv, size_t& i);
    bool parseFast();          
    bool parseSlowFallback();  

    enum class Mode { Unknown, FlatArray, NDJSON, SingleObject };

    static inline bool is_ws(unsigned char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }
    static size_t skip_ws(std::string_view sv, size_t i);
    static size_t skip_bom_and_ws(std::string_view sv);

    static bool parse_json_string_view(std::string_view sv, size_t& i, std::string_view& out);
    static bool parse_json_value_view(std::string_view sv, size_t& i, std::string_view& out); 
    static bool expect_char(std::string_view sv, size_t& i, char ch);

    Mode detectMode(std::string_view sv) const;
    bool parseFlatArray(std::string_view sv);
    bool parseNDJSON(std::string_view sv);
    bool parseSingleObjectToRoot(std::string_view sv);

    struct ObjSpan { size_t begin; size_t end; }; 
    static bool find_object_spans_in_array(std::string_view sv, size_t arrayBegin, size_t arrayEnd, std::vector<ObjSpan>& out);

    bool parseObjectRow(std::string_view obj, std::vector<std::string_view>& keysTmp, std::vector<std::string_view>& valsTmp, std::string_view* rowPtr);
    void buildColumnsFromFirstObject(const std::vector<std::string_view>& keysTmp);
    const std::string& materialize(size_t r, size_t c) const;
private:
    std::string filename_;
    Options opt_;
    std::vector<JSONNode> nodes_;
    std::vector<uint32_t> arenaChildren;
    std::vector<std::pair<std::string_view, uint32_t>> arenaMembers;
    size_t rootIndex_ = SIZE_MAX;
    const char* base_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMap_ = nullptr;
#else
    int fd_ = -1;
#endif

    std::vector<std::string_view> dataViews_;
    size_t rows_ = 0;
    size_t cols_ = 0;
    size_t totalFields_ = 0;
    std::unordered_map<std::string_view, size_t> colIndex_;
    mutable std::string empty_ = {};
};