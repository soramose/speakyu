#include <Windows.h>
#include <stdio.h>
#include <commdlg.h>
#include <CommCtrl.h>
#include <vector>
#include <string>
#include <memory>
#include <iomanip>
#include <sstream>
#include "resource.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

typedef void* H_AQTKDA; // AquesTalkDaのインスタンスハンドル

// --- 関数ポインタの型定義 ---
// AquesTalk.dll用
typedef unsigned char* (__stdcall* FN_AquesTalk_Synthe)(const char* koe, int iSpeed, int* size);
typedef void(__stdcall* FN_AquesTalk_FreeWave)(unsigned char* wav);

// AquesTalkDa.dll用
typedef H_AQTKDA(__stdcall* FN_AquesTalkDa_Create)();
typedef void(__stdcall* FN_AquesTalkDa_Release)(H_AQTKDA hMe);
typedef int(__stdcall* FN_AquesTalkDa_Play)(H_AQTKDA hMe, const char* koe, int iSpeed, HWND hWnd, unsigned long msg, unsigned long dwUser);
typedef void(__stdcall* FN_AquesTalkDa_Stop)(H_AQTKDA hMe);
typedef int(__stdcall* FN_AquesTalkDa_IsPlay)(H_AQTKDA hMe);

// --- 構造体定義 ---
struct AquesTalkEngine {
    HMODULE hDll;   // AquesTalk.dll
    HMODULE hDaDll; // AquesTalkDa.dll

    FN_AquesTalk_Synthe   pSynthe;
    FN_AquesTalk_FreeWave pFreeWave;

    FN_AquesTalkDa_Create  pCreate;
    FN_AquesTalkDa_Release pRelease;
    FN_AquesTalkDa_Play    pPlay;
    FN_AquesTalkDa_Stop    pStop;
    FN_AquesTalkDa_IsPlay  pIsPlay;
};

CRITICAL_SECTION g_cs;
H_AQTKDA g_hMe = NULL;
BOOL g_isPlaying = FALSE;
AquesTalkEngine g_engine = { 0 };

// ライブラリ解放
void unloadAquesTalk(AquesTalkEngine* engine) {
    if (engine->hDll) { FreeLibrary(engine->hDll); engine->hDll = NULL; }
    if (engine->hDaDll) { FreeLibrary(engine->hDaDll); engine->hDaDll = NULL; }
    memset(engine, 0, sizeof(AquesTalkEngine));
}

// ライブラリロード
int loadAquesTalk(AquesTalkEngine* engine, int voiceIndex) {
    unloadAquesTalk(engine);

    const wchar_t* subDir;
    switch (voiceIndex) {
    case 0: subDir = L"f1"; break;
    case 1: subDir = L"f2"; break;
    case 2: subDir = L"m1"; break;
    case 3: subDir = L"m2"; break;
    case 4: subDir = L"r1"; break;
    case 5: subDir = L"imd1"; break;
    case 6: subDir = L"jgr"; break;
    default: subDir = L"f1"; break;
    }

    // DLLがあるディレクトリを検索パスに追加（依存DLL解決のため）
    SetDllDirectory(subDir);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"bin\\%s\\AquesTalk.dll", subDir);
    engine->hDll = LoadLibrary(path);

    swprintf_s(path, L"bin\\%s\\AquesTalkDa.dll", subDir);
    engine->hDaDll = LoadLibrary(path);

    // 検索パスを元に戻す
    SetDllDirectory(NULL);

    if (!engine->hDll || !engine->hDaDll) {
        // デバッグ用
        // MessageBox(NULL, L"DLLのロードに失敗しました", L"Error", MB_ICONERROR);
        unloadAquesTalk(engine);
        return 1;
    }

    engine->pSynthe = (FN_AquesTalk_Synthe)GetProcAddress(engine->hDll, "AquesTalk_Synthe");
    engine->pFreeWave = (FN_AquesTalk_FreeWave)GetProcAddress(engine->hDll, "AquesTalk_FreeWave");
    engine->pCreate = (FN_AquesTalkDa_Create)GetProcAddress(engine->hDaDll, "AquesTalkDa_Create");
    engine->pRelease = (FN_AquesTalkDa_Release)GetProcAddress(engine->hDaDll, "AquesTalkDa_Release");
    engine->pPlay = (FN_AquesTalkDa_Play)GetProcAddress(engine->hDaDll, "AquesTalkDa_Play");
    engine->pStop = (FN_AquesTalkDa_Stop)GetProcAddress(engine->hDaDll, "AquesTalkDa_Stop");
    engine->pIsPlay = (FN_AquesTalkDa_IsPlay)GetProcAddress(engine->hDaDll, "AquesTalkDa_IsPlay");

    if (!engine->pSynthe || !engine->pCreate || !engine->pPlay) {
        unloadAquesTalk(engine);
        return 1;
    }
    return 0;
}

// wavファイル保存
void saveWav(const wchar_t* filename, unsigned char* data, int size) {
    FILE* fp = NULL;
    if (_wfopen_s(&fp, filename, L"wb") == 0 && fp) {
        if (data) fwrite(data, 1, size, fp);
        fclose(fp);
    }
}

// ファイル名指定
BOOL getFileName(TCHAR* filename, int size) {
    OPENFILENAME ofn = { 0 };
    filename[0] = L'\0'; // バッファを初期化

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"WAVファイル (*.wav)\0*.wav\0すべてのファイル (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = size;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"wav";
    return GetSaveFileName(&ofn);
}

struct ThreadData {
    std::wstring text;
    int speed;
    int voiceIndex;
};

// --- 再生スレッド ---
DWORD WINAPI PlayThread(LPVOID lpParam) {
    std::unique_ptr<ThreadData> data(static_cast<ThreadData*>(lpParam));

    int nLen = WideCharToMultiByte(CP_ACP, 0, data->text.c_str(), -1, NULL, 0, NULL, NULL);
    std::vector<char> ansiText(nLen);
    WideCharToMultiByte(CP_ACP, 0, data->text.c_str(), -1, ansiText.data(), nLen, NULL, NULL);

    EnterCriticalSection(&g_cs);
    if (g_isPlaying || loadAquesTalk(&g_engine, data->voiceIndex) != 0) {
        LeaveCriticalSection(&g_cs);
        return 0;
    }

    g_isPlaying = TRUE;
    g_hMe = g_engine.pCreate();
    LeaveCriticalSection(&g_cs);

    if (g_hMe) {
        if (g_engine.pPlay(g_hMe, ansiText.data(), data->speed, NULL, 0, 0) == 0) {
            while (g_engine.pIsPlay(g_hMe)) { Sleep(10); }
        }
        Sleep(150);

        EnterCriticalSection(&g_cs);
        g_engine.pRelease(g_hMe);
        g_hMe = NULL;
        g_isPlaying = FALSE;
        LeaveCriticalSection(&g_cs);
    }
    else {
        EnterCriticalSection(&g_cs);
        g_isPlaying = FALSE;
        LeaveCriticalSection(&g_cs);
    }

    return 0;
}

std::wstring FormatSrtTime(int totalMs) {
    int ms = totalMs % 1000;
    int totalSec = totalMs / 1000;
    int sec = totalSec % 60;
    int totalMin = totalSec / 60;
    int min = totalMin % 60;
    int hours = totalMin / 60;

    std::wstringstream wss;
    wss << std::setfill(L'0')
        << std::setw(2) << hours << L":"
        << std::setw(2) << min << L":"
        << std::setw(2) << sec << L","
        << std::setw(3) << ms;
    return wss.str();
}

void saveSrt(const wchar_t* wavFilename, const wchar_t* text, int wavSize) {
    // WAVサイズから音声の長さ（ミリ秒）を計算
    // ヘッダサイズ約44バイトを引く
    int dataSize = (wavSize > 44) ? (wavSize - 44) : wavSize;
    int durationMs = static_cast<int>((static_cast<double>(dataSize) / 16000.0) * 1000.0);

    std::wstring srtFilename = wavFilename;
    size_t lastDot = srtFilename.find_last_of(L'.');
    if (lastDot != std::wstring::npos) {
        srtFilename = srtFilename.substr(0, lastDot) + L".srt";
    }
    else {
        srtFilename += L".srt";
    }

    // SRTフォーマットで書き込み
    FILE* fp = NULL;
    if (_wfopen_s(&fp, srtFilename.c_str(), L"w, ccs=UTF-8") == 0 && fp) {
        std::wstring startTime = FormatSrtTime(0); // 0秒からスタート
        std::wstring endTime = FormatSrtTime(durationMs); // 音声の終了まで

        // SRTの書き出し 
        fwprintf_s(fp, L"1\n");
        fwprintf_s(fp, L"%s --> %s\n", startTime.c_str(), endTime.c_str());
        fwprintf_s(fp, L"%s\n\n", text); // 入力したテキスト

        fclose(fp);
    }
}

INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->code == NM_CLICK || ((LPNMHDR)lParam)->code == NM_RETURN)
        {
            PNMLINK pNmlink = (PNMLINK)lParam;
            ShellExecute(NULL, L"open", pNmlink->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
            return (INT_PTR)TRUE;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
    {
        HWND hSlider = GetDlgItem(hDlg, IDC_SLIDER1);
        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELONG(50, 300));
        SendMessage(hSlider, TBM_SETPOS, TRUE, 100);
        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO1);
        const wchar_t* voices[] = { L"f1", L"f2", L"m1", L"m2", L"r1", L"imd1", L"jgr" };
        for (int i = 0; i < 7; i++) {
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)voices[i]);
        }
        SendMessage(hCombo, CB_SETCURSEL, 0, 0);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_EXIT) {
            EndDialog(hDlg, 0);
            return TRUE;
        }

        if (LOWORD(wParam) == ID_ABOUT) {
            DialogBox(
                GetModuleHandle(NULL), 
                MAKEINTRESOURCE(IDD_ABOUTBOX),
                hDlg,
                AboutDlgProc
            );
            return (INT_PTR)TRUE;
        }

        // 再生
        if (LOWORD(wParam) == IDC_BUTTON1 || LOWORD(wParam) == ID_PLAY) {
            wchar_t szBuffer[256];
            GetDlgItemText(hDlg, IDC_EDIT1, szBuffer, 256);

            auto pData = std::make_unique<ThreadData>();
            pData->text = szBuffer;
            pData->speed = (int)SendMessage(GetDlgItem(hDlg, IDC_SLIDER1), TBM_GETPOS, 0, 0);
            pData->voiceIndex = (int)SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0);

            CreateThread(NULL, 0, PlayThread, pData.release(), 0, NULL);
            return TRUE;
        }

        // 停止
        if (LOWORD(wParam) == IDC_BUTTON2 || LOWORD(wParam) == ID_STOP) {
            EnterCriticalSection(&g_cs);
            if (g_hMe && g_isPlaying && g_engine.pStop) {
                g_engine.pStop(g_hMe);
            }
            LeaveCriticalSection(&g_cs);
            return TRUE;
        }

        // WAV保存
        if (LOWORD(wParam) == IDC_BUTTON3 || LOWORD(wParam) == ID_MAKE_WAVE || LOWORD(wParam) == ID_MAKE_WAVE_SUB) {
            wchar_t szBuffer[256];
            GetDlgItemText(hDlg, IDC_EDIT1, szBuffer, 256);
            int voiceIdx = (int)SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0);

            AquesTalkEngine tempEngine = { 0 };
            if (loadAquesTalk(&tempEngine, voiceIdx) == 0) {
                if (!tempEngine.pSynthe || !tempEngine.pFreeWave) {
                    MessageBox(hDlg, L"ライブラリの関数取得に失敗しました。", L"エラー", MB_OK);
                    unloadAquesTalk(&tempEngine);
                    return TRUE;
                }

                int nLen = WideCharToMultiByte(CP_ACP, 0, szBuffer, -1, NULL, 0, NULL, NULL);
                std::vector<char> ansi(nLen);
                WideCharToMultiByte(CP_ACP, 0, szBuffer, -1, ansi.data(), nLen, NULL, NULL);

                int speed = (int)SendMessage(GetDlgItem(hDlg, IDC_SLIDER1), TBM_GETPOS, 0, 0);
                int size = 0;

                unsigned char* wav = tempEngine.pSynthe(ansi.data(), speed, &size);

                if (wav) {
                    wchar_t filename[MAX_PATH];
                    if (getFileName(filename, MAX_PATH)) {
                        saveWav(filename, wav, size);
                        if (LOWORD(wParam) == ID_MAKE_WAVE_SUB) { saveSrt(filename, szBuffer, size); }
                    }
                    tempEngine.pFreeWave(wav);
                }
                else {
                    MessageBox(hDlg, L"音声生成に失敗しました。\n入力が音声記号（カタカナ等）か確認してください。", L"情報", MB_OK);
                }
                unloadAquesTalk(&tempEngine);
            }
            else {
                MessageBox(hDlg, L"ライブラリのロードに失敗しました。", L"エラー", MB_OK);
            }
            return TRUE;
        }
        break;

    case WM_HSCROLL:
        if ((HWND)lParam == GetDlgItem(hDlg, IDC_SLIDER1)) {
            int pos = (int)SendMessage((HWND)lParam, TBM_GETPOS, 0, 0);
            wchar_t buf[10];
            swprintf_s(buf, L"%d", pos);
            SetDlgItemText(hDlg, IDC_EDIT2, buf);
            return TRUE;
        }
        break;
    case WM_CLOSE:
    {
        EndDialog(hDlg, 0);
        return TRUE;
    }
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitializeCriticalSection(&g_cs);
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, DialogProc);
    DeleteCriticalSection(&g_cs);
    return 0;
}