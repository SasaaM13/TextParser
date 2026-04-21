#pragma once
#include <string_view>
#include "CSVParser.h"
#include <rapidcsv.h>
class ITableAdapter
{
public:
    virtual ~ITableAdapter() = default;

    virtual bool load() = 0;

    virtual size_t rows() const = 0;
    virtual size_t cols() const = 0;

    virtual std::string_view value(size_t r, size_t c) const = 0;
    virtual double toDouble(size_t r, size_t c) const = 0;
};

class CustomCSVAdapter : public ITableAdapter
{
    CSVParser csv;

public:
    CustomCSVAdapter(const std::string& path)
        : csv(path, CSVParser::Options{}) {
    }

    bool load() override { return csv.load(); }

    size_t rows() const override { return csv.rowCount(); }
    size_t cols() const override { return csv.colCount(); }

    std::string_view value(size_t r, size_t c) const override
    {
        return csv.valueView(r, c);
    }

    double toDouble(size_t r, size_t c) const override
    {
        if(auto v = csv.toDouble(r, c)) return *v;
        return 0.0;
    }
};

class RapidCSVAdapter : public ITableAdapter
{
    rapidcsv::Document doc;
    std::vector<std::vector<std::string>> cache;

public:
    RapidCSVAdapter(const std::string& path)
        : doc(path) {
    }

    bool load() override
    {
        size_t r = doc.GetRowCount();
        size_t c = doc.GetColumnCount();

        cache.resize(r, std::vector<std::string>(c));

        for(size_t i = 0; i < r; ++i)
            for(size_t j = 0; j < c; ++j)
                cache[i][j] = doc.GetCell<std::string>(j, i);

        return true;
    }

    size_t rows() const override { return cache.size(); }
    size_t cols() const override { return cache.empty() ? 0 : cache[0].size(); }

    std::string_view value(size_t r, size_t c) const override
    {
        return cache[r][c];
    }

    double toDouble(size_t r, size_t c) const override
    {
        return std::strtod(cache[r][c].c_str(), nullptr);
    }
};