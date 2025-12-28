#include "JSONParser.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <thread>
#include <future>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#include <immintrin.h>
#define JSONP_HAS_COMPILETIME_AVX2 1
#else
#define JSONP_HAS_COMPILETIME_AVX2 0
#endif

using namespace std;

// ================================
//   Pomocne funkcije
// ================================
void JSONParser::skip_ws(const std::string_view& s, size_t& i) {
    while(i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if(c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        break;
    }
}
static inline bool at_end(const std::string_view& s, size_t i) { return i >= s.size(); }

std::string JSONParser::parse_string(const std::string_view& s, size_t& i) {
    std::string out;
    if(at_end(s, i) || s[i] != '"') return out;
    ++i;
    while(!at_end(s, i) && s[i] != '"') {
        char c = s[i];
        if(c == '\\') {
            ++i;
            if(at_end(s, i)) break;
            char e = s[i];
            switch(e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '"': out.push_back('"'); break;
            case '\\':out.push_back('\\'); break;
            default:  out.push_back(e); break;
            }
        }
        else out.push_back(c);
        ++i;
    }
    if(!at_end(s, i) && s[i] == '"') ++i;
    return out;
}

DataNode JSONParser::parse_value(const std::string_view& s, size_t& i,
    const std::string& name, size_t level) {
    skip_ws(s, i);
    if(at_end(s, i)) return DataNode(name, "", level);

    if(s[i] == '{') {
        DataNode node(name, "", level);
        ++i;
        while(true) {
            skip_ws(s, i);
            if(at_end(s, i)) break;
            if(s[i] == '}') { ++i; break; }
            if(s[i] != '"') { ++i; continue; }
            std::string key = parse_string(s, i);
            skip_ws(s, i);
            if(at_end(s, i) || s[i] != ':') { ++i; continue; }
            ++i;
            DataNode child = parse_value(s, i, key, level + 1);
            node.children.push_back(std::move(child));
            skip_ws(s, i);
            if(at_end(s, i)) break;
            if(s[i] == ',') { ++i; continue; }
            if(s[i] == '}') { ++i; break; }
        }
        return node;
    }

    if(s[i] == '[') {
        DataNode node(name, "", level);
        ++i;
        int idx = 0;
        while(true) {
            skip_ws(s, i);
            if(at_end(s, i) || s[i] == ']') { if(!at_end(s, i)) ++i; break; }
            DataNode child = parse_value(s, i, "elem" + std::to_string(idx++), level + 1);
            node.children.push_back(std::move(child));
            skip_ws(s, i);
            if(at_end(s, i)) break;
            if(s[i] == ',') { ++i; continue; }
            if(s[i] == ']') { ++i; break; }
        }
        return node;
    }

    if(s[i] == '"') {
        std::string val = parse_string(s, i);
        return DataNode(name, val, level);
    }

    size_t start = i;
    while(!at_end(s, i) && !isspace((unsigned char)s[i]) && s[i] != ',' && s[i] != ']' && s[i] != '}')
        ++i;
    std::string val(s.substr(start, i - start));
    return DataNode(name, val, level);
}

// ================================
//   Spori fallback (ifstream) — podržava [..], {...}, NDJSON
// ================================
bool JSONParser::load_slow() {
    std::ifstream in(filename_, std::ios::binary);
    if(!in.is_open()) return false;

    std::string text((std::istreambuf_iterator<char>(in)), {});
    size_t i = 0;

    // --- preskoči UTF-8 BOM ---
    if(text.size() >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
    {
        i = 3;
    }

    skip_ws(text, i);
    if(i >= text.size()) { root_ = DataNode("root", "", 0); notifyLoaded(); return true; }

    data_.clear();
    root_ = DataNode("root", "", 0);

    // ===== 1. Flat array: [ {..}, {..} ] =====
    if(text[i] == '[') {
        ++i;
        std::vector<std::string> keys;
        while(true) {
            skip_ws(text, i);
            if(i >= text.size() || text[i] == ']') break;
            if(text[i] != '{') { root_ = parse_value(text, i, "root", 0); notifyLoaded(); return true; }
            ++i;
            std::vector<std::string> currentKeys, values;
            while(true) {
                skip_ws(text, i);
                if(i >= text.size()) break;
                if(text[i] == '}') { ++i; break; }
                if(text[i] != '"') { ++i; continue; }
                std::string key = parse_string(text, i);
                skip_ws(text, i);
                if(i >= text.size() || text[i] != ':') { ++i; continue; }
                ++i; skip_ws(text, i);
                std::string val;
                if(i < text.size() && text[i] == '"') val = parse_string(text, i);
                else {
                    size_t v0 = i;
                    while(i < text.size() && text[i] != ',' && text[i] != '}') ++i;
                    val = text.substr(v0, i - v0);
                }
                currentKeys.push_back(key);
                values.push_back(val);
                skip_ws(text, i);
                if(i < text.size() && text[i] == ',') { ++i; continue; }
                if(i < text.size() && text[i] == '}') { ++i; break; }
            }
            if(keys.empty()) { keys = currentKeys; colNames_ = keys; }
            std::vector<std::string> row(keys.size());
            for(size_t k = 0; k < currentKeys.size(); ++k)
                for(size_t idx = 0; idx < keys.size(); ++idx)
                    if(keys[idx] == currentKeys[k]) row[idx] = values[k];
            data_.push_back(std::move(row));
            skip_ws(text, i);
            if(i >= text.size()) break;
            if(text[i] == ',') { ++i; continue; }
            if(text[i] == ']') { ++i; break; }
        }
        notifyLoaded();
        return true;
    }

    // ===== 2. NDJSON / JSON Lines =====
    if(text[i] == '{') {
        std::ifstream in(filename_, std::ios::binary);
        if(!in.is_open()) return false;

        std::vector<std::string> keys;
        std::string line;
        line.reserve(4096); // prealloc

        while(true) {
            if(!std::getline(in, line)) break;
            size_t j = 0;
            skip_ws(line, j);
            if(j >= line.size() || line[j] != '{') continue;

            std::vector<std::string> currentKeys, values;
            ++j;
            while(j < line.size()) {
                skip_ws(line, j);
                if(j >= line.size() || line[j] == '}') { ++j; break; }
                if(line[j] != '"') { ++j; continue; }

                std::string key = parse_string(line, j);
                skip_ws(line, j);
                if(j >= line.size() || line[j] != ':') { ++j; continue; }
                ++j; skip_ws(line, j);

                std::string val;
                if(j < line.size() && line[j] == '"') val = parse_string(line, j);
                else {
                    size_t v0 = j;
                    while(j < line.size() && line[j] != ',' && line[j] != '}') ++j;
                    val = line.substr(v0, j - v0);
                }
                currentKeys.push_back(std::move(key));
                values.push_back(std::move(val));

                skip_ws(line, j);
                if(j < line.size() && line[j] == ',') { ++j; continue; }
                if(j < line.size() && line[j] == '}') { ++j; break; }
            }

            if(keys.empty()) { keys = currentKeys; colNames_ = keys; }
            std::vector<std::string> row(keys.size());
            for(size_t k = 0; k < currentKeys.size(); ++k)
                for(size_t idx = 0; idx < keys.size(); ++idx)
                    if(keys[idx] == currentKeys[k])
                        row[idx] = values[k];
            data_.push_back(std::move(row));
        }

        notifyLoaded();
        return true;
    }

    // ===== 3. Single object =====
    root_ = parse_value(text, i, "root", 0);
    notifyLoaded();
    return true;
}

// ================================
//   Fast loader (mmap) — detektuje [..], {..}, NDJSON
// ================================
bool JSONParser::load_fast() {
    namespace fs = std::filesystem;
    size_t fsize = fs::file_size(filename_);
    if(fsize == 0) { root_ = DataNode("root", "", 0); notifyLoaded(); return true; }

#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(hFile == INVALID_HANDLE_VALUE) return false;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if(!hMap) { CloseHandle(hFile); return false; }
    const char* base = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if(!base) { CloseHandle(hMap); CloseHandle(hFile); return false; }
    std::string_view sv(base, fsize);
#else
    int fd = open(filename_.c_str(), O_RDONLY);
    if(fd < 0) return false;
    void* mapped = mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mapped == MAP_FAILED) { close(fd); return false; }
    const char* base = (const char*)mapped;
    std::string_view sv(base, fsize);
#endif

    size_t p = 0;
    skip_ws(sv, p);

    // preskoči UTF-8 BOM ako postoji
    if(p + 3 < sv.size() &&
        (unsigned char)sv[p] == 0xEF &&
        (unsigned char)sv[p + 1] == 0xBB &&
        (unsigned char)sv[p + 2] == 0xBF) {
        p += 3;
        skip_ws(sv, p);
    }

    char start = (p < sv.size()) ? sv[p] : 0;

    bool ok = false;
    if(start == '[') ok = true;      // flat array
    else if(start == '{') ok = true; // NDJSON or single-object

#ifdef _WIN32
    UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
#else
    munmap((void*)base, fsize); close(fd);
#endif

    if(!ok) return load_slow();
    // koristimo spori parser jer je mmap samo za peek ovde
    return load_slow();
}

// ================================
//   load(): bira fast/slow
// ================================
bool JSONParser::load() {
    namespace fs = std::filesystem;
    if(!fs::exists(filename_)) return false;
    if(!opt_.preferFastPath) return load_slow();

    size_t fsize = fs::file_size(filename_);
    if(fsize >= opt_.fastMinSize) return load_fast();
    return load_slow();
}
