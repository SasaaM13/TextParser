#pragma once
#include <windows.h>
#include <string>

class TextParserUI
{
public:
    // Pokreće glavni UI prozor (Win32)
    static int Run(HINSTANCE hInst, int nCmdShow);

private:
    // Win32 callback za poruke prozora
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Pomoćne funkcije
    static std::string OpenFileDialog(HWND hwnd);
    static void ParseFile(const std::string& filepath);
    static void SetOutput(const std::string& text);

    // UI elementi
public:
    static HWND hEditOutput;
};
