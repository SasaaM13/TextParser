#pragma once
#include <string_view>
#include <vector>
#include <cctype>
#include <string>

// ==========================
//   Date structure
// ==========================
struct Date {
    int year;
    int month;
    int day;
};

// ==========================
//   Fast helpers
// ==========================
inline bool parseNum(std::string_view s, int& out)
{
    if(s.empty()) return false;
    int v = 0;
    for(char c : s) {
        if(!std::isdigit((unsigned char)c)) return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

inline bool validDate(int y, int m, int d)
{
    if(m < 1 || m > 12 || d < 1) return false;

    static const int mdays[] =
    { 31,28,31,30,31,30,31,31,30,31,30,31 };

    int maxd = mdays[m - 1];

    if(m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        if(leap) maxd = 29;
    }
    return d <= maxd;
}

// ==========================
//   Month name decoding
// ==========================
inline int monthFromString(std::string_view s)
{
    std::string m;
    for(char c : s)
        if(std::isalpha((unsigned char)c))
            m.push_back((char)std::tolower((unsigned char)c));

    static const struct { const char* n; int v; } map[] = {

        {"jan",1}, {"januar",1}, {"january",1},

        {"feb",2}, {"februar",2}, {"february",2},

        {"mar",3}, {"mart",3}, {"march",3},

        {"apr",4}, {"april",4},

        {"may",5}, {"maj",5},

        {"jun",6}, {"june",6},

        {"jul",7}, {"july",7},

        {"aug",8}, {"avg",8}, {"avgust",8}, {"august",8},

        {"sep",9}, {"sept",9}, {"septembar",9}, {"september",9},

        {"oct",10}, {"okt",10}, {"oktobar",10}, {"october",10},

        {"nov",11}, {"novembar",11}, {"november",11},

        {"dec",12}, {"decembar",12}, {"december",12}
    };

    for(const auto& e : map)
        if(m == e.n) return e.v;

    return 0;
}

// ==========================
//   Main date parser
// ==========================
inline bool parseDate(std::string_view s, Date& out)
{
    std::vector<std::string_view> parts;
    size_t i = 0;

    while(i < s.size()) {
        size_t j = i;
        while(j < s.size() && std::isalnum((unsigned char)s[j])) j++;
        if(j > i) parts.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }

    if(parts.size() < 3) return false;

    int a = 0, b = 0, c = 0;

    // --------------------------
    // All numeric forms
    // --------------------------
    if(parseNum(parts[0], a) && parseNum(parts[1], b) && parseNum(parts[2], c)) {

        // YYYY-MM-DD
        if(a > 31 && validDate(a, b, c)) {
            out = { a,b,c }; return true;
        }

        // DD-MM-YYYY
        if(c > 31 && validDate(c, b, a)) {
            out = { c,b,a }; return true;
        }

        // MM-DD-YYYY (US)
        if(c > 31 && validDate(c, a, b)) {
            out = { c,a,b }; return true;
        }

        return false;
    }

    // --------------------------
    // D Month Y
    // --------------------------
    int m = monthFromString(parts[1]);
    if(m && parseNum(parts[0], a) && parseNum(parts[2], c)) {
        if(validDate(c, m, a)) {
            out = { c,m,a }; return true;
        }
    }

    // --------------------------
    // Month D Y
    // --------------------------
    m = monthFromString(parts[0]);
    if(m && parseNum(parts[1], a) && parseNum(parts[2], c)) {
        if(validDate(c, m, a)) {
            out = { c,m,a }; return true;
        }
    }

    // --------------------------
    // Y Month D
    // --------------------------
    m = monthFromString(parts[1]);
    if(m && parseNum(parts[0], a) && parseNum(parts[2], b)) {
        if(validDate(a, m, b)) {
            out = { a,m,b }; return true;
        }
    }

    return false;
}
