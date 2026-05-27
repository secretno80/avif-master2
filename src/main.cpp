#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>
#include <wincodec.h>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

const wchar_t* MUTEX_NAME = L"Global\\AVIFMaster2_Mutex";
const wchar_t* WND_CLASS = L"AVIFMaster2_Class";
const int ID_LISTVIEW = 1001;
const int ID_BTN_CONVERT = 1002;
const int ID_CMB_QUALITY = 1003;
const int ID_CMB_HW = 1004;
const int ID_EDT_JOBS = 1005;
const int ID_EDT_THREADS = 1006;
const int ID_CHK_SIMD = 1007;
const int ID_BTN_CHECKALL = 1008;
const int ID_BTN_UNCHECKALL = 1009;
const int ID_BTN_DEL = 1010;
const int ID_CHK_DEL_ORIG = 1011;
const int ID_CHK_AUTO_CLOSE = 1012;
const int ID_BTN_SAVE_SETTINGS = 1013;
const int ID_BTN_DEL_COMPLETED = 1014;
const int ID_BTN_DEL_FAILED = 1015;
const int ID_PROGRESS = 1016;
const int ID_CHK_CUSTOM_OUTPUT = 1017;
const int ID_EDT_OUTPUT_DIR = 1018;
const int ID_BTN_OUTPUT_BROWSE = 1019;
const int ID_REGISTER_PANEL = 1020;
const int ID_REGISTER_TEXT = 1021;
const int ID_REGISTER_PROGRESS = 1022;
const int ID_CHK_KEEP_CREATION_TIME = 1023;
const int ID_CMB_LANG = 1024;

struct FileItem {
    std::wstring path;
    std::wstring name;
    std::wstring type;
    std::wstring status;
    std::wstring origSize;
    std::wstring resultSize;
    std::wstring ratio;
    std::wstring elapsedTime;
    std::wstring logPath;
    std::wstring tempWorkDir;
    uintmax_t sourceSizeBytes = 0;
    bool checked = true;
};

std::vector<FileItem> g_items;
std::mutex g_itemsMutex;
HWND hListView;
HWND g_hProgress = NULL;
HWND g_hwnd = NULL;
HWND g_hRegisterPanel = NULL;
HWND g_hRegisterText = NULL;
HWND g_hRegisterProgress = NULL;
bool g_autoConvert = false;
std::atomic<bool> g_isConverting(false);
std::atomic<bool> g_stopRequested(false);

// Options
int g_quality = 1; // 0: Fast, 1: Normal, 2: High
int g_hwMode = 0; // 0: CPU, 1: CPU+GPU
int g_concurrentJobs = 2;
int g_threadsPerJob = 4;
bool g_simd = true;
bool g_delOrig = false;
bool g_autoClose = false;
bool g_useCustomOutputDir = false;
bool g_keepModifiedTime = false;
std::wstring g_customOutputDir;
std::wstring g_uiLanguage = L"korean";
HFONT g_hFont = NULL;

const wchar_t* REG_KEY = L"Software\\AVIFMaster2";
const wchar_t* TEMP_ARTIFACT_PREFIX = L"AVIFMaster_";
const int TEMP_CLEANUP_STALE_MINUTES = 15;
const UINT WM_APP_CONVERT_FINISHED = WM_APP + 1;
const UINT WM_APP_PROGRESS_UPDATE = WM_APP + 2;
const UINT WM_APP_DELETE_STATUS = WM_APP + 3;
const UINT WM_APP_REGISTER_PROGRESS = WM_APP + 4;
const UINT WM_APP_REGISTER_APPEND_BATCH = WM_APP + 5;
const UINT WM_APP_REGISTER_FINISHED = WM_APP + 6;

std::atomic<unsigned long long> g_totalBytes(0);
std::atomic<unsigned long long> g_processedBytes(0);
std::atomic<bool> g_closeAfterCurrentRun(false);
std::atomic<bool> g_pendingDeleteCompleted(false);
std::atomic<bool> g_pendingDeleteFailed(false);
std::atomic<bool> g_isRegistering(false);
std::atomic<bool> g_pendingAutoConvert(false);
std::atomic<bool> g_pendingAutoClose(false);
std::mutex g_registerMutex;
std::vector<std::wstring> g_pendingRegisterRoots;
std::vector<std::wstring> g_startupRegisterRoots;

const size_t REGISTER_BATCH_SIZE = 200;
const unsigned long long REGISTER_PROGRESS_INTERVAL = 200ULL;

struct RegisterProgressPayload {
    bool scanning = false;
    bool marquee = true;
    unsigned long long processed = 0;
    unsigned long long total = 0;
};

void WriteLogLineW(HANDLE hLogFile, const std::wstring& line);

bool IsEnglishUiLanguage() {
    return _wcsicmp(g_uiLanguage.c_str(), L"english") == 0;
}

const wchar_t* UiText(const wchar_t* ko, const wchar_t* en) {
    return IsEnglishUiLanguage() ? en : ko;
}

void LoadUiLanguagePreference() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    wchar_t langBuf[32] = { 0 };
    DWORD langType = REG_SZ;
    DWORD langSize = sizeof(langBuf);
    if (RegQueryValueExW(hKey, L"UILanguage", NULL, &langType, (BYTE*)langBuf, &langSize) == ERROR_SUCCESS) {
        if (_wcsicmp(langBuf, L"english") == 0) {
            g_uiLanguage = L"english";
        } else {
            g_uiLanguage = L"korean";
        }
    }
    RegCloseKey(hKey);
}

bool IsAvifMasterTempArtifactName(const std::wstring& name) {
    return name.rfind(TEMP_ARTIFACT_PREFIX, 0) == 0;
}

bool IsStaleTempArtifact(const fs::directory_entry& entry, std::chrono::minutes threshold) {
    std::error_code ec;
    const auto writeTime = entry.last_write_time(ec);
    if (ec) {
        return false;
    }

    const auto now = fs::file_time_type::clock::now();
    return (now - writeTime) > threshold;
}

void CleanupStaleTempArtifacts(const fs::path& tempRoot) {
    std::error_code ec;
    if (!fs::exists(tempRoot, ec) || ec) {
        return;
    }

    const auto staleThreshold = std::chrono::minutes(TEMP_CLEANUP_STALE_MINUTES);
    for (const auto& entry : fs::directory_iterator(tempRoot, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }

        const std::wstring filename = entry.path().filename().wstring();
        if (!IsAvifMasterTempArtifactName(filename)) {
            continue;
        }
        if (!IsStaleTempArtifact(entry, staleThreshold)) {
            continue;
        }

        std::error_code removeEc;
        if (entry.is_directory(removeEc) && !removeEc) {
            fs::remove_all(entry.path(), removeEc);
        } else {
            removeEc.clear();
            fs::remove(entry.path(), removeEc);
        }
    }
}

bool TryDeleteFileWithRetry(const std::wstring& filePath, int retryCount = 5, DWORD sleepMs = 80) {
    for (int attempt = 0; attempt < retryCount; ++attempt) {
        std::error_code ec;
        const bool removed = fs::remove(filePath, ec);
        if (removed || (!ec && !fs::exists(filePath, ec))) {
            return true;
        }
        Sleep(sleepMs);
    }
    return false;
}

bool TryMoveFileWithFallback(const fs::path& fromPath, const fs::path& toPath, std::error_code& outEc,
    int retryCount = 5, DWORD sleepMs = 80) {
    outEc.clear();

    for (int attempt = 0; attempt < retryCount; ++attempt) {
        std::error_code moveEc;
        fs::rename(fromPath, toPath, moveEc);
        if (!moveEc) {
            outEc.clear();
            return true;
        }

        // UNC/network shares can intermittently fail with std::filesystem::rename.
        if (MoveFileExW(fromPath.c_str(), toPath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            outEc.clear();
            return true;
        }

        outEc = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        Sleep(sleepMs);
    }

    // Last-resort fallback for network filesystems: copy then delete source.
    std::error_code copyEc;
    fs::copy_file(fromPath, toPath, fs::copy_options::none, copyEc);
    if (copyEc) {
        outEc = copyEc;
        return false;
    }

    std::error_code removeEc;
    fs::remove(fromPath, removeEc);
    if (removeEc) {
        std::error_code cleanupEc;
        fs::remove(toPath, cleanupEc);
        outEc = removeEc;
        return false;
    }

    outEc.clear();
    return true;
}

bool TryGetLastWriteTime(const fs::path& path, FILETIME& outWriteTime) {
    outWriteTime = FILETIME{ 0, 0 };
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        flags = FILE_ATTRIBUTE_NORMAL;
    }

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, flags, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    FILETIME accessTime = { 0, 0 };
    FILETIME writeTime = { 0, 0 };
    const BOOL ok = GetFileTime(hFile, NULL, &accessTime, &outWriteTime);
    CloseHandle(hFile);
    return ok == TRUE;
}

bool TrySetLastWriteTime(const fs::path& path, const FILETIME& writeTime) {
    HANDLE hFile = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    const BOOL ok = SetFileTime(hFile, NULL, NULL, &writeTime);
    CloseHandle(hFile);
    return ok == TRUE;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool ConvertWebPToPngWithWic(const fs::path& webpPath, const fs::path& pngPath, HANDLE hLogFile) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        WriteLogLineW(hLogFile, L"[WEBP] CoInitializeEx failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* srcFrame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* dstFrame = nullptr;

    bool ok = false;
    do {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] WIC factory creation failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = factory->CreateDecoderFromFilename(
            webpPath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Decoder open failed: " + webpPath.wstring() +
                L" (HRESULT=" + std::to_wstring(static_cast<unsigned long>(hr)) + L")");
            break;
        }

        hr = decoder->GetFrame(0, &srcFrame);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Decoder frame read failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Format converter creation failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = converter->Initialize(
            srcFrame,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Pixel format conversion failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Output stream creation failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Output stream open failed: " + pngPath.wstring() +
                L" (HRESULT=" + std::to_wstring(static_cast<unsigned long>(hr)) + L")");
            break;
        }

        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG encoder creation failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG encoder initialize failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = encoder->CreateNewFrame(&dstFrame, nullptr);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG frame creation failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = dstFrame->Initialize(nullptr);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG frame initialize failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        UINT width = 0;
        UINT height = 0;
        hr = srcFrame->GetSize(&width, &height);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] Source size read failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = dstFrame->SetSize(width, height);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG frame size set failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = dstFrame->SetPixelFormat(&format);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG frame format set failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = dstFrame->WriteSource(converter, nullptr);
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG write source failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = dstFrame->Commit();
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG frame commit failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        hr = encoder->Commit();
        if (FAILED(hr)) {
            WriteLogLineW(hLogFile, L"[WEBP] PNG encoder commit failed: " + std::to_wstring(static_cast<unsigned long>(hr)));
            break;
        }

        ok = true;
    } while (false);

    SafeRelease(dstFrame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(converter);
    SafeRelease(srcFrame);
    SafeRelease(decoder);
    SafeRelease(factory);

    if (shouldUninit) {
        CoUninitialize();
    }

    if (ok) {
        WriteLogLineW(hLogFile, L"[WEBP] Converted to PNG: " + webpPath.wstring() + L" -> " + pngPath.wstring());
    }
    return ok;
}

void SetConvertButtonText(HWND hwnd, const wchar_t* text) {
    HWND hBtnConvert = GetDlgItem(hwnd, ID_BTN_CONVERT);
    if (hBtnConvert) {
        SetWindowTextW(hBtnConvert, text);
    }
}

bool IsTerminalStatus(const std::wstring& status) {
    return status.find(L"완료") != std::wstring::npos ||
           status.find(L"실패") != std::wstring::npos ||
           status.find(L"사용자 중지") != std::wstring::npos;
}

void MarkActiveItemsAsUserStopped() {
    std::lock_guard<std::mutex> lock(g_itemsMutex);
    for (auto& item : g_items) {
        if (!IsTerminalStatus(item.status)) {
            item.status = L"사용자 중지";
        }
    }
}

void CleanupActiveTempWorkDirs() {
    std::vector<std::wstring> tempDirs;
    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        for (auto& item : g_items) {
            if (!item.tempWorkDir.empty()) {
                tempDirs.push_back(item.tempWorkDir);
                item.tempWorkDir.clear();
            }
        }
    }

    for (const auto& dir : tempDirs) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

void NotifyProgressChanged() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_PROGRESS_UPDATE, 0, 0);
    }
}

void AddProcessedBytes(uintmax_t bytes) {
    g_processedBytes.fetch_add(static_cast<unsigned long long>(bytes), std::memory_order_relaxed);
    NotifyProgressChanged();
}

void UpdateOutputDirUiState(HWND hwnd) {
    HWND hChk = GetDlgItem(hwnd, ID_CHK_CUSTOM_OUTPUT);
    HWND hEdt = GetDlgItem(hwnd, ID_EDT_OUTPUT_DIR);
    HWND hBtn = GetDlgItem(hwnd, ID_BTN_OUTPUT_BROWSE);
    if (!hChk || !hEdt || !hBtn) {
        return;
    }

    const bool useCustom = SendMessageW(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(hEdt, useCustom ? TRUE : FALSE);
    EnableWindow(hBtn, useCustom ? TRUE : FALSE);
}

bool BrowseForFolder(HWND owner, std::wstring& selectedPath) {
    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = owner;
    bi.lpszTitle = UiText(L"출력 폴더를 선택하세요", L"Select output folder");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return false;
    }

    wchar_t pathBuf[MAX_PATH] = { 0 };
    const BOOL ok = SHGetPathFromIDListW(pidl, pathBuf);
    CoTaskMemFree(pidl);
    if (!ok || pathBuf[0] == L'\0') {
        return false;
    }

    selectedPath = pathBuf;
    return true;
}

void CleanupTrackedLogFiles() {
    std::vector<std::wstring> logs;
    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        for (auto& item : g_items) {
            if (!item.logPath.empty()) {
                logs.push_back(item.logPath);
                item.logPath.clear();
            }
        }
    }

    for (const auto& log : logs) {
        TryDeleteFileWithRetry(log, 3, 50);
    }
}

void TerminateChildProcessesOfCurrentProcess() {
    const DWORD selfPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snapshot, &pe)) {
        CloseHandle(snapshot);
        return;
    }

    do {
        if (pe.th32ParentProcessID != selfPid) {
            continue;
        }

        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (hProc) {
            TerminateProcess(hProc, ERROR_CANCELLED);
            CloseHandle(hProc);
        }
    } while (Process32NextW(snapshot, &pe));

    CloseHandle(snapshot);
}

uintmax_t GetItemSourceSizeBytes(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return 0;
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        return 0;
    }
    ec.clear();
    uintmax_t size = fs::file_size(path, ec);
    if (ec || size == static_cast<uintmax_t>(-1)) {
        return 0;
    }
    return size;
}

void PumpPendingUiMessages() {
    MSG msg = { 0 };
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void LayoutRegisterOverlay(HWND hwnd) {
    if (!g_hRegisterPanel || !g_hRegisterText || !g_hRegisterProgress) {
        return;
    }

    RECT rc = { 0 };
    GetClientRect(hwnd, &rc);

    const int panelW = 380;
    const int panelH = 110;
    int x = (rc.right - panelW) / 2;
    int y = (rc.bottom - panelH) / 2;

    POINT pt = { x, y };
    ClientToScreen(hwnd, &pt);

    MoveWindow(g_hRegisterPanel, pt.x, pt.y, panelW, panelH, TRUE);
    MoveWindow(g_hRegisterText, 20, 20, panelW - 40, 24, TRUE);
    MoveWindow(g_hRegisterProgress, 20, 58, panelW - 40, 20, TRUE);
}

void ShowRegisterOverlay(HWND hwnd, const std::wstring& text, bool marquee, int pos = -1) {
    if (!g_hRegisterPanel || !g_hRegisterText || !g_hRegisterProgress) {
        return;
    }

    if (hwnd) {
        LayoutRegisterOverlay(hwnd);
    }
    SetWindowTextW(g_hRegisterText, text.c_str());

    if (marquee) {
        SendMessageW(g_hRegisterProgress, PBM_SETMARQUEE, TRUE, 20);
    } else {
        SendMessageW(g_hRegisterProgress, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(g_hRegisterProgress, PBM_SETRANGE32, 0, 1000);
        if (pos >= 0) {
            SendMessageW(g_hRegisterProgress, PBM_SETPOS, pos, 0);
        }
    }

    ShowWindow(g_hRegisterPanel, SW_SHOWNA);
    ShowWindow(g_hRegisterText, SW_SHOW);
    ShowWindow(g_hRegisterProgress, SW_SHOW);
    SetWindowPos(g_hRegisterPanel, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BringWindowToTop(g_hRegisterPanel);
    UpdateWindow(g_hRegisterPanel);
    if (hwnd) {
        UpdateWindow(hwnd);
    }
}

void HideRegisterOverlay() {
    if (!g_hRegisterPanel || !g_hRegisterProgress) {
        return;
    }
    SendMessageW(g_hRegisterProgress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(g_hRegisterPanel, SW_HIDE);
}

void UpdateProgressBarUI() {
    if (!g_hProgress) {
        return;
    }

    const unsigned long long total = g_totalBytes.load(std::memory_order_relaxed);
    const unsigned long long processed = g_processedBytes.load(std::memory_order_relaxed);
    int pos = 0;
    if (total > 0) {
        const unsigned long long clamped = (processed > total) ? total : processed;
        pos = static_cast<int>((clamped * 1000ULL) / total);
    }
    SendMessageW(g_hProgress, PBM_SETPOS, pos, 0);
}

size_t DeleteItemsByResultStatus(bool removeCompleted, bool removeFailed) {
    std::lock_guard<std::mutex> lock(g_itemsMutex);
    std::vector<FileItem> filteredItems;
    filteredItems.reserve(g_items.size());

    for (const auto& item : g_items) {
        const bool isCompleted = item.status.find(L"완료") != std::wstring::npos;
        const bool isFailed = item.status.find(L"실패") != std::wstring::npos;

        if ((removeCompleted && isCompleted) || (removeFailed && isFailed)) {
            continue;
        }
        filteredItems.push_back(item);
    }
    g_items = std::move(filteredItems);
    return g_items.size();
}

void SaveSettings(HWND hwnd) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;
    DWORD val;
    val = (DWORD)SendMessageW(GetDlgItem(hwnd, ID_CMB_QUALITY), CB_GETCURSEL, 0, 0);
    RegSetValueExW(hKey, L"Quality", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    val = (DWORD)SendMessageW(GetDlgItem(hwnd, ID_CMB_HW), CB_GETCURSEL, 0, 0);
    RegSetValueExW(hKey, L"HwMode", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    wchar_t buf[32];
    GetWindowTextW(GetDlgItem(hwnd, ID_EDT_JOBS), buf, 32);
    val = (DWORD)_wtoi(buf);
    RegSetValueExW(hKey, L"Jobs", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    GetWindowTextW(GetDlgItem(hwnd, ID_EDT_THREADS), buf, 32);
    val = (DWORD)_wtoi(buf);
    RegSetValueExW(hKey, L"Threads", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    val = (SendMessageW(GetDlgItem(hwnd, ID_CHK_SIMD), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    RegSetValueExW(hKey, L"Simd", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    val = (SendMessageW(GetDlgItem(hwnd, ID_CHK_DEL_ORIG), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    RegSetValueExW(hKey, L"DelOrig", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    val = (SendMessageW(GetDlgItem(hwnd, ID_CHK_AUTO_CLOSE), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    RegSetValueExW(hKey, L"AutoClose", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
    val = (SendMessageW(GetDlgItem(hwnd, ID_CHK_KEEP_CREATION_TIME), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    RegSetValueExW(hKey, L"KeepModifiedTime", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));

    val = (SendMessageW(GetDlgItem(hwnd, ID_CHK_CUSTOM_OUTPUT), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    RegSetValueExW(hKey, L"UseCustomOutput", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));

    wchar_t pathBuf[MAX_PATH] = { 0 };
    GetWindowTextW(GetDlgItem(hwnd, ID_EDT_OUTPUT_DIR), pathBuf, MAX_PATH);
    RegSetValueExW(hKey, L"CustomOutputDir", 0, REG_SZ, (const BYTE*)pathBuf,
        (DWORD)((wcslen(pathBuf) + 1) * sizeof(wchar_t)));

    HWND hLang = GetDlgItem(hwnd, ID_CMB_LANG);
    if (hLang) {
        const int langSel = (int)SendMessageW(hLang, CB_GETCURSEL, 0, 0);
        const wchar_t* langValue = (langSel == 1) ? L"english" : L"korean";
        RegSetValueExW(hKey, L"UILanguage", 0, REG_SZ, (const BYTE*)langValue,
            (DWORD)((wcslen(langValue) + 1) * sizeof(wchar_t)));
        g_uiLanguage = langValue;
    }
    RegCloseKey(hKey);
}

void LoadSettings(HWND hwnd) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    DWORD val = 0, sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"Quality", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        SendMessageW(GetDlgItem(hwnd, ID_CMB_QUALITY), CB_SETCURSEL, val, 0);
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"HwMode", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        SendMessageW(GetDlgItem(hwnd, ID_CMB_HW), CB_SETCURSEL, val, 0);
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"Jobs", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS && val > 0)
        SetWindowTextW(GetDlgItem(hwnd, ID_EDT_JOBS), std::to_wstring(val).c_str());
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"Threads", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS && val > 0)
        SetWindowTextW(GetDlgItem(hwnd, ID_EDT_THREADS), std::to_wstring(val).c_str());
    sz = sizeof(DWORD); val = 1;
    if (RegQueryValueExW(hKey, L"Simd", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        SendMessageW(GetDlgItem(hwnd, ID_CHK_SIMD), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"DelOrig", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        SendMessageW(GetDlgItem(hwnd, ID_CHK_DEL_ORIG), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"AutoClose", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        SendMessageW(GetDlgItem(hwnd, ID_CHK_AUTO_CLOSE), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"KeepModifiedTime", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
        SendMessageW(GetDlgItem(hwnd, ID_CHK_KEEP_CREATION_TIME), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
        sz = sizeof(DWORD); val = 0;
        if (RegQueryValueExW(hKey, L"KeepCreationTime", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
            SendMessageW(GetDlgItem(hwnd, ID_CHK_KEEP_CREATION_TIME), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    sz = sizeof(DWORD); val = 0;
    if (RegQueryValueExW(hKey, L"UseCustomOutput", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
        SendMessageW(GetDlgItem(hwnd, ID_CHK_CUSTOM_OUTPUT), BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    wchar_t outDir[MAX_PATH] = { 0 };
    DWORD outDirSize = sizeof(outDir);
    DWORD outDirType = REG_SZ;
    if (RegQueryValueExW(hKey, L"CustomOutputDir", NULL, &outDirType, (BYTE*)outDir, &outDirSize) == ERROR_SUCCESS && outDir[0] != L'\0') {
        SetWindowTextW(GetDlgItem(hwnd, ID_EDT_OUTPUT_DIR), outDir);
    }

    HWND hLang = GetDlgItem(hwnd, ID_CMB_LANG);
    if (hLang) {
        SendMessageW(hLang, CB_SETCURSEL, IsEnglishUiLanguage() ? 1 : 0, 0);
    }
    RegCloseKey(hKey);
}

std::wstring FormatSize(uintmax_t bytes) {
    double size = bytes;
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    int i = 0;
    while (size >= 1024 && i < 3) {
        size /= 1024;
        i++;
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f %s", size, units[i]);
    return std::wstring(buf);
}

std::wstring ToLowerString(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value;
}

bool IsSupportedInputFile(const fs::path& fp) {
    const std::wstring ext = ToLowerString(fp.extension().wstring());
    const std::wstring nameLower = ToLowerString(fp.filename().wstring());

    if (nameLower.find(L"avif") != std::wstring::npos) {
        return false;
    }

    static const wchar_t* kArchiveExts[] = {
        L".zip", L".cbz", L".7z", L".rar", L".cbr", L".tar", L".cbt",
        L".gz", L".tgz", L".bz2", L".tbz", L".tbz2", L".xz", L".txz"
    };

    for (const auto* archiveExt : kArchiveExts) {
        if (ext == archiveExt) {
            return true;
        }
    }

    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".y4m" ||
           ext == L".webp";
}

bool IsArchiveInputFile(const fs::path& fp) {
    const std::wstring ext = ToLowerString(fp.extension().wstring());
    return ext == L".zip" || ext == L".cbz" || ext == L".7z" || ext == L".rar" ||
           ext == L".cbr" || ext == L".tar" || ext == L".cbt" || ext == L".gz" ||
           ext == L".tgz" || ext == L".bz2" || ext == L".tbz" || ext == L".tbz2" ||
           ext == L".xz" || ext == L".txz";
}

bool ContainsSpecialPathCharsForExternalTool(const std::wstring& path) {
    static const std::wstring specialChars = L"[]!*?()&^`;,=";
    for (wchar_t c : path) {
        if (c > 127 || specialChars.find(c) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

void WriteLogLineW(HANDLE hLogFile, const std::wstring& line) {
    if (hLogFile == INVALID_HANDLE_VALUE) {
        return;
    }

    std::wstring logLine = line;
    if (logLine.empty() || logLine.back() != L'\n') {
        logLine += L"\r\n";
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, logLine.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) {
        return;
    }

    std::vector<char> utf8Buf(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, logLine.c_str(), -1, utf8Buf.data(), utf8Len, NULL, NULL);
    DWORD written = 0;
    WriteFile(hLogFile, utf8Buf.data(), utf8Len - 1, &written, NULL);
}

std::wstring EscapePowerShellSingleQuoted(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t c : value) {
        if (c == L'\'') {
            out += L"''";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring BuildLogPath(const fs::path& inputPath, const fs::path& outputPath) {
    std::error_code ec;
    if (fs::is_directory(inputPath, ec) && !ec) {
        return (inputPath / L"AVIFMaster.log").wstring();
    }

    fs::path logDir = inputPath.parent_path();
    std::wstring baseName = inputPath.stem().wstring();
    if (baseName.empty()) {
        baseName = outputPath.stem().wstring();
    }
    return (logDir / (baseName + L".avifmaster.log")).wstring();
}

std::wstring ResolveAvifencExecutablePath() {
    wchar_t modulePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) == 0) {
        return L"lib\\avifenc.exe";
    }

    fs::path moduleDir(modulePath);
    moduleDir = moduleDir.parent_path();

    std::vector<fs::path> candidates;
    candidates.push_back(moduleDir / L"lib" / L"avifenc.exe");
    candidates.push_back(moduleDir.parent_path() / L"lib" / L"avifenc.exe");
    candidates.push_back(moduleDir.parent_path().parent_path() / L"lib" / L"avifenc.exe");
    candidates.push_back(moduleDir / L"avifenc.exe");

    std::error_code cwdEc;
    fs::path cwd = fs::current_path(cwdEc);
    if (!cwdEc) {
        candidates.push_back(cwd / L"lib" / L"avifenc.exe");
    }

    std::error_code ec;
    for (const auto& candidate : candidates) {
        ec.clear();
        if (fs::exists(candidate, ec) && !ec) {
            return candidate.wstring();
        }
    }

    return (moduleDir.parent_path() / L"lib" / L"avifenc.exe").wstring();
}

bool BuildOutputPathForItem(const fs::path& inputPath, bool isArchive, bool isDir, fs::path& outputPath) {
    fs::path baseOutput;
    if (g_useCustomOutputDir && !g_customOutputDir.empty()) {
        std::error_code ec;
        fs::create_directories(g_customOutputDir, ec);
        if (ec) {
            return false;
        }

        std::wstring baseName;
        if (isDir) {
            baseName = inputPath.filename().wstring();
            if (baseName.empty()) {
                baseName = inputPath.stem().wstring();
            }
        } else {
            baseName = inputPath.stem().wstring();
        }
        if (baseName.empty()) {
            baseName = L"output";
        }
        baseOutput = fs::path(g_customOutputDir) / baseName;
    } else {
        baseOutput = inputPath;
    }

    if (isArchive || isDir) {
        outputPath = baseOutput;
        outputPath.replace_extension(L".avif.zip");
    } else {
        outputPath = baseOutput;
        outputPath.replace_extension(L".avif");
    }
    return true;
}

std::wstring FindExistingLogPath(const FileItem& item) {
    std::error_code ec;

    if (!item.logPath.empty() && fs::exists(item.logPath, ec) && !ec) {
        return item.logPath;
    }

    fs::path targetDir(item.path);
    ec.clear();
    if (!fs::is_directory(targetDir, ec) || ec) {
        targetDir = targetDir.parent_path();
    }
    if (targetDir.empty() || !fs::exists(targetDir, ec) || ec) {
        return L"";
    }

    fs::path newestLog;
    fs::file_time_type newestTime;
    bool found = false;

    for (const auto& entry : fs::directory_iterator(targetDir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code itemEc;
        if (!entry.is_regular_file(itemEc) || itemEc) continue;

        const std::wstring ext = ToLowerString(entry.path().extension().wstring());
        if (ext != L".log") continue;

        const auto writeTime = entry.last_write_time(itemEc);
        if (itemEc) continue;

        if (!found || writeTime > newestTime) {
            newestTime = writeTime;
            newestLog = entry.path();
            found = true;
        }
    }

    return found ? newestLog.wstring() : L"";
}

void OpenFailureLogForItem(HWND hwnd, const FileItem& item) {
    std::wstring logToOpen = FindExistingLogPath(item);
    if (logToOpen.empty()) {
        MessageBoxW(hwnd,
            UiText(L"열 수 있는 로그 파일을 찾지 못했습니다.", L"No log file could be found."),
            UiText(L"로그 없음", L"No Log"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    HINSTANCE result = ShellExecuteW(hwnd, L"open", logToOpen.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        MessageBoxW(hwnd,
            UiText(L"로그 파일을 열지 못했습니다.", L"Failed to open log file."),
            UiText(L"오류", L"Error"), MB_OK | MB_ICONERROR);
    }
}

std::wstring NormalizeInputPath(const std::wstring& pathStr) {
    std::wstring cleanPath = pathStr;
    while (!cleanPath.empty() && (cleanPath.back() == L'\\' || cleanPath.back() == L'/' || cleanPath.back() == L' ')) {
        // Preserve UNC root (e.g. \\server\share)
        if (cleanPath.size() <= 3) break;
        // Preserve trailing backslash on drive roots like C:\
        if (cleanPath.size() == 3 && cleanPath[1] == L':') break;
        cleanPath.pop_back();
    }
    return cleanPath;
}

std::wstring BuildRegisterSpoolPath() {
    wchar_t tempPath[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"AVIFMaster_register_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64()) + L".txt";
}

void PostRegisterProgress(bool scanning, bool marquee, unsigned long long processed, unsigned long long total) {
    if (!g_hwnd) {
        return;
    }

    RegisterProgressPayload* payload = new RegisterProgressPayload();
    payload->scanning = scanning;
    payload->marquee = marquee;
    payload->processed = processed;
    payload->total = total;

    if (!PostMessageW(g_hwnd, WM_APP_REGISTER_PROGRESS, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void PostRegisterBatch(std::vector<FileItem>& batch) {
    if (!g_hwnd || batch.empty()) {
        return;
    }

    std::vector<FileItem>* payload = new std::vector<FileItem>();
    payload->swap(batch);
    if (!PostMessageW(g_hwnd, WM_APP_REGISTER_APPEND_BATCH, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void RunRegisterWorker() {
    while (true) {
        std::vector<std::wstring> roots;
        {
            std::lock_guard<std::mutex> lock(g_registerMutex);
            if (g_pendingRegisterRoots.empty()) {
                g_isRegistering.store(false);
                if (g_hwnd) {
                    PostMessageW(g_hwnd, WM_APP_REGISTER_FINISHED, 0, 0);
                }
                return;
            }
            roots.swap(g_pendingRegisterRoots);
        }

        const std::wstring spoolPath = BuildRegisterSpoolPath();
        std::wofstream ofs(spoolPath.c_str(), std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            continue;
        }

        PostRegisterProgress(true, true, 0, 0);

        unsigned long long discovered = 0;
        for (const auto& rootStr : roots) {
            const std::wstring cleanRoot = NormalizeInputPath(rootStr);
            if (cleanRoot.empty()) {
                continue;
            }

            fs::path rootPath(cleanRoot);
            std::error_code ec;
            if (!fs::exists(rootPath, ec) || ec) {
                continue;
            }

            if (fs::is_directory(rootPath, ec) && !ec) {
                for (const auto& entry : fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, ec)) {
                    if (ec) {
                        break;
                    }
                    std::error_code fileEc;
                    if (fs::is_regular_file(entry.status(fileEc)) && !fileEc) {
                        ofs << entry.path().wstring() << L"\n";
                        discovered++;
                        if ((discovered % 300ULL) == 0ULL) {
                            PostRegisterProgress(true, true, discovered, 0);
                        }
                    }
                }
            } else if (!ec) {
                std::error_code fileEc;
                if (fs::is_regular_file(rootPath, fileEc) && !fileEc) {
                    ofs << rootPath.wstring() << L"\n";
                    discovered++;
                }
            }
        }
        ofs.close();

        std::unordered_set<std::wstring> existingLower;
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            existingLower.reserve(g_items.size() * 2 + 16);
            for (const auto& item : g_items) {
                existingLower.insert(ToLowerString(item.path));
            }
        }

        std::wifstream ifs(spoolPath.c_str(), std::ios::in);
        if (!ifs.is_open()) {
            std::error_code rmEc;
            fs::remove(spoolPath, rmEc);
            continue;
        }

        std::vector<FileItem> batch;
        batch.reserve(REGISTER_BATCH_SIZE);
        unsigned long long processed = 0;
        std::wstring line;

        while (std::getline(ifs, line)) {
            processed++;
            if (line.empty()) {
                if ((processed % REGISTER_PROGRESS_INTERVAL) == 0ULL) {
                    PostRegisterProgress(false, false, processed, discovered);
                }
                continue;
            }

            fs::path fp(line);
            if (!IsSupportedInputFile(fp)) {
                if ((processed % REGISTER_PROGRESS_INTERVAL) == 0ULL) {
                    PostRegisterProgress(false, false, processed, discovered);
                }
                continue;
            }

            const std::wstring fpStr = fp.wstring();
            const std::wstring lower = ToLowerString(fpStr);
            if (existingLower.find(lower) != existingLower.end()) {
                if ((processed % REGISTER_PROGRESS_INTERVAL) == 0ULL) {
                    PostRegisterProgress(false, false, processed, discovered);
                }
                continue;
            }
            existingLower.insert(lower);

            FileItem item;
            item.path = fpStr;
            item.name = fp.filename().wstring();
            item.type = fp.extension().wstring();
            item.status = L"대기";

            std::error_code szEc;
            uintmax_t size = fs::file_size(fp, szEc);
            if (!szEc && size != static_cast<uintmax_t>(-1)) {
                item.origSize = FormatSize(size);
                item.sourceSizeBytes = size;
            }
            batch.push_back(std::move(item));

            if (batch.size() >= REGISTER_BATCH_SIZE) {
                PostRegisterBatch(batch);
            }

            if ((processed % REGISTER_PROGRESS_INTERVAL) == 0ULL) {
                PostRegisterProgress(false, false, processed, discovered);
            }
        }

        if (!batch.empty()) {
            PostRegisterBatch(batch);
        }

        PostRegisterProgress(false, false, discovered, discovered);

        std::error_code rmEc;
        fs::remove(spoolPath, rmEc);
    }
}

void StartAsyncRegister(const std::vector<std::wstring>& roots) {
    if (roots.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_registerMutex);
        for (const auto& root : roots) {
            if (!root.empty()) {
                g_pendingRegisterRoots.push_back(root);
            }
        }
    }

    bool expected = false;
    if (g_isRegistering.compare_exchange_strong(expected, true)) {
        std::thread(RunRegisterWorker).detach();
    }
}

void AddFileItem(const std::wstring& pathStr) {
    // Normalize path: trim trailing whitespace/separators for reliable fs operations
    std::wstring cleanPath = NormalizeInputPath(pathStr);

    fs::path p(cleanPath);
    std::error_code ec;

    // Verify the path actually exists before proceeding
    if (!fs::exists(p, ec) || ec) {
        return;
    }

    std::vector<fs::path> pathsToAdd;
    if (fs::is_directory(p, ec)) {
        size_t scannedEntries = 0;
        for (const auto& entry : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code fec;
            if (fs::is_regular_file(entry.status(fec)) && !fec) {
                pathsToAdd.push_back(entry.path());
            }
            scannedEntries++;
            if ((scannedEntries % 200) == 0) {
                PumpPendingUiMessages();
            }
        }
    } else if (!ec) {
        pathsToAdd.push_back(p);
    }

    if (pathsToAdd.empty()) return;

    std::vector<FileItem> newItems;
    size_t processedPaths = 0;
    for (const auto& fp : pathsToAdd) {
        if (!IsSupportedInputFile(fp)) {
            processedPaths++;
            if ((processedPaths % 200) == 0) {
                PumpPendingUiMessages();
            }
            continue;
        }

        // Deduplicate: skip if the file is already in g_items
        std::wstring fpStr = fp.wstring();
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            bool duplicate = false;
            for (const auto& existing : g_items) {
                if (_wcsicmp(existing.path.c_str(), fpStr.c_str()) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
        }

        FileItem item;
        item.path = fpStr;
        item.name = fp.filename().wstring();
        item.type = fp.extension().wstring();
        item.status = L"대기";

        std::error_code szEc;
        uintmax_t size = fs::file_size(fp, szEc);
        if (!szEc && size != static_cast<uintmax_t>(-1)) {
            item.origSize = FormatSize(size);
            item.sourceSizeBytes = size;
        }
        newItems.push_back(item);

        processedPaths++;
        if ((processedPaths % 200) == 0) {
            PumpPendingUiMessages();
        }
    }

    if (newItems.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items.insert(g_items.end(), newItems.begin(), newItems.end());
    }

    if (hListView) {
        ListView_SetItemCountEx(hListView, g_items.size(), LVSICF_NOINVALIDATEALL);
        InvalidateRect(hListView, NULL, FALSE);
    }
}
bool RunCmdLog(const std::wstring& cmd, HANDLE hLogFile, DWORD* exitCodeOut = nullptr, bool* cancelledOut = nullptr) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (cancelledOut) {
        *cancelledOut = false;
    }
    
    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hLogFile;
        si.hStdError = hLogFile;
        WriteLogLineW(hLogFile, L"Executing: " + cmd);
    }
    
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        bool cancelled = false;
        while (true) {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 200);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (waitResult == WAIT_TIMEOUT) {
                if (g_stopRequested.load()) {
                    cancelled = true;
                    TerminateProcess(pi.hProcess, ERROR_CANCELLED);
                    WaitForSingleObject(pi.hProcess, 3000);
                    break;
                }
                continue;
            }
            break;
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (hLogFile != INVALID_HANDLE_VALUE) {
            WriteLogLineW(hLogFile, L"Command exit code: " + std::to_wstring(exitCode));
        }
        if (exitCodeOut) *exitCodeOut = exitCode;
        if (cancelledOut) *cancelledOut = cancelled;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (cancelled && hLogFile != INVALID_HANDLE_VALUE) {
            WriteLogLineW(hLogFile, L"Execution cancelled by user request");
        }
        return exitCode == 0;
    } else {
        DWORD err = GetLastError();
        if (hLogFile != INVALID_HANDLE_VALUE) {
            WriteLogLineW(hLogFile, L"CreateProcessW failed with error code: " + std::to_wstring(err));
        }
    }
    return false;
}

void ProcessItem(size_t i) {
    auto startTime = std::chrono::steady_clock::now();
    uintmax_t itemBytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        if (i < g_items.size()) {
            itemBytes = g_items[i].sourceSizeBytes;
        }
    }

    if (g_stopRequested.load()) {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].status = L"사용자 중지";
        g_items[i].elapsedTime = L"00:00";
        InvalidateRect(hListView, NULL, FALSE);
        AddProcessedBytes(itemBytes);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].status = L"준비중...";
    }
    InvalidateRect(hListView, NULL, FALSE);
    
    fs::path inputPath(g_items[i].path);
    FILETIME sourceModifiedTime = { 0, 0 };
    bool hasSourceModifiedTime = false;
    if (g_keepModifiedTime) {
        hasSourceModifiedTime = TryGetLastWriteTime(inputPath, sourceModifiedTime);
    }

    // Verify the input path is still accessible (important for UNC/network paths)
    std::error_code existEc;
    if (!fs::exists(inputPath, existEc) || existEc) {
        auto endTime = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].status = L"실패 (경로 없음)";
        g_items[i].elapsedTime = L"00:00";
        InvalidateRect(hListView, NULL, FALSE);
        AddProcessedBytes(itemBytes);
        return;
    }

    if (itemBytes == 0) {
        itemBytes = GetItemSourceSizeBytes(inputPath);
    }

    std::wstring ext = inputPath.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    bool isArchive = IsArchiveInputFile(inputPath);
    std::error_code isDirEc;
    bool isDir = fs::is_directory(inputPath, isDirEc);

    fs::path outputPath;
    if (!BuildOutputPathForItem(inputPath, isArchive, isDir, outputPath)) {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].status = L"실패 (출력폴더)";
        g_items[i].elapsedTime = L"00:00";
        InvalidateRect(hListView, NULL, FALSE);
        AddProcessedBytes(itemBytes);
        return;
    }
    
    std::wstring absExePath = ResolveAvifencExecutablePath();

    std::wstring speed = L"6";
    std::wstring q = L"50";
    if (g_quality == 0) { speed = L"8"; q = L"60"; }
    else if (g_quality == 2) { speed = L"4"; q = L"40"; }

    std::wstring jobs = std::to_wstring(g_threadsPerJob);
    std::wstring advFlags = L" --tilerowslog2 2 --tilecolslog2 3 --yuv 420 --ignore-icc";
    if (g_simd) {
        advFlags += L" -a row-mt=1";
    }

    std::wstring logPath = BuildLogPath(inputPath, outputPath);
    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].logPath = logPath;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hLogFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    std::error_code encEc;
    const bool encoderAvailable = fs::exists(absExePath, encEc) && !encEc;
    if (hLogFile != INVALID_HANDLE_VALUE) {
        WriteLogLineW(hLogFile, L"Resolved avifenc path: " + absExePath);
        if (!encoderAvailable) {
            WriteLogLineW(hLogFile, L"avifenc executable not found.");
        }
    }

    bool success = false;
    std::atomic<bool> cancelledByUser(false);
    std::wstring failureStatus = L"실패";
    bool archiveMarkedAsAvif = false;

    if (!encoderAvailable) {
        failureStatus = L"실패 (avifenc 없음)";
    } else if (isArchive || isDir) {
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            g_items[i].status = isDir ? L"파일복사중..." : L"압축해제중...";
        }
        InvalidateRect(hListView, NULL, FALSE);
        
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring tempDir = std::wstring(tempPath) + L"AVIFMaster_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(i);
        std::error_code ec;
        fs::create_directories(tempDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            g_items[i].tempWorkDir = tempDir;
        }

        bool prepSuccess = false;
        if (isArchive) {
            const std::wstring arcExt = ToLowerString(inputPath.extension().wstring());
            // Always stage archives to a safe temp filename so external tools don't choke on special chars.
            std::wstring srcForTar = inputPath.wstring();
            std::wstring tempSrcZip;

            WriteLogLineW(hLogFile, L"[Stage] Input archive path: " + srcForTar);
            if (ContainsSpecialPathCharsForExternalTool(srcForTar)) {
                WriteLogLineW(hLogFile, L"[Stage] Special characters detected in source path. Using safe temp staging.");
            }

            std::wstring ext2 = inputPath.extension().wstring();
            tempSrcZip = std::wstring(tempPath) + L"AVIFMaster_src_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(i) + ext2;

            WriteLogLineW(hLogFile, L"[Stage] Copy archive -> " + tempSrcZip);
            std::error_code copyEc;
            fs::copy_file(inputPath, tempSrcZip, fs::copy_options::overwrite_existing, copyEc);
            if (!copyEc) {
                srcForTar = tempSrcZip;
                WriteLogLineW(hLogFile, L"[Stage] Archive staging copy success.");
            } else {
                const std::string copyErr = copyEc.message();
                WriteLogLineW(hLogFile, L"[Stage] Archive staging copy failed: " + std::wstring(copyErr.begin(), copyErr.end()));
                WriteLogLineW(hLogFile, L"[Stage] Fallback to original archive path for extraction.");
                tempSrcZip.clear();
            }

            bool cmdCancelled = false;
            if (arcExt == L".zip" || arcExt == L".cbz") {
                const std::wstring srcEsc = EscapePowerShellSingleQuoted(srcForTar);
                const std::wstring dstEsc = EscapePowerShellSingleQuoted(tempDir);
                std::wstring extractCmd =
                    L"powershell.exe -NoProfile -NonInteractive -Command \"$ErrorActionPreference='Stop'; "
                    L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                    L"$src='" + srcEsc + L"'; $dst='" + dstEsc + L"'; "
                    L"if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue }; "
                    L"$global:lastZipErr=$null; "
                    L"function TryExtract([System.Text.Encoding]$enc) { "
                    L"  for ($i=0; $i -lt 3; $i++) { "
                    L"    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue }; "
                    L"    try { [System.IO.Compression.ZipFile]::ExtractToDirectory($src, $dst, $enc); return $true } "
                    L"    catch { $global:lastZipErr=$_; [System.GC]::Collect(); [System.GC]::WaitForPendingFinalizers(); [System.Threading.Thread]::Sleep(150) } "
                    L"  }; "
                    L"  return $false "
                    L"}; "
                    L"if (TryExtract([System.Text.Encoding]::UTF8)) { exit 0 }; "
                    L"if (TryExtract([System.Text.Encoding]::GetEncoding(949))) { exit 0 }; "
                    L"if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue }; "
                    L"New-Item -ItemType Directory -Path $dst -Force | Out-Null; "
                    L"& tar.exe -xf $src -C $dst; "
                    L"if ($LASTEXITCODE -eq 0) { exit 0 }; "
                    L"if ($global:lastZipErr) { $global:lastZipErr | Out-String | Write-Output }; "
                    L"exit 1\"";

                prepSuccess = RunCmdLog(extractCmd, hLogFile, nullptr, &cmdCancelled);
                if (!prepSuccess) {
                    WriteLogLineW(hLogFile, L"[Stage] ZIP extraction with UTF-8/CP949 fallback failed.");
                }
            } else {
                std::wstring extractCmd = L"tar.exe -xf \"" + srcForTar + L"\" -C \"" + tempDir + L"\"";
                prepSuccess = RunCmdLog(extractCmd, hLogFile, nullptr, &cmdCancelled);
                if (!prepSuccess) {
                    WriteLogLineW(hLogFile, L"[Stage] Extraction command failed.");
                }
            }

            if (cmdCancelled) {
                cancelledByUser.store(true);
            } else if (!prepSuccess && !g_stopRequested.load()) {
                failureStatus = L"실패 (압축해제 오류)";
            }
            if (!tempSrcZip.empty()) {
                WriteLogLineW(hLogFile, L"[Stage] Temporary staged archive kept for diagnostics: " + tempSrcZip);
            }
        } else {
            fs::path stagedInputDir = fs::path(tempDir) / L"input";
            WriteLogLineW(hLogFile, L"[Stage] Copy directory -> " + stagedInputDir.wstring());

            std::error_code mkEc;
            fs::create_directories(stagedInputDir, mkEc);
            if (mkEc) {
                ec = mkEc;
            } else {
                fs::copy(inputPath, stagedInputDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            }

            prepSuccess = !ec;
            if (!prepSuccess) {
                WriteLogLineW(hLogFile, L"[Stage] Failed to copy directory.");
                const std::string dirCopyErr = ec.message();
                WriteLogLineW(hLogFile, L"[Stage] Error: " + std::wstring(dirCopyErr.begin(), dirCopyErr.end()));
                WriteLogLineW(hLogFile, L"[Stage] Source: " + inputPath.wstring());
                WriteLogLineW(hLogFile, L"[Stage] Destination: " + stagedInputDir.wstring());
                failureStatus = L"실패 (입력복사 오류)";
            } else {
                WriteLogLineW(hLogFile, L"[Stage] Directory staging copy success.");
            }
        }

        if (prepSuccess && !g_stopRequested.load()) {
            auto collectConvertibleImages = [&](std::vector<fs::path>& imagesOut) -> bool {
                imagesOut.clear();
                std::error_code scanEc;
                size_t regularFileCount = 0;
                size_t totalEntryCount = 0;
                for (auto it = fs::recursive_directory_iterator(tempDir, fs::directory_options::skip_permission_denied, scanEc);
                     it != fs::recursive_directory_iterator(); it.increment(scanEc)) {
                    if (scanEc) {
                        break;
                    }
                    if (g_stopRequested.load()) {
                        cancelledByUser.store(true);
                        break;
                    }
                    totalEntryCount++;
                    std::error_code fileEc;
                    if (!it->is_regular_file(fileEc) || fileEc) {
                        continue;
                    }
                    regularFileCount++;
                    std::wstring imgExt = it->path().extension().wstring();
                    std::transform(imgExt.begin(), imgExt.end(), imgExt.begin(), ::towlower);
                    if (imgExt == L".jpg" || imgExt == L".jpeg" || imgExt == L".png" || imgExt == L".y4m" || imgExt == L".webp") {
                        imagesOut.push_back(it->path());
                    }
                }

                if (scanEc) {
                    const std::string msg = scanEc.message();
                    WriteLogLineW(hLogFile, L"[Stage] Directory scan error: " + std::wstring(msg.begin(), msg.end()));
                }
                WriteLogLineW(hLogFile, L"[Stage] Extracted entries: " + std::to_wstring(totalEntryCount) +
                    L", regular files: " + std::to_wstring(regularFileCount));
                WriteLogLineW(hLogFile, L"[Stage] Images discovered in staged archive: " + std::to_wstring(imagesOut.size()));
                return !scanEc;
            };

            std::vector<fs::path> imagesToConvert;
            collectConvertibleImages(imagesToConvert);
            
            std::atomic<bool> allGood(true);
            size_t totalImgs = imagesToConvert.size();
            std::atomic<size_t> completed(0);
            
            std::vector<std::thread> zipWorkers;
            std::atomic<size_t> imgIdx(0);
            
            // For ZIP processing, allow parallel encoding up to the number of total threads available.
            // If g_concurrentJobs * g_threadsPerJob is high, we can spawn many processes.
            // To maximize CPU, we spawn multiple avifenc processes inside the ZIP.
            int numWorkers = g_concurrentJobs > 0 ? g_concurrentJobs : 1; 
            
            for (int w = 0; w < numWorkers; ++w) {
                zipWorkers.emplace_back([&]() {
                    while(true) {
                        if (g_stopRequested.load()) {
                            cancelledByUser.store(true);
                            break;
                        }

                        size_t currentIdx = imgIdx.fetch_add(1);
                        if (currentIdx >= totalImgs) break;
                        
                        {
                            std::lock_guard<std::mutex> lock(g_itemsMutex);
                            g_items[i].status = L"변환중(" + std::to_wstring(completed.load() + 1) + L"/" + std::to_wstring(totalImgs) + L")";
                        }
                        InvalidateRect(hListView, NULL, FALSE);
                        
                        fs::path imgPath = imagesToConvert[currentIdx];
                        fs::path encodeInputPath = imgPath;
                        fs::path tempPngInput;
                        if (ToLowerString(imgPath.extension().wstring()) == L".webp") {
                            tempPngInput = imgPath;
                            tempPngInput += L".wic.png";
                            if (!ConvertWebPToPngWithWic(imgPath, tempPngInput, hLogFile)) {
                                allGood = false;
                                failureStatus = L"실패 (WEBP 변환 오류)";
                                completed.fetch_add(1);
                                continue;
                            }
                            encodeInputPath = tempPngInput;
                        }
                        fs::path imgOut = imgPath;
                        imgOut.replace_extension(L".avif");
                        
                        std::wstring cmd = L"\"" + absExePath + L"\" --speed " + speed + L" -q " + q + L" -j " + jobs + advFlags + L" \"" + encodeInputPath.wstring() + L"\" \"" + imgOut.wstring() + L"\"";
                        
                        bool cmdCancelled = false;
                        if (RunCmdLog(cmd, hLogFile, nullptr, &cmdCancelled)) {
                            std::error_code local_ec;
                            fs::remove(imgPath, local_ec);
                            if (!tempPngInput.empty()) {
                                local_ec.clear();
                                fs::remove(tempPngInput, local_ec);
                            }
                        } else {
                            allGood = false;
                            if (g_stopRequested.load()) {
                                cancelledByUser.store(true);
                            } else {
                                failureStatus = L"실패 (인코딩 오류)";
                            }
                            if (cmdCancelled) {
                                cancelledByUser.store(true);
                            }
                            if (!tempPngInput.empty()) {
                                std::error_code local_ec;
                                fs::remove(tempPngInput, local_ec);
                            }
                        }
                        completed.fetch_add(1);
                    }
                });
            }
            
            for (auto& t : zipWorkers) {
                if (t.joinable()) t.join();
            }
            
            if (allGood && totalImgs > 0 && !g_stopRequested.load()) {
                {
                    std::lock_guard<std::mutex> lock(g_itemsMutex);
                    g_items[i].status = L"재압축중...";
                }
                InvalidateRect(hListView, NULL, FALSE);

                const std::wstring srcRootEsc = EscapePowerShellSingleQuoted(tempDir);
                const std::wstring dstEsc = EscapePowerShellSingleQuoted(outputPath.wstring());
                std::wstring archiveCmd =
                    L"powershell.exe -NoProfile -NonInteractive -Command \"$ErrorActionPreference='Stop'; "
                    L"$srcRoot='" + srcRootEsc + L"'; $dst='" + dstEsc + L"'; "
                    L"if (!(Test-Path -LiteralPath $srcRoot)) { Write-Output 'Archive source directory not found.'; exit 1 }; "
                    L"$srcItems = Get-ChildItem -LiteralPath $srcRoot -Force; "
                    L"if (-not $srcItems -or $srcItems.Count -eq 0) { Write-Output 'Archive source is empty.'; exit 1 }; "
                    L"if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue }; "
                    L"$dstParent = Split-Path -Parent $dst; "
                    L"if ($dstParent -and !(Test-Path -LiteralPath $dstParent)) { New-Item -ItemType Directory -Path $dstParent -Force | Out-Null }; "
                    L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                    L"[System.IO.Compression.ZipFile]::CreateFromDirectory($srcRoot, $dst, [System.IO.Compression.CompressionLevel]::Optimal, $false); exit 0\"";
                bool cmdCancelled = false;
                if (RunCmdLog(archiveCmd, hLogFile, nullptr, &cmdCancelled)) {
                    success = true;
                } else if (!cmdCancelled && !g_stopRequested.load()) {
                    failureStatus = L"실패 (재압축 오류)";
                }
                if (cmdCancelled) {
                    cancelledByUser.store(true);
                }
            } else if (prepSuccess && totalImgs == 0) {
                WriteLogLineW(hLogFile, L"[Stage] No convertible images found after extraction.");
                if (isArchive) {
                    const std::wstring stem = inputPath.stem().wstring();
                    const std::wstring ext = inputPath.extension().wstring();
                    fs::path markedPath = inputPath.parent_path() / (stem + L".avif" + ext);

                    std::error_code markEc;
                    if (fs::exists(markedPath, markEc) && !markEc) {
                        int suffix = 1;
                        while (suffix < 10000) {
                            fs::path candidate = inputPath.parent_path() /
                                (stem + L".avif." + std::to_wstring(suffix) + ext);
                            markEc.clear();
                            if (!fs::exists(candidate, markEc) || markEc) {
                                markedPath = candidate;
                                break;
                            }
                            ++suffix;
                        }
                    }

                    markEc.clear();
                    if (TryMoveFileWithFallback(inputPath, markedPath, markEc)) {
                        outputPath = markedPath;
                        success = true;
                        archiveMarkedAsAvif = true;
                        WriteLogLineW(hLogFile, L"[Stage] No convertible image found. Archive marked as: " + markedPath.wstring());
                    } else {
                        const std::string msg = markEc.message();
                        WriteLogLineW(hLogFile, L"[Stage] Failed to append .avif marker: " + std::wstring(msg.begin(), msg.end()));
                        failureStatus = L"실패 (.avif 표기 실패)";
                    }
                } else {
                    failureStatus = L"실패 (변환대상 없음)";
                }
            }
        }
        fs::remove_all(tempDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            g_items[i].tempWorkDir.clear();
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(g_itemsMutex);
            g_items[i].status = L"변환중...";
        }
        InvalidateRect(hListView, NULL, FALSE);
        
        fs::path encodeInputPath = inputPath;
        fs::path tempPngInput;
        if (ToLowerString(inputPath.extension().wstring()) == L".webp") {
            wchar_t tempPath[MAX_PATH] = { 0 };
            GetTempPathW(MAX_PATH, tempPath);
            tempPngInput = fs::path(tempPath) /
                (L"AVIFMaster_webp_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(i) + L".png");
            if (!ConvertWebPToPngWithWic(inputPath, tempPngInput, hLogFile)) {
                failureStatus = L"실패 (WEBP 변환 오류)";
            } else {
                encodeInputPath = tempPngInput;
            }
        }

        std::wstring cmd = L"\"" + absExePath + L"\" --speed " + speed + L" -q " + q + L" -j " + jobs + advFlags;
        cmd += L" \"" + encodeInputPath.wstring() + L"\" \"" + outputPath.wstring() + L"\"";
        
        DWORD exitCode = 1;
        bool cmdCancelled = false;
        if (failureStatus != L"실패 (WEBP 변환 오류)" && RunCmdLog(cmd, hLogFile, &exitCode, &cmdCancelled) && exitCode == 0) {
            success = true;
        } else if (!cmdCancelled && !g_stopRequested.load()) {
            if (failureStatus != L"실패 (WEBP 변환 오류)") {
                failureStatus = L"실패 (인코딩 오류)";
            }
        }
        if (cmdCancelled) {
            cancelledByUser.store(true);
        }
        if (!tempPngInput.empty()) {
            std::error_code removeEc;
            fs::remove(tempPngInput, removeEc);
        }
    }

    if (hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hLogFile);
    }

    std::error_code ec;
    uintmax_t resultSize = 0;
    bool hasResultSize = false;
    if (success) {
        resultSize = fs::file_size(outputPath, ec);
        hasResultSize = (!ec && resultSize > 0);
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto diff = endTime - startTime;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
    int hrs = ms / 3600000;
    int mins = (ms % 3600000) / 60000;
    int secs = (ms % 60000) / 1000;
    wchar_t timeBuf[32];
    if (hrs > 0) swprintf(timeBuf, 32, L"%02d:%02d:%02d", hrs, mins, secs);
    else swprintf(timeBuf, 32, L"%02d:%02d", mins, secs);

    std::wstring finalStatus;
    std::wstring finalResultSize;
    std::wstring finalRatio;
    bool clearLogPath = false;

    if (success) {
        bool modifiedTimeApplied = true;
        if (g_keepModifiedTime && hasSourceModifiedTime) {
            modifiedTimeApplied = TrySetLastWriteTime(outputPath, sourceModifiedTime);
        }

        finalStatus = L"완료";
        if (hasResultSize) {
            finalResultSize = FormatSize(resultSize);
        }

        uintmax_t origSize = itemBytes;
        if (!archiveMarkedAsAvif) {
            origSize = fs::file_size(inputPath, ec);
            if (ec || origSize == static_cast<uintmax_t>(-1)) {
                origSize = itemBytes;
            }
        }
        if (hasResultSize && origSize > 0) {
            double ratio = (double)(origSize - resultSize) / (double)origSize * 100.0;
            wchar_t ratioBuf[32];
            swprintf(ratioBuf, 32, L"%.1f%%", ratio);
            finalRatio = ratioBuf;
        }

        if (archiveMarkedAsAvif) {
            finalStatus = L"완료(변환대상 없음)";
        } else if (g_delOrig) {
            std::error_code delEc;
            if (fs::is_directory(inputPath, delEc) && !delEc) {
                fs::remove_all(inputPath, delEc);
            } else {
                delEc.clear();
                fs::remove(inputPath, delEc);
            }

            if (delEc) {
                finalStatus = L"완료(원본삭제 실패)";
            } else {
                finalStatus = L"완료(원본삭제)";
            }
        }

        if (g_keepModifiedTime && !hasSourceModifiedTime) {
            finalStatus += L"(수정일 읽기 실패)";
        } else if (g_keepModifiedTime && !modifiedTimeApplied) {
            finalStatus += L"(수정일 유지 실패)";
        }

        clearLogPath = TryDeleteFileWithRetry(logPath);
        if (!clearLogPath) {
            std::error_code logExistsEc;
            clearLogPath = !fs::exists(logPath, logExistsEc) && !logExistsEc;
        }
    } else if (g_stopRequested.load() || cancelledByUser.load()) {
        finalStatus = L"사용자 중지";
    } else {
        finalStatus = failureStatus;
    }

    {
        std::lock_guard<std::mutex> lock(g_itemsMutex);
        g_items[i].elapsedTime = timeBuf;
        g_items[i].status = finalStatus;
        if (!finalResultSize.empty()) {
            g_items[i].resultSize = finalResultSize;
        }
        if (!finalRatio.empty()) {
            g_items[i].ratio = finalRatio;
        }
        if (clearLogPath) {
            g_items[i].logPath.clear();
        }
    }
    InvalidateRect(hListView, NULL, FALSE);
    AddProcessedBytes(itemBytes);
}

void MasterWorkerThread() {
    std::atomic<size_t> currentIndex(0);
    std::vector<std::thread> workers;
    
    for (int i = 0; i < g_concurrentJobs; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                if (g_stopRequested.load()) {
                    break;
                }
                size_t index = currentIndex.fetch_add(1);
                if (index >= g_items.size()) break;
                ProcessItem(index);
            }
        });
    }
    
    for (auto& w : workers) {
        w.join();
    }
    const bool stoppedByUser = g_stopRequested.load();
    const bool closeAfterRun = g_closeAfterCurrentRun.load();
    g_isConverting.store(false);
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_CONVERT_FINISHED, stoppedByUser ? 1 : 0, 0);
    }

    if (!stoppedByUser && g_autoClose && g_hwnd) {
        if (g_isRegistering.load()) {
            g_pendingAutoClose.store(true);
        } else {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
    } else if (!stoppedByUser && !closeAfterRun) {
        MessageBoxW(NULL, L"모든 변환 작업이 완료되었습니다.", L"알림", MB_OK | MB_ICONINFORMATION);
    } else if (stoppedByUser && !closeAfterRun) {
        MessageBoxW(NULL,
            UiText(L"변환 작업이 사용자 요청으로 중지되었습니다.", L"Conversion was stopped by user request."),
            UiText(L"알림", L"Notice"), MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            LoadUiLanguagePreference();
            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_LISTVIEW_CLASSES;
            InitCommonControlsEx(&icex);

            // Create default GUI font for all controls
            NONCLIENTMETRICSW ncm = { sizeof(ncm) };
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
                g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            }
            if (!g_hFont) {
                g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            }

            DragAcceptFiles(hwnd, TRUE);

            hListView = CreateWindowExW(0, WC_LISTVIEWW, L"", 
                WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_OWNERDATA,
                10, 10, 760, 360, hwnd, (HMENU)ID_LISTVIEW, GetModuleHandle(NULL), NULL);
            
            ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
            
            LVCOLUMNW lvc = {0};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            const wchar_t* cols[] = {
                UiText(L"이름", L"Name"),
                UiText(L"형식", L"Type"),
                UiText(L"원본 용량", L"Original Size"),
                UiText(L"상태", L"Status"),
                UiText(L"결과 용량", L"Result Size"),
                UiText(L"압축률", L"Reduction"),
                UiText(L"소요 시간(EST)", L"Elapsed (EST)")
            };
            int widths[] = { 180, 50, 80, 120, 80, 80, 120 };
            for(int i = 0; i < 7; ++i) {
                lvc.pszText = (LPWSTR)cols[i];
                lvc.cx = widths[i];
                ListView_InsertColumn(hListView, i, &lvc);
            }
            
            // Options Group
            CreateWindowExW(0, L"BUTTON", UiText(L"설정", L"Settings"), WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 380, 760, 175, hwnd, NULL, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"STATIC", UiText(L"언어:", L"Language:"), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                20, 400, 45, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            HWND hCmbLang = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                65, 395, 120, 100, hwnd, (HMENU)ID_CMB_LANG, GetModuleHandle(NULL), NULL);
            SendMessageW(hCmbLang, CB_ADDSTRING, 0, (LPARAM)L"한국어");
            SendMessageW(hCmbLang, CB_ADDSTRING, 0, (LPARAM)L"English");
            SendMessageW(hCmbLang, CB_SETCURSEL, IsEnglishUiLanguage() ? 1 : 0, 0);
            
            CreateWindowExW(0, L"STATIC", UiText(L"품질:", L"Quality:"), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                200, 400, 45, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            HWND hCmbQuality = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                245, 395, 120, 100, hwnd, (HMENU)ID_CMB_QUALITY, GetModuleHandle(NULL), NULL);
            SendMessageW(hCmbQuality, CB_ADDSTRING, 0, (LPARAM)UiText(L"Fast (낮은 압축률)", L"Fast (Lower compression)"));
            SendMessageW(hCmbQuality, CB_ADDSTRING, 0, (LPARAM)UiText(L"Normal (균형)", L"Normal (Balanced)"));
            SendMessageW(hCmbQuality, CB_ADDSTRING, 0, (LPARAM)UiText(L"High ( 높은 압축률)", L"High (Higher compression)"));
            SendMessageW(hCmbQuality, CB_SETCURSEL, 1, 0);

            CreateWindowExW(0, L"STATIC", UiText(L"하드웨어:", L"Hardware:"), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                380, 400, 65, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            HWND hCmbHw = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                450, 395, 100, 100, hwnd, (HMENU)ID_CMB_HW, GetModuleHandle(NULL), NULL);
            SendMessageW(hCmbHw, CB_ADDSTRING, 0, (LPARAM)L"CPU Only");
            SendMessageW(hCmbHw, CB_ADDSTRING, 0, (LPARAM)L"CPU + GPU");
            SendMessageW(hCmbHw, CB_SETCURSEL, 0, 0);

            CreateWindowExW(0, L"STATIC", UiText(L"동시작업:", L"Jobs:"), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                565, 400, 55, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            
            unsigned int numCores = std::thread::hardware_concurrency();
            if (numCores == 0) numCores = 4;
            
            // Intelligent defaults: 
            // - If 16 cores or more: 8 concurrent jobs * 4 threads per job
            // - If 8 cores: 4 concurrent jobs * 4 threads per job
            // - If 4 cores: 2 concurrent jobs * 2 threads per job
            int defaultJobs = (numCores >= 16) ? 8 : ((numCores >= 8) ? 4 : 2);
            int defaultThreads = (numCores >= 16) ? 4 : ((numCores >= 8) ? 4 : 2);
            
            HWND hEdtJobs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(defaultJobs).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER,
                620, 395, 30, 25, hwnd, (HMENU)ID_EDT_JOBS, GetModuleHandle(NULL), NULL);
            
            CreateWindowExW(0, L"STATIC", UiText(L"스레드:", L"Threads:"), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                655, 400, 60, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            HWND hEdtThreads = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(defaultThreads).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER,
                715, 395, 35, 25, hwnd, (HMENU)ID_EDT_THREADS, GetModuleHandle(NULL), NULL);
            
            HWND hChkSimd = CreateWindowExW(0, L"BUTTON", UiText(L"SIMD/Assembly 최적화 사용", L"Use SIMD/Assembly optimization"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                20, 435, 210, 20, hwnd, (HMENU)ID_CHK_SIMD, GetModuleHandle(NULL), NULL);
            SendMessageW(hChkSimd, BM_SETCHECK, BST_CHECKED, 0);

            CreateWindowExW(0, L"BUTTON", UiText(L"기존 파일 제거", L"Delete original files"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                240, 435, 100, 20, hwnd, (HMENU)ID_CHK_DEL_ORIG, GetModuleHandle(NULL), NULL);
            CreateWindowExW(0, L"BUTTON", UiText(L"작업완료 시 창닫기", L"Close window on completion"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                350, 435, 160, 20, hwnd, (HMENU)ID_CHK_AUTO_CLOSE, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"선택 삭제 (DEL)", L"Delete selected (DEL)"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                620, 435, 130, 25, hwnd, (HMENU)ID_BTN_DEL, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"설정 저장", L"Save settings"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 465, 110, 25, hwnd, (HMENU)ID_BTN_SAVE_SETTINGS, GetModuleHandle(NULL), NULL);
            CreateWindowExW(0, L"BUTTON", UiText(L"완료 목록 삭제", L"Delete completed"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                140, 465, 120, 25, hwnd, (HMENU)ID_BTN_DEL_COMPLETED, GetModuleHandle(NULL), NULL);
            CreateWindowExW(0, L"BUTTON", UiText(L"실패 목록 삭제", L"Delete failed"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                270, 465, 120, 25, hwnd, (HMENU)ID_BTN_DEL_FAILED, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"특정 폴더에 저장", L"Save to specific folder"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                20, 495, 150, 22, hwnd, (HMENU)ID_CHK_CUSTOM_OUTPUT, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"파일수정일유지", L"Keep modified date"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                180, 495, 130, 22, hwnd, (HMENU)ID_CHK_KEEP_CREATION_TIME, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                20, 522, 640, 24, hwnd, (HMENU)ID_EDT_OUTPUT_DIR, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"폴더 선택", L"Browse"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                670, 522, 80, 24, hwnd, (HMENU)ID_BTN_OUTPUT_BROWSE, GetModuleHandle(NULL), NULL);

            CreateWindowExW(0, L"BUTTON", UiText(L"변환 시작", L"Start conversion"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 560, 760, 40, hwnd, (HMENU)ID_BTN_CONVERT, GetModuleHandle(NULL), NULL);

            g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                10, 605, 760, 8, hwnd, (HMENU)ID_PROGRESS, GetModuleHandle(NULL), NULL);
            SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 1000);
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

            // Apply font to all child controls
            EnumChildWindows(hwnd, [](HWND hChild, LPARAM lParam) -> BOOL {
                SendMessageW(hChild, WM_SETFONT, (WPARAM)lParam, TRUE);
                return TRUE;
            }, (LPARAM)g_hFont);

            // Restore saved settings
            LoadSettings(hwnd);
            UpdateOutputDirUiState(hwnd);

            HWND hHeader = ListView_GetHeader(hListView);
            DWORD dwStyle = GetWindowLongW(hHeader, GWL_STYLE);
            SetWindowLongW(hHeader, GWL_STYLE, dwStyle | HDS_CHECKBOXES);
            
            HDITEMW hdi = {0};
            hdi.mask = HDI_FORMAT;
            Header_GetItem(hHeader, 0, &hdi);
            hdi.fmt |= HDF_CHECKBOX | HDF_CHECKED;
            Header_SetItem(hHeader, 0, &hdi);
            
            ListView_SetItemCountEx(hListView, g_items.size(), LVSICF_NOINVALIDATEALL);

            if (g_autoConvert) {
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_CONVERT, 0), 0);
            }
            break;
        }
        case WM_NOTIFY: {
            LPNMHDR nmhdr = (LPNMHDR)lParam;
            if (nmhdr->idFrom == ID_LISTVIEW) {
                if (nmhdr->code == LVN_GETDISPINFO) {
                    NMLVDISPINFOW* pdi = (NMLVDISPINFOW*)lParam;
                    std::lock_guard<std::mutex> lock(g_itemsMutex);
                    int row = pdi->item.iItem;
                    if (row >= 0 && row < g_items.size()) {
                        if (pdi->item.mask & LVIF_TEXT) {
                            int col = pdi->item.iSubItem;
                            std::wstring text;
                            if (col == 0) text = g_items[row].name;
                            else if (col == 1) text = g_items[row].type;
                            else if (col == 2) text = g_items[row].origSize;
                            else if (col == 3) text = g_items[row].status;
                            else if (col == 4) text = g_items[row].resultSize;
                            else if (col == 5) text = g_items[row].ratio;
                            else if (col == 6) text = g_items[row].elapsedTime;
                            wcsncpy_s(pdi->item.pszText, pdi->item.cchTextMax, text.c_str(), _TRUNCATE);
                        }
                        if (pdi->item.mask & LVIF_STATE) {
                            pdi->item.state = INDEXTOSTATEIMAGEMASK(g_items[row].checked ? 2 : 1);
                            pdi->item.stateMask = LVIS_STATEIMAGEMASK;
                        }
                    }
                } else if (nmhdr->code == NM_CLICK || nmhdr->code == NM_DBLCLK) {
                    NMITEMACTIVATE* pnmia = (NMITEMACTIVATE*)lParam;
                    if (pnmia->iItem != -1) {
                        LVHITTESTINFO ht = {0};
                        ht.pt = pnmia->ptAction;
                        ListView_HitTest(hListView, &ht);

                        if (pnmia->iSubItem == 0 && (ht.flags & LVHT_ONITEMSTATEICON)) {
                            std::lock_guard<std::mutex> lock(g_itemsMutex);
                            g_items[pnmia->iItem].checked = !g_items[pnmia->iItem].checked;
                            InvalidateRect(hListView, NULL, FALSE);
                        } else if (nmhdr->code == NM_DBLCLK) {
                            FileItem clickedItem;
                            bool canOpenLog = false;
                            {
                                std::lock_guard<std::mutex> lock(g_itemsMutex);
                                if (pnmia->iItem >= 0 && pnmia->iItem < (int)g_items.size()) {
                                    clickedItem = g_items[pnmia->iItem];
                                    canOpenLog = clickedItem.status.find(L"실패") != std::wstring::npos;
                                }
                            }
                            if (canOpenLog) {
                                OpenFailureLogForItem(hwnd, clickedItem);
                            }
                        }
                    }
                } else if (nmhdr->code == LVN_KEYDOWN) {
                    LPNMLVKEYDOWN pnkd = (LPNMLVKEYDOWN)lParam;
                    if (pnkd->wVKey == VK_DELETE) {
                        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_DEL, 0), 0);
                    } else if (pnkd->wVKey == L'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_CHECKALL, 0), 0);
                    }
                }
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

            for(UINT i=0; i<count; ++i) {
                UINT len = DragQueryFileW(hDrop, i, NULL, 0);
                if (len > 0) {
                    std::vector<wchar_t> buf(len + 1, L'\0');
                    if(DragQueryFileW(hDrop, i, buf.data(), (UINT)buf.size())) {
                        AddFileItem(buf.data());
                    }
                }
            }
            DragFinish(hDrop);
            break;
        }
        case WM_APP_CONVERT_FINISHED: {
            SetConvertButtonText(hwnd, UiText(L"변환 시작", L"Start conversion"));
            const bool removeCompleted = g_pendingDeleteCompleted.exchange(false);
            const bool removeFailed = g_pendingDeleteFailed.exchange(false);
            if (removeCompleted || removeFailed) {
                const size_t newCount = DeleteItemsByResultStatus(removeCompleted, removeFailed);
                ListView_SetItemCountEx(hListView, (int)newCount, LVSICF_NOINVALIDATEALL);
                ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED);
            }
            UpdateProgressBarUI();
            InvalidateRect(hListView, NULL, FALSE);
            if (g_closeAfterCurrentRun.load()) {
                g_closeAfterCurrentRun.store(false);
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_APP_PROGRESS_UPDATE: {
            UpdateProgressBarUI();
            break;
        }
        case WM_APP_DELETE_STATUS: {
            const bool removeCompleted = wParam != 0;
            const bool removeFailed = lParam != 0;
            const size_t newCount = DeleteItemsByResultStatus(removeCompleted, removeFailed);
            ListView_SetItemCountEx(hListView, (int)newCount, LVSICF_NOINVALIDATEALL);
            ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED);
            InvalidateRect(hListView, NULL, FALSE);
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == ID_BTN_CONVERT) {
                if (g_isConverting.load()) {
                    g_stopRequested.store(true);
                    SetConvertButtonText(hwnd, UiText(L"중지 요청중...", L"Stopping..."));
                    MarkActiveItemsAsUserStopped();
                    CleanupActiveTempWorkDirs();
                    InvalidateRect(hListView, NULL, FALSE);
                    break;
                }

                g_stopRequested.store(false);
                g_isConverting.store(true);
                g_closeAfterCurrentRun.store(false);
                SetConvertButtonText(hwnd, UiText(L"변환 중지", L"Stop conversion"));

                unsigned long long totalBytes = 0;
                {
                    std::lock_guard<std::mutex> lock(g_itemsMutex);
                    for (size_t idx = 0; idx < g_items.size(); ++idx) {
                        uintmax_t bytes = g_items[idx].sourceSizeBytes;
                        if (bytes == 0) {
                            bytes = GetItemSourceSizeBytes(g_items[idx].path);
                            g_items[idx].sourceSizeBytes = bytes;
                        }
                        totalBytes += static_cast<unsigned long long>(bytes);
                    }
                }
                g_totalBytes.store(totalBytes, std::memory_order_relaxed);
                g_processedBytes.store(0, std::memory_order_relaxed);
                UpdateProgressBarUI();
                
                g_quality = SendMessageW(GetDlgItem(hwnd, ID_CMB_QUALITY), CB_GETCURSEL, 0, 0);
                g_hwMode = SendMessageW(GetDlgItem(hwnd, ID_CMB_HW), CB_GETCURSEL, 0, 0);
                
                wchar_t buf[32];
                GetWindowTextW(GetDlgItem(hwnd, ID_EDT_JOBS), buf, 32);
                g_concurrentJobs = _wtoi(buf);
                if (g_concurrentJobs <= 0) g_concurrentJobs = 1;
                
                GetWindowTextW(GetDlgItem(hwnd, ID_EDT_THREADS), buf, 32);
                g_threadsPerJob = _wtoi(buf);
                if (g_threadsPerJob <= 0) g_threadsPerJob = 1;
                
                g_simd = SendMessageW(GetDlgItem(hwnd, ID_CHK_SIMD), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_delOrig = SendMessageW(GetDlgItem(hwnd, ID_CHK_DEL_ORIG), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_autoClose = SendMessageW(GetDlgItem(hwnd, ID_CHK_AUTO_CLOSE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_keepModifiedTime = SendMessageW(GetDlgItem(hwnd, ID_CHK_KEEP_CREATION_TIME), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_useCustomOutputDir = SendMessageW(GetDlgItem(hwnd, ID_CHK_CUSTOM_OUTPUT), BM_GETCHECK, 0, 0) == BST_CHECKED;
                {
                    wchar_t outDir[MAX_PATH] = { 0 };
                    GetWindowTextW(GetDlgItem(hwnd, ID_EDT_OUTPUT_DIR), outDir, MAX_PATH);
                    g_customOutputDir = outDir;
                }

                if (g_useCustomOutputDir && g_customOutputDir.empty()) {
                    MessageBoxW(hwnd,
                        UiText(L"특정 폴더 저장을 사용하려면 출력 폴더를 선택하세요.", L"Select an output folder to use custom output."),
                        UiText(L"알림", L"Notice"), MB_OK | MB_ICONWARNING);
                    g_isConverting.store(false);
                    SetConvertButtonText(hwnd, UiText(L"변환 시작", L"Start conversion"));
                    break;
                }

                std::thread(MasterWorkerThread).detach();
            } else if (LOWORD(wParam) == ID_CHK_CUSTOM_OUTPUT) {
                UpdateOutputDirUiState(hwnd);
            } else if (LOWORD(wParam) == ID_BTN_OUTPUT_BROWSE) {
                std::wstring selected;
                if (BrowseForFolder(hwnd, selected)) {
                    SetWindowTextW(GetDlgItem(hwnd, ID_EDT_OUTPUT_DIR), selected.c_str());
                }
            } else if (LOWORD(wParam) == ID_BTN_CHECKALL) {
                std::lock_guard<std::mutex> lock(g_itemsMutex);
                for(auto& item : g_items) item.checked = true;
                InvalidateRect(hListView, NULL, FALSE);
            } else if (LOWORD(wParam) == ID_BTN_UNCHECKALL) {
                std::lock_guard<std::mutex> lock(g_itemsMutex);
                for(auto& item : g_items) item.checked = false;
                InvalidateRect(hListView, NULL, FALSE);
            } else if (LOWORD(wParam) == ID_BTN_DEL) {
                if (g_isConverting.load()) {
                    MessageBoxW(hwnd,
                        UiText(L"변환 중에는 삭제할 수 없습니다.", L"Cannot delete while conversion is running."),
                        UiText(L"알림", L"Notice"), MB_OK | MB_ICONWARNING);
                    break;
                }
                std::vector<FileItem> new_items;
                {
                    std::lock_guard<std::mutex> lock(g_itemsMutex);
                    new_items.reserve(g_items.size());
                    for(size_t i = 0; i < g_items.size(); ++i) {
                        bool selected = (ListView_GetItemState(hListView, (int)i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        if (!g_items[i].checked && !selected) {
                            new_items.push_back(g_items[i]);
                        }
                    }
                    g_items = std::move(new_items);
                }
                size_t itemCount = 0;
                {
                    std::lock_guard<std::mutex> lock(g_itemsMutex);
                    itemCount = g_items.size();
                }
                ListView_SetItemCountEx(hListView, (int)itemCount, LVSICF_NOINVALIDATEALL);
                ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED); // Clear selection to prevent out of bounds
                InvalidateRect(hListView, NULL, FALSE);
            } else if (LOWORD(wParam) == ID_BTN_SAVE_SETTINGS) {
                SaveSettings(hwnd);
                MessageBoxW(hwnd,
                    UiText(L"설정이 저장되었습니다. 언어 변경은 재시작 후 적용됩니다.", L"Settings saved. Language changes apply after restart."),
                    UiText(L"알림", L"Notice"), MB_OK | MB_ICONINFORMATION);
            } else if (LOWORD(wParam) == ID_BTN_DEL_COMPLETED || LOWORD(wParam) == ID_BTN_DEL_FAILED) {
                const bool removeCompleted = LOWORD(wParam) == ID_BTN_DEL_COMPLETED;
                const bool removeFailed = LOWORD(wParam) == ID_BTN_DEL_FAILED;

                if (g_isConverting.load()) {
                    if (removeCompleted) {
                        g_pendingDeleteCompleted.store(true);
                    }
                    if (removeFailed) {
                        g_pendingDeleteFailed.store(true);
                    }
                    MessageBoxW(hwnd,
                        UiText(L"변환 중에는 목록 삭제를 예약합니다. 현재 작업 완료 후 자동으로 삭제됩니다.", L"Deletion is queued while converting. It will run after current work completes."),
                        UiText(L"안내", L"Info"), MB_OK | MB_ICONINFORMATION);
                    break;
                }
                PostMessageW(hwnd, WM_APP_DELETE_STATUS, removeCompleted ? 1 : 0, removeFailed ? 1 : 0);
            }
            break;
        }
        case WM_COPYDATA: {
            COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
            if (cds->dwData == 1) {
                std::wstring path((wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
                AddFileItem(path);
            }
            break;
        }
        case WM_SIZE: {
            LayoutRegisterOverlay(hwnd);
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_CLOSE: {
            if (g_isConverting.load()) {
                int answer = MessageBoxW(hwnd,
                    L"변환이 진행 중입니다.\n예: 진행 중인 작업을 끝까지 처리 후 종료\n아니요: 작업을 중단하고 임시/로그 파일 정리 후 즉시 종료",
                    L"종료 확인",
                    MB_YESNOCANCEL | MB_ICONQUESTION);

                if (answer == IDYES) {
                    g_closeAfterCurrentRun.store(true);
                    return 0;
                }

                if (answer == IDNO) {
                    g_stopRequested.store(true);
                    g_closeAfterCurrentRun.store(false);
                    MarkActiveItemsAsUserStopped();
                    TerminateChildProcessesOfCurrentProcess();
                    CleanupActiveTempWorkDirs();
                    CleanupTrackedLogFiles();
                    ExitProcess(0);
                    return 0;
                }

                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            if (g_hFont) {
                DeleteObject(g_hFont);
                g_hFont = NULL;
            }
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    bool isMaster = false;
    
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        isMaster = false;
        CloseHandle(hMutex);
    } else {
        isMaster = true;
    }
    
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    std::vector<std::wstring> files;
    if (argv) {
        for(int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"/auto" || arg == L"--auto-convert") {
                g_autoConvert = true;
            } else {
                files.push_back(arg);
            }
        }
        LocalFree(argv);
    }
    
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    CleanupStaleTempArtifacts(tempPath);
    
    if (!isMaster) {
        if (!files.empty()) {
            std::wstring argFile = std::wstring(tempPath) + L"AVIFMaster_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + std::to_wstring(GetTickCount()) + L"_" + std::to_wstring(files.size()) + L".args";
            std::wofstream ofs(argFile.c_str());
            for (const auto& file : files) {
                ofs << file << L"\n";
            }
        }
        return 0;
    }
    
    // Master: Adaptive Settling Time (wait 300-500ms for incoming files from other context menu processes)
    DWORD lastTime = GetTickCount();
    while (GetTickCount() - lastTime < 400) {
        Sleep(50);
        for (const auto& entry : fs::directory_iterator(tempPath)) {
            std::wstring fname = entry.path().filename().wstring();
            if (fname.find(L"AVIFMaster_") == 0 && fname.find(L".args") != std::wstring::npos) {
                std::wifstream ifs(entry.path());
                std::wstring file;
                while (std::getline(ifs, file)) {
                    if (!file.empty()) {
                        files.push_back(file);
                    }
                }
                ifs.close();
                std::error_code ec;
                fs::remove(entry.path(), ec);
                lastTime = GetTickCount();
            }
        }
    }
    
    for (const auto& file : files) {
        AddFileItem(file);
    }
    
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WND_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    RegisterClassW(&wc);
    
    // Fixed Window Size
    RECT rc = { 0, 0, 780, 650 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
    
    HWND hwnd = CreateWindowExW(0, WND_CLASS, UiText(L"AVIF-Master", L"AVIF-Master"), WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInstance, NULL);
        
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
