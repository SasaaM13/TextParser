#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <string_view>
#include <mutex>

// ===================================================
//   Universal Data Model for all text-based formats
// ===================================================
struct DataNode {
    std::string name;               // ime taga / kolone / ključa
    std::string value;              // vrednost (može biti prazno)
    std::vector<DataNode> children; // ugnježdeni čvorovi
    size_t level = 0;               // dubina u hijerarhiji

    DataNode() = default;
    DataNode(std::string n, std::string v = "", size_t lvl = 0)
        : name(std::move(n)), value(std::move(v)), level(lvl) {
    }
};

// ===================================================
//   TextFileParser (base class)
// ===================================================
class TextFileParser {
public:
    // --- Enum sa bit maskama za bool parsiranje ---
    enum class BoolFormat : uint8_t {
        NONE = 0,
        TRUE_FALSE = 1 << 0,  // "true"/"false"
        YES_NO = 1 << 1,      // "yes"/"no"
        ONE_ZERO = 1 << 2     // "1"/"0"
    };

    virtual ~TextFileParser() = default;
    virtual bool load() = 0;

    // === Tabelarni interfejs (CSV / XLSX) ===
    virtual size_t rowCount() const { return 0; }
    virtual size_t colCount() const { return 0; }

    // BEZ KOPIJE — preporučeno za brzi rad
    virtual std::string_view valueView(size_t /*row*/, size_t /*col*/) const {
        static constexpr std::string_view empty{};
        return empty;
    }

    // Kompatibilnost sa starim interfejsom: lenjo materijalizuje string
    virtual const std::string& value(size_t row, size_t col) const = 0;

    // === Hijerarhijski interfejs (XML / JSON) ===
    virtual const DataNode& root() const { return root_; }

    // === Kolone (zaglavlje) ===
    virtual std::string getColName(size_t index) const {
        if(index < colNames_.size()) return colNames_[index];
        static const std::string empty;
        return empty;
    }

    virtual const std::vector<std::string>& getColNames() const {
        return colNames_;
    }

    // --- Tipizirane konverzije (na bazi valueView) ---
    std::optional<int>    toInt(size_t row, size_t col);
    std::optional<double> toDouble(size_t row, size_t col);
    std::optional<bool>   toBool(size_t row, size_t col,
        BoolFormat fmt = static_cast<BoolFormat>(
            static_cast<uint8_t>(BoolFormat::TRUE_FALSE) |
            static_cast<uint8_t>(BoolFormat::YES_NO) |
            static_cast<uint8_t>(BoolFormat::ONE_ZERO)));

protected:
    void notifyLoaded();

    static bool parseInt(std::string_view s, int& out);
    static bool parseDouble(std::string_view s, double& out);
    static bool parseBool(std::string_view s, bool& out, BoolFormat fmt);

    std::vector<std::string> colNames_;
    DataNode root_;

    // Thread-safe lazy string cache (za value()) — flat po ćeliji
    mutable std::vector<std::optional<std::string>> cacheString_;
    mutable std::mutex cacheMutex_;

    // 🧠 Tipizirani cache-ovi (dodajeni!)
    mutable std::vector<std::optional<int>>    cacheInt_;
    mutable std::vector<std::optional<double>> cacheDouble_;
    mutable std::vector<std::optional<bool>>   cacheBool_;

    size_t key(size_t r, size_t c) const { return r * colCount() + c; }
};
