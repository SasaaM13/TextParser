// TextFileParser.h
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <string_view>
#include <mutex>
#include <limits>

struct DataNode {
    std::string name;               
    std::string value;              
    std::vector<DataNode> children; 
    size_t level = 0;               

    DataNode() = default;
    DataNode(std::string n, std::string v = "", size_t lvl = 0): name(std::move(n)), value(std::move(v)), level(lvl) {}
};

class TextFileParser 
{
public:
    enum class BoolFormat : uint8_t
    {
        NONE = 0,
        TRUE_FALSE = 1 << 0,
        YES_NO = 1 << 1,  
        // ONE_ZERO = 1 << 2  // "1"/"0"
    };

    enum CellKind : uint8_t 
    {
        CK_Empty = 0,
        CK_String = 1,
        CK_Number = 2,
        CK_Bool = 3,
        CK_Date = 4
    };

    virtual ~TextFileParser() = default;
    virtual bool load() = 0;

    virtual size_t rowCount() const { return 0; }
    virtual size_t colCount() const { return 0; }

    virtual std::string_view valueView(size_t /*row*/, size_t /*col*/) const 
    {
        static constexpr std::string_view empty{};
        return empty;
    }

    virtual CellKind cellKind(size_t row, size_t col) const {
        auto v = valueView(row, col);
        return v.empty() ? CellKind::CK_Empty : CellKind::CK_String;
    }

    virtual const std::string& value(size_t row, size_t col) const = 0;

    virtual const DataNode& root() const { return root_; }

    virtual std::string getColName(size_t index) const
    {
        if(index < colNames_.size())
            return colNames_[index];
        static const std::string empty;
        return empty;
    }

    virtual const std::vector<std::string>& getColNames() const { return colNames_; }

    std::optional<int>    toInt(size_t row, size_t col);
    std::optional<double> toDouble(size_t row, size_t col);

    static constexpr BoolFormat DefaultBoolFmt =
        static_cast<BoolFormat>(
            static_cast<uint8_t>(BoolFormat::TRUE_FALSE) |
            static_cast<uint8_t>(BoolFormat::YES_NO));

    std::optional<bool> toBool(size_t row, size_t col, BoolFormat fmt = DefaultBoolFmt);

protected:
    void notifyLoaded();
    void ensureStringCacheSize() const;
    void ensureIntCacheSize() const;
    void ensureDoubleCacheSize() const;
    void ensureBoolCacheSize() const;
    static bool parseInt(std::string_view s, int& out);
    static bool parseDouble(std::string_view s, double& out);
    static bool parseBool(std::string_view s, bool& out, BoolFormat fmt);

    std::vector<std::string> colNames_;
    DataNode root_;

    mutable std::vector<std::optional<std::string>> cacheString_;
    mutable std::mutex cacheMutex_;

    mutable std::vector<std::optional<int>>    cacheInt_;
    mutable std::vector<std::optional<double>> cacheDouble_;
    mutable std::vector<std::optional<bool>>   cacheBool_;

    mutable std::vector<uint8_t> seenInt_; //TODO:remove
    mutable std::vector<uint8_t> seenDouble_;
    mutable std::vector<uint8_t> seenBool_; 

    std::optional<size_t> keyChecked(size_t r, size_t c) const;
};
