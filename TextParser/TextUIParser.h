#pragma once
#include <windows.h>
#include <string>

class TextParserUI
{
public:
    static int Run(HINSTANCE hInst, int nCmdShow);
private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static std::string OpenFileDialog(HWND hwnd);
    static void ParseFile(const std::string& filepath);
public:
    static HWND hEditOutput;
};
