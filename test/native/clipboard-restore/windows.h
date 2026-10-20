#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

using UINT = unsigned int;
using DWORD = std::uint32_t;
using BYTE = unsigned char;
using SIZE_T = std::size_t;
using BOOL = int;
using HANDLE = void *;
using HWND = void *;
using HGLOBAL = void *;
using HENHMETAFILE = void *;
using ULONG_PTR = std::uintptr_t;
constexpr int VK_CONTROL = 0x11;
constexpr UINT CF_TEXT = 1, CF_BITMAP = 2, CF_METAFILEPICT = 3, CF_OEMTEXT = 7, CF_DIB = 8,
               CF_PALETTE = 9, CF_UNICODETEXT = 13, CF_ENHMETAFILE = 14, CF_LOCALE = 16,
               CF_OWNERDISPLAY = 0x80, CF_DSPTEXT = 0x81, CF_DSPBITMAP = 0x82,
               CF_DSPMETAFILEPICT = 0x83, CF_DSPENHMETAFILE = 0x8e,
               CF_PRIVATEFIRST = 0x200, CF_PRIVATELAST = 0x2ff,
               CF_GDIOBJFIRST = 0x300, CF_GDIOBJLAST = 0x3ff;
constexpr UINT GMEM_MOVEABLE = 2, CP_ACP = 0;
BOOL OpenClipboard(HWND);
BOOL CloseClipboard();
BOOL EmptyClipboard();
HANDLE GetClipboardData(UINT);
HANDLE SetClipboardData(UINT, HANDLE);
int CountClipboardFormats();
UINT EnumClipboardFormats(UINT);
HGLOBAL GlobalAlloc(UINT, SIZE_T);
HGLOBAL GlobalFree(HGLOBAL);
SIZE_T GlobalSize(HGLOBAL);
void *GlobalLock(HGLOBAL);
BOOL GlobalUnlock(HGLOBAL);
UINT GetEnhMetaFileBits(HENHMETAFILE, UINT, BYTE *);
HENHMETAFILE SetEnhMetaFileBits(UINT, const BYTE *);
BOOL DeleteEnhMetaFile(HENHMETAFILE);
int MultiByteToWideChar(UINT, DWORD, const char *, int, wchar_t *, int);
DWORD GetClipboardSequenceNumber();
short GetAsyncKeyState(int);
void Sleep(DWORD);
using WORD = unsigned short;
constexpr int VK_SHIFT = 0x10, VK_MENU = 0x12, VK_INSERT = 0x2d, VK_DELETE = 0x2e;
constexpr int VK_LCONTROL = 0xa2, VK_RCONTROL = 0xa3, VK_LSHIFT = 0xa0, VK_RSHIFT = 0xa1;
constexpr int VK_LMENU = 0xa4, VK_RMENU = 0xa5, VK_LWIN = 0x5b, VK_RWIN = 0x5c;
constexpr DWORD LLKHF_INJECTED = 0x10, KEYEVENTF_KEYUP = 2;
constexpr UINT INPUT_KEYBOARD = 1;
struct KEYBDINPUT { WORD wVk = 0, wScan = 0; DWORD dwFlags = 0, time = 0; ULONG_PTR dwExtraInfo = 0; };
struct INPUT { DWORD type = 0; KEYBDINPUT ki; };
UINT SendInput(UINT, INPUT *, int);
