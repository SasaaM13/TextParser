#pragma once
#include "TextFileParser.h"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "DateParser.h"

enum class XMLNodeType : uint32_t
{
    Element,
    Text
};

struct XMLNode
{
    uint32_t type = 0;
    std::string_view name{};
    std::string_view text{};

    uint32_t parent = UINT32_MAX;
    uint32_t firstChild = UINT32_MAX;
    uint32_t nextSibling = UINT32_MAX;
};

struct XMLNodeP
{
    uint32_t parent = UINT32_MAX;
    uint32_t firstChild = UINT32_MAX;
    uint32_t nextSibling = UINT32_MAX;
    uint32_t lastChild = UINT32_MAX;

    const char* name = nullptr;
    uint32_t nameLen = 0;

    const char* text = nullptr;
    uint32_t textLen = 0;
};

class XMLParser;
class XMLValue
{
public:
    XMLValue() = default;
    XMLValue(const XMLParser* p, uint32_t i) : owner_(p), idx_(i) {}

    bool valid() const;

    bool isElement() const;
    bool isText() const;

    std::string_view name() const;
    std::string_view text() const;

    XMLValue firstChild() const;
    XMLValue nextSibling() const;
    XMLValue child(std::string_view name) const;
    std::string_view childText(std::string_view name) const;
    std::optional<int> toInt() const;
    std::optional<double> toDouble() const;
    std::optional<bool> toBool() const;
    std::optional<Date> toDate() const;
    std::vector<XMLValue> children(std::string_view name) const;
private:
    const XMLParser* owner_ = nullptr;
    uint32_t idx_ = UINT32_MAX;
};

class XMLParser final : public TextFileParser
{
public:
    struct Options
    {
        bool usePointerDom = true; 
    };

    explicit XMLParser(std::string path = "", Options opt = {});
    ~XMLParser() { unmapFile(); }

    bool load() override;
    XMLValue rootValue() const { return XMLValue(this, rootIndex_); }
    size_t elementCount() const { return elementCount_; }
    const std::string& value(size_t, size_t) const override
    {
        static const std::string empty;
        return empty;
    }
    const std::vector<XMLNodeP>& nodesP() const { return nodesP_; }
private:
    bool usingPointerDom() const { return opt_.usePointerDom; }
    bool mapFile();
    void unmapFile();
    void reset();
    bool parseDOMPointer(); 
    size_t findSeq(size_t from, std::string_view seq) const;

    inline uint32_t appendNodeP(const XMLNodeP& n)
    {
        nodesP_.push_back(n);
        return (uint32_t)(nodesP_.size() - 1);
    }
    inline void appendChildP(uint32_t parent, uint32_t child)
    {
        auto& p = nodesP_[parent];
        if(p.firstChild == UINT32_MAX)
        {
            p.firstChild = child;
        }
        else
        {
            nodesP_[p.lastChild].nextSibling = child;
        }

        p.lastChild = child;
    }

    inline size_t findNextLT(size_t pos) const
    {
        const void* hit = std::memchr(data_ + pos, '<', size_ - pos);
        if(!hit)
            return size_t(-1);
        return (const char*)hit - data_;
    }

private:
    std::string path_;
    Options opt_;

    char* data_ = nullptr;
    char* base_ = nullptr;
    const char* end_ = nullptr;
    size_t size_ = 0;

    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMap_ = nullptr;
    std::vector<XMLNodeP> nodesP_;
    uint32_t rootIndex_ = 0;
    size_t elementCount_ = 0;
    friend class XMLValue;
};