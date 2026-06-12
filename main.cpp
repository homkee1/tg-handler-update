#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <cstdlib> // rand, srand
#include <ctime>   // time
#include <cmath>   // pow
#include <uiautomation.h> // Для способа 2 (UI Automation)

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")

enum UIState {
    UI_DASHBOARD,
    UI_LIST,
    UI_HELP
};

enum SendMethod {
    METHOD_SENDINPUT,     // Стандартный (активное окно)
    METHOD_POSTMESSAGE,   // Фоновый посимвольный (SendMessage/PostMessage)
    METHOD_UIAUTOMATION   // Фоновый умный (UI Automation)
};

struct TypingOptions {
    bool enterBefore;
    int enterBeforeDelay;
    bool enterAfter;
    int enterAfterDelay;
    int typingDelay;
};

// Структура параметров рандомизации
struct RandomSettings {
    bool enabled = false;
    int central = 500;
    int left = 100;
    int right = 1000;
    int deviation = 20; // 1 to 100
};

// Глобальные переменные и синхронизация
HHOOK hHook = NULL;
std::mutex g_mutex;
std::vector<std::wstring> g_phrases;
size_t g_phraseIndex = 0;
UIState g_uiState = UI_DASHBOARD;

// Название активного файла и флаг работы с реестром
std::wstring g_phrasesFile = L"phrases.txt";
bool g_useRegistry = false;

// Конфигурационные параметры
std::wstring g_targetProcess = L"telegram.exe";
bool g_checkWindow = true;
DWORD g_hotkey = VK_F1;
bool g_enterBefore = false;
int g_enterBeforeDelay = 25;
bool g_enterAfter = false;
int g_enterAfterDelay = 15;
int g_typingDelay = 0; 

// Параметры зажатия и триггера
bool g_holdRepeat = false;
int g_holdInterval = 500;
bool g_isHotkeyHeld = false;

bool g_toggleRepeat = false;
int g_toggleInterval = 500;
bool g_isToggleActive = false;

// Новые параметры: Метод отправки и остановка при потере фокуса
SendMethod g_sendMethod = METHOD_SENDINPUT;
bool g_stopOnFocusLoss = true;

// Кэшированный дескриптор целевого окна для оптимизации производительности
HWND g_cachedHwnd = NULL;

// Хранилища настроек рандомизации
RandomSettings g_randTr;
RandomSettings g_randHr;
RandomSettings g_randD;

// Переменные для точного учета времени
int g_accumulatedTimeMs = 0;
int g_currentTargetInterval = 500;
bool g_needNewInterval = true;
bool g_isHotkeyPhysicallyDown = false; // Отслеживание физического состояния клавиши

// Вспомогательные функции реестра
bool RegWriteDWORD(HKEY hKey, const std::wstring& valueName, DWORD value) {
    return RegSetValueExW(hKey, valueName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(DWORD)) == ERROR_SUCCESS;
}

bool RegWriteSZ(HKEY hKey, const std::wstring& valueName, const std::wstring& value) {
    return RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegReadDWORD(HKEY hKey, const std::wstring& valueName, DWORD& value) {
    DWORD size = sizeof(DWORD);
    return RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS;
}

bool RegReadSZ(HKEY hKey, const std::wstring& valueName, std::wstring& value) {
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS) {
        value = buffer;
        return true;
    }
    return false;
}

// Вспомогательные функции путей
std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring GetExecutableDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos + 1);
}

std::wstring GetPhrasesFilePath() {
    std::wstring filename;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        filename = g_phrasesFile;
    }
    return GetExecutableDir() + filename;
}

// Поиск дескриптора окна (HWND) по имени исполняемого файла процесса
struct EnumData {
    std::wstring targetExe;
    HWND foundHwnd;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumData* data = reinterpret_cast<EnumData*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != 0) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (hProcess) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
                std::wstring exeName = GetFileName(path);
                for (auto& c : exeName) c = towlower(c);
                
                std::wstring targetName = data->targetExe;
                for (auto& c : targetName) c = towlower(c);

                if (exeName == targetName && IsWindowVisible(hwnd)) {
                    data->foundHwnd = hwnd;
                    CloseHandle(hProcess);
                    return FALSE; // Окно найдено, прекращаем перебор
                }
            }
            CloseHandle(hProcess);
        }
    }
    return TRUE;
}

HWND FindTargetWindow(const std::wstring& targetExe) {
    EnumData data;
    data.targetExe = targetExe;
    data.foundHwnd = NULL;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return data.foundHwnd;
}

void SaveSettingsToRegistry() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhraseSender", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_mutex);
        RegWriteDWORD(hKey, L"UseRegistry", g_useRegistry ? 1 : 0);
        RegWriteSZ(hKey, L"PhrasesFile", g_phrasesFile);
        RegWriteSZ(hKey, L"TargetProcess", g_targetProcess);
        RegWriteDWORD(hKey, L"CheckWindow", g_checkWindow ? 1 : 0);
        RegWriteDWORD(hKey, L"Hotkey", g_hotkey);
        RegWriteDWORD(hKey, L"EnterBefore", g_enterBefore ? 1 : 0);
        RegWriteDWORD(hKey, L"EnterBeforeDelay", g_enterBeforeDelay);
        RegWriteDWORD(hKey, L"EnterAfter", g_enterAfter ? 1 : 0);
        RegWriteDWORD(hKey, L"EnterAfterDelay", g_enterAfterDelay);
        RegWriteDWORD(hKey, L"TypingDelay", g_typingDelay);
        RegWriteDWORD(hKey, L"HoldRepeat", g_holdRepeat ? 1 : 0);
        RegWriteDWORD(hKey, L"HoldInterval", g_holdInterval);
        RegWriteDWORD(hKey, L"ToggleRepeat", g_toggleRepeat ? 1 : 0);
        RegWriteDWORD(hKey, L"ToggleInterval", g_toggleInterval);

        RegWriteDWORD(hKey, L"SendMethod", static_cast<DWORD>(g_sendMethod));
        RegWriteDWORD(hKey, L"StopOnFocusLoss", g_stopOnFocusLoss ? 1 : 0);

        RegWriteDWORD(hKey, L"RandTr_Enabled", g_randTr.enabled ? 1 : 0);
        RegWriteDWORD(hKey, L"RandTr_Central", g_randTr.central);
        RegWriteDWORD(hKey, L"RandTr_Left", g_randTr.left);
        RegWriteDWORD(hKey, L"RandTr_Right", g_randTr.right);
        RegWriteDWORD(hKey, L"RandTr_Dev", g_randTr.deviation);

        RegWriteDWORD(hKey, L"RandHr_Enabled", g_randHr.enabled ? 1 : 0);
        RegWriteDWORD(hKey, L"RandHr_Central", g_randHr.central);
        RegWriteDWORD(hKey, L"RandHr_Left", g_randHr.left);
        RegWriteDWORD(hKey, L"RandHr_Right", g_randHr.right);
        RegWriteDWORD(hKey, L"RandHr_Dev", g_randHr.deviation);

        RegWriteDWORD(hKey, L"RandD_Enabled", g_randD.enabled ? 1 : 0);
        RegWriteDWORD(hKey, L"RandD_Central", g_randD.central);
        RegWriteDWORD(hKey, L"RandD_Left", g_randD.left);
        RegWriteDWORD(hKey, L"RandD_Right", g_randD.right);
        RegWriteDWORD(hKey, L"RandD_Dev", g_randD.deviation);

        RegCloseKey(hKey);
    }
}

void LoadSettingsFromRegistry() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhraseSender", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DWORD val = 0;
        std::wstring strVal;

        if (RegReadDWORD(hKey, L"UseRegistry", val)) g_useRegistry = (val != 0);
        if (RegReadSZ(hKey, L"PhrasesFile", strVal)) g_phrasesFile = strVal;
        if (RegReadSZ(hKey, L"TargetProcess", strVal)) g_targetProcess = strVal;
        if (RegReadDWORD(hKey, L"CheckWindow", val)) g_checkWindow = (val != 0);
        if (RegReadDWORD(hKey, L"Hotkey", val)) g_hotkey = val;
        if (RegReadDWORD(hKey, L"EnterBefore", val)) g_enterBefore = (val != 0);
        if (RegReadDWORD(hKey, L"EnterBeforeDelay", val)) g_enterBeforeDelay = val;
        if (RegReadDWORD(hKey, L"EnterAfter", val)) g_enterAfter = (val != 0);
        if (RegReadDWORD(hKey, L"EnterAfterDelay", val)) g_enterAfterDelay = val;
        if (RegReadDWORD(hKey, L"TypingDelay", val)) g_typingDelay = val;
        if (RegReadDWORD(hKey, L"HoldRepeat", val)) g_holdRepeat = (val != 0);
        if (RegReadDWORD(hKey, L"HoldInterval", val)) g_holdInterval = val;
        if (RegReadDWORD(hKey, L"ToggleRepeat", val)) g_toggleRepeat = (val != 0);
        if (RegReadDWORD(hKey, L"ToggleInterval", val)) g_toggleInterval = val;

        if (RegReadDWORD(hKey, L"SendMethod", val)) g_sendMethod = static_cast<SendMethod>(val);
        if (RegReadDWORD(hKey, L"StopOnFocusLoss", val)) g_stopOnFocusLoss = (val != 0);

        if (RegReadDWORD(hKey, L"RandTr_Enabled", val)) g_randTr.enabled = (val != 0);
        if (RegReadDWORD(hKey, L"RandTr_Central", val)) g_randTr.central = val;
        if (RegReadDWORD(hKey, L"RandTr_Left", val)) g_randTr.left = val;
        if (RegReadDWORD(hKey, L"RandTr_Right", val)) g_randTr.right = val;
        if (RegReadDWORD(hKey, L"RandTr_Dev", val)) g_randTr.deviation = val;

        if (RegReadDWORD(hKey, L"RandHr_Enabled", val)) g_randHr.enabled = (val != 0);
        if (RegReadDWORD(hKey, L"RandHr_Central", val)) g_randHr.central = val;
        if (RegReadDWORD(hKey, L"RandHr_Left", val)) g_randHr.left = val;
        if (RegReadDWORD(hKey, L"RandHr_Right", val)) g_randHr.right = val;
        if (RegReadDWORD(hKey, L"RandHr_Dev", val)) g_randHr.deviation = val;

        if (RegReadDWORD(hKey, L"RandD_Enabled", val)) g_randD.enabled = (val != 0);
        if (RegReadDWORD(hKey, L"RandD_Central", val)) g_randD.central = val;
        if (RegReadDWORD(hKey, L"RandD_Left", val)) g_randD.left = val;
        if (RegReadDWORD(hKey, L"RandD_Right", val)) g_randD.right = val;
        if (RegReadDWORD(hKey, L"RandD_Dev", val)) g_randD.deviation = val;

        RegCloseKey(hKey);
    }
}

// Вызывается БЕЗ удержания g_mutex во избежание взаимной блокировки
void AutoSave() {
    bool shouldSave = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        shouldSave = g_useRegistry;
    }
    if (shouldSave) {
        SaveSettingsToRegistry();
    }
}

void BootRegistrySettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhraseSender", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"UseRegistry", nullptr, nullptr, reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
            if (val != 0) {
                RegCloseKey(hKey);
                LoadSettingsFromRegistry();
                return;
            }
        }
        RegCloseKey(hKey);
    }
}

// Вспомогательная функция математической генерации случайной задержки
int GetRandomValue(int central, int left, int right, int deviationChance) {
    double x = deviationChance / 100.0;
    double k = 8.0 * (1.0 - x) * (1.0 - x) + 0.15;

    double u = static_cast<double>(rand()) / RAND_MAX;
    double factor = pow(u, k);

    if (rand() % 2 == 0) {
        return central - static_cast<int>((central - left) * factor);
    } else {
        return central + static_cast<int>((right - central) * factor);
    }
}

// Получение активной задержки с учетом возможной рандомизации
int GetActiveInterval(const RandomSettings& randSet, int defaultVal) {
    if (randSet.enabled) {
        return GetRandomValue(randSet.central, randSet.left, randSet.right, randSet.deviation);
    }
    return defaultVal;
}

// Получение текущей побуквенной задержки
int GetCurrentTypingDelay(int defaultDelay) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_randD.enabled) {
        return GetRandomValue(g_randD.central, g_randD.left, g_randD.right, g_randD.deviation);
    }
    return defaultDelay;
}

std::wstring Utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::vector<std::wstring> ReadPhrases(const std::wstring& filePath) {
    std::vector<std::wstring> phrases;
    std::ifstream file(filePath.c_str());
    if (!file.is_open()) return phrases;
    
    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (firstLine) {
            if (line.size() >= 3 && 
                (unsigned char)line[0] == 0x00EF && 
                (unsigned char)line[1] == 0x00BB && 
                (unsigned char)line[2] == 0x00BF) {
                line = line.substr(3);
            }
            firstLine = false;
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            phrases.push_back(Utf8ToWide(line));
        }
    }
    return phrases;
}

void CreateDefaultPhrasesFile(const std::wstring& filePath) {
    std::ifstream check(filePath.c_str());
    if (!check.good()) {
        check.close();
        std::ofstream out(filePath.c_str());
        if (out.is_open()) {
            out << "Привет! Это первая фраза.\n";
            out << "Это вторая фраза.\n";
            out << "А это третья фраза!\n";
        }
    }
}

DWORD ParseKey(const std::wstring& keyStr) {
    std::wstring s = keyStr;
    for (auto& c : s) c = towlower(c);
    if (s == L"f1") return VK_F1;
    if (s == L"f2") return VK_F2;
    if (s == L"f3") return VK_F3;
    if (s == L"f4") return VK_F4;
    if (s == L"f5") return VK_F5;
    if (s == L"f6") return VK_F6;
    if (s == L"f7") return VK_F7;
    if (s == L"f8") return VK_F8;
    if (s == L"f9") return VK_F9;
    if (s == L"f10") return VK_F10;
    if (s == L"f11") return VK_F11;
    if (s == L"f12") return VK_F12;
    if (s == L"enter") return VK_RETURN;
    if (s == L"space") return VK_SPACE;
    try {
        return std::stoi(s);
    } catch (...) {}
    return 0;
}

std::wstring KeyToString(DWORD vk) {
    if (vk == VK_F1) return L"F1";
    if (vk == VK_F2) return L"F2";
    if (vk == VK_F3) return L"F3";
    if (vk == VK_F4) return L"F4";
    if (vk == VK_F5) return L"F5";
    if (vk == VK_F6) return L"F6";
    if (vk == VK_F7) return L"F7";
    if (vk == VK_F8) return L"F8";
    if (vk == VK_F9) return L"F9";
    if (vk == VK_F10) return L"F10";
    if (vk == VK_F11) return L"F11";
    if (vk == VK_F12) return L"F12";
    if (vk == VK_RETURN) return L"Enter";
    if (vk == VK_SPACE) return L"Space";
    return std::to_wstring(vk);
}

// Оптимизация: Потокобезопасный кэш последнего проверенного HWND, 
// защищающий хук клавиатуры от лагов системных вызовов OpenProcess
bool IsTargetWindow(HWND hwnd, const std::wstring& target, bool check) {
    if (!check) return true; 
    if (!hwnd) return false;

    static HWND lastHwnd = NULL;
    static bool lastResult = false;
    static std::wstring lastTarget = L"";
    static bool lastCheck = true;
    static std::mutex cacheMutex;

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (hwnd == lastHwnd && target == lastTarget && check == lastCheck) {
            return lastResult;
        }
    }

    bool result = false;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != 0) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (hProcess) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
                std::wstring exeName = GetFileName(path);
                for (auto& c : exeName) c = towlower(c);
                
                std::wstring targetName = GetFileName(target);
                for (auto& c : targetName) c = towlower(c);

                if (exeName == targetName) {
                    result = true;
                }
            }
            CloseHandle(hProcess);
        }
    }

    if (!result && GetFileName(target) == L"telegram.exe") {
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256) > 0) {
            std::wstring cls(className);
            if (cls.find(L"QWindowIcon") != std::wstring::npos && cls.find(L"Qt") != std::wstring::npos) {
                result = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        lastHwnd = hwnd;
        lastResult = result;
        lastTarget = target;
        lastCheck = check;
    }

    return result;
}

void SendKey(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// ==========================================
// МЕТОДЫ ОТПРАВКИ ТЕКСТА
// ==========================================

// Способ 1: Стандартный SendInput (Требует активности окна)
void SendString(const std::wstring& str, const TypingOptions& opts) {
    if (opts.enterBefore) {
        SendKey(VK_RETURN);
        Sleep(opts.enterBeforeDelay);
    }

    bool useTypingDelay = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        useTypingDelay = (opts.typingDelay > 0) || g_randD.enabled;
    }

    if (useTypingDelay) {
        for (wchar_t ch : str) {
            INPUT down = {};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = ch;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            SendInput(1, &down, sizeof(INPUT));

            int delay = GetCurrentTypingDelay(opts.typingDelay);
            if (delay > 0) Sleep(delay);

            INPUT up = {};
            up.type = INPUT_KEYBOARD;
            up.ki.wScan = ch;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput(1, &up, sizeof(INPUT));

            delay = GetCurrentTypingDelay(opts.typingDelay);
            if (delay > 0) Sleep(delay);
        }
    } else {
        if (str.empty()) return;

        std::vector<INPUT> inputs;
        inputs.reserve(str.size() * 2);

        for (wchar_t ch : str) {
            INPUT down = {};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = ch;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            inputs.push_back(down);

            INPUT up = {};
            up.type = INPUT_KEYBOARD;
            up.ki.wScan = ch;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inputs.push_back(up);
        }

        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

    if (opts.enterAfter) {
        Sleep(opts.enterAfterDelay);
        SendKey(VK_RETURN);
    }
}

// Способ 2: Фоновый ввод через PostMessage/WM_CHAR
void SendStringBackgroundPostMessage(HWND hwnd, const std::wstring& str, const TypingOptions& opts) {
    if (!hwnd) return;

    if (opts.enterBefore) {
        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
        Sleep(opts.enterBeforeDelay);
    }

    for (wchar_t ch : str) {
        PostMessageW(hwnd, WM_CHAR, ch, 0);
        int delay = GetCurrentTypingDelay(opts.typingDelay);
        if (delay > 0) Sleep(delay);
    }

    if (opts.enterAfter) {
        Sleep(opts.enterAfterDelay);
        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
    }
}

// Способ 3: Фоновый ввод через UI Automation (Исправлены дэдлоки и падения COM)
bool SendStringUIAutomation(HWND hwnd, const std::wstring& str, const TypingOptions& opts) {
    if (!hwnd) return false;

    // ОШИБКА 2 ИСПРАВЛЕНА: Используем COINIT_MULTITHREADED (MTA) вместо STA для фоновых потоков без цикла сообщений
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    // ОШИБКА 3 ИСПРАВЛЕНА: Вызовем CoUninitialize только если библиотека успешно инициализирована именно в этом вызове
    bool needUninitialize = (hr == S_OK || hr == S_FALSE);

    bool success = false;
    IUIAutomation* pAutomation = NULL;
    
    HRESULT hrCo = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&pAutomation);
    if (SUCCEEDED(hrCo) && pAutomation) {
        IUIAutomationElement* pWindowElement = NULL;
        hrCo = pAutomation->ElementFromHandle(hwnd, &pWindowElement);
        if (SUCCEEDED(hrCo) && pWindowElement) {
            IUIAutomationCondition* pCondition = NULL;
            VARIANT varProp;
            varProp.vt = VT_I4;
            varProp.lVal = UIA_EditControlTypeId;
            hrCo = pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varProp, &pCondition);
            if (SUCCEEDED(hrCo) && pCondition) {
                IUIAutomationElement* pEditElement = NULL;
                hrCo = pWindowElement->FindFirst(TreeScope_Descendants, pCondition, &pEditElement);
                if (SUCCEEDED(hrCo) && pEditElement) {
                    IUIAutomationValuePattern* pValuePattern = NULL;
                    hrCo = pEditElement->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern);
                    if (SUCCEEDED(hrCo) && pValuePattern) {
                        BSTR bstrVal = SysAllocString(str.c_str());
                        hrCo = pValuePattern->SetValue(bstrVal);
                        if (SUCCEEDED(hrCo)) {
                            success = true;
                        }
                        SysFreeString(bstrVal);
                        pValuePattern->Release();
                    }
                    pEditElement->Release();
                }
                pCondition->Release();
            }
            pWindowElement->Release();
        }
        pAutomation->Release();
    }
    
    if (needUninitialize) {
        CoUninitialize();
    }

    if (success && opts.enterAfter) {
        Sleep(opts.enterAfterDelay);
        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
    }
    return success;
}

// Диспетчер отправки, перенаправляющий вызовы по методам (Исправлена производительность и дэдлоки)
void DispatchSendString(const std::wstring& str, const TypingOptions& opts) {
    SendMethod method;
    std::wstring target;
    HWND hwnd = NULL;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        method = g_sendMethod;
        target = g_targetProcess;
        hwnd = g_cachedHwnd;
    }

    if (method == METHOD_SENDINPUT) {
        SendString(str, opts);
    } else {
        // ОШИБКА 4 (И ДЭДЛОК) ИСПРАВЛЕНЫ: Ищем окно БЕЗ удержания глобального g_mutex!
        // Это предотвращает зависание хука клавиатуры на основном потоке.
        if (!hwnd || !IsWindow(hwnd)) {
            HWND foundHwnd = FindTargetWindow(target); // Тяжелый поиск за пределами мьютекса
            
            std::lock_guard<std::mutex> lock(g_mutex);
            // Проверяем, не изменился ли целевой процесс в консоли во время поиска (защита от race condition)
            if (g_targetProcess == target) {
                g_cachedHwnd = foundHwnd;
                hwnd = foundHwnd;
            } else {
                hwnd = g_cachedHwnd; // Если сменился, берем актуальное значение
            }
        }

        if (hwnd) {
            if (method == METHOD_POSTMESSAGE) {
                SendStringBackgroundPostMessage(hwnd, str, opts);
            } else if (method == METHOD_UIAUTOMATION) {
                if (!SendStringUIAutomation(hwnd, str, opts)) {
                    SendStringBackgroundPostMessage(hwnd, str, opts);
                }
            }
        } else {
            // Если фоновое окно не найдено, выводим в активное в качестве резерва
            SendString(str, opts);
        }
    }
}

// ==========================================

// Парсинг числового диапазона вида "100-500"
bool ParseRange(const std::wstring& str, int& left, int& right) {
    size_t dash = str.find(L'-');
    if (dash == std::wstring::npos) return false;
    try {
        left = std::stoi(str.substr(0, dash));
        right = std::stoi(str.substr(dash + 1));
        if (left > right) std::swap(left, right);
        return true;
    } catch (...) {
        return false;
    }
}

// Токенизация входных строк для гибкого парсинга аргументов
std::vector<std::wstring> Tokenize(const std::wstring& str) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (wchar_t ch : str) {
        if (iswspace(ch)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

// Общий метод парсинга и установки конфигурации рандомизации
bool ProcessRandomSettings(RandomSettings& settings, const std::wstring& arg, std::wstring& feedback, const std::wstring& name) {
    auto tokens = Tokenize(arg);
    if (tokens.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        settings.enabled = !settings.enabled;
        g_accumulatedTimeMs = 0;
        g_needNewInterval = true;
        feedback = name + L" рандомизация: " + (settings.enabled ? L"ВКЛ" : L"ВЫКЛ");
        return true;
    }
    if (tokens.size() == 3) {
        try {
            int central = std::stoi(tokens[0]);
            int left = 0, right = 0;
            if (!ParseRange(tokens[1], left, right)) {
                feedback = L"Ошибка! Неверный формат диапазона. Пример: 100-1000";
                return false;
            }
            int dev = std::stoi(tokens[2]);
            if (dev < 1) dev = 1;
            if (dev > 100) dev = 100;

            if (central < left || central > right) {
                feedback = L"Ошибка! Центр (" + std::to_wstring(central) + L") должен быть внутри диапазона " + std::to_wstring(left) + L"-" + std::to_wstring(right);
                return false;
            }

            std::lock_guard<std::mutex> lock(g_mutex);
            settings.central = central;
            settings.left = left;
            settings.right = right;
            settings.deviation = dev;
            settings.enabled = true;
            g_accumulatedTimeMs = 0;
            g_needNewInterval = true;
            
            feedback = name + L" рандомизация: Центр=" + std::to_wstring(central) + 
                       L", Диапазон=" + std::to_wstring(left) + L"-" + std::to_wstring(right) + 
                       L", Отклонение=" + std::to_wstring(dev) + L"%. [ВКЛ]";
            return true;
        } catch (...) {
            feedback = L"Ошибка! Неверный формат числовых параметров.";
            return false;
        }
    }
    feedback = L"Использование: r" + name + L" [центр] [лево-право] [отклонение 1-100]\nПример: r" + name + L" 500 100-1000 30";
    return false;
}

// Функция очистки консоли через Windows API
void ClearScreen() {
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    COORD homeCoords = { 0, 0 };

    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) return;

    DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;

    if (!FillConsoleOutputCharacterW(hStdOut, L' ', cellCount, homeCoords, &count)) return;
    if (!FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count)) return;

    SetConsoleCursorPosition(hStdOut, homeCoords);
}

void RedrawUI() {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    ClearScreen();
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    WORD originalAttrs = csbi.wAttributes;

    if (g_uiState == UI_LIST) {
        std::wcout << L"=== Список загруженных фраз ===" << std::endl;
        size_t total = g_phrases.size();
        for (size_t i = 0; i < total; ++i) {
            if (total > 0 && i == g_phraseIndex % total) {
                std::wcout << (i + 1) << L": ";
                SetConsoleTextAttribute(hConsole, BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);
                std::wcout << g_phrases[i];
                SetConsoleTextAttribute(hConsole, originalAttrs);
                std::wcout << L" <--- [ТЕКУЩАЯ]" << std::endl;
            } else {
                std::wcout << (i + 1) << L": " << g_phrases[i] << std::endl;
            }
        }
        std::wcout << L"--------------------------------" << std::endl;
        std::wcout << L"Введите любую команду или нажмите Enter для возврата к панели." << std::endl;
        std::wcout << L"> " << std::flush;
        return;
    }
    
    if (g_uiState == UI_HELP) {
        std::wcout << L"=== Справка по коротким командам ===" << std::endl;
        std::wcout << L"g <N>       - Абсолютный переход к фразе №N (напр: g 5)" << std::endl;
        std::wcout << L"b <N>       - Откат назад на N фраз (напр: b 2). По умолч: b 1" << std::endl;
        std::wcout << L"f <N>       - Смещение вперед на N фраз (напр: f 3). По умолч: f 1" << std::endl;
        std::wcout << L"r           - Сбросить счетчик к началу (на фразу №1)" << std::endl;
        std::wcout << L"w <process> - Изменить имя процесса (напр: w notepad.exe)" << std::endl;
        std::wcout << L"wc          - Вкл/Выкл проверку активного окна" << std::endl;
        std::wcout << L"k <key>     - Изменить горячую клавишу (напр: k f2, k enter)" << std::endl;
        std::wcout << L"rl          - Перезагрузить файл фраз" << std::endl;
        std::wcout << L"file <name> - Сменить активный файл (напр: file words.txt)" << std::endl;
        std::wcout << L"eb [ms]     - Переключить Enter ДО или задать задержку (напр: eb 25)" << std::endl;
        std::wcout << L"ea [ms]     - Переключить Enter ПОСЛЕ или задать задержку (напр: ea 15)" << std::endl;
        std::wcout << L"d <ms>      - Посимвольный ввод (0 - мгновенно, >0 - задержка в мс)" << std::endl;
        std::wcout << L"hr [ms]     - Вкл режим зажатия или задать интервал повтора (напр: hr 500)" << std::endl;
        std::wcout << L"tr [ms]     - Вкл режим триггера или задать интервал повтора (напр: tr 500)" << std::endl;
        std::wcout << L"rtr/rhr/rd  - Настройка рандомизации для tr, hr или d соответственно" << std::endl;
        std::wcout << L"              (напр: rtr 500 100-1000 30 | без аргументов — Вкл/Выкл)" << std::endl;
        std::wcout << L"sm <method> - Сменить метод отправки. Доступно: si (SendInput), pm (PostMessage), uia (UI Automation)" << std::endl;
        std::wcout << L"sfl         - Вкл/Выкл остановку цикла повтора при потере фокуса" << std::endl;
        std::wcout << L"reg         - Включить/Выключить автоматическое сохранение настроек в реестр" << std::endl;
        std::wcout << L"df / resetcfg - Сбросить конфигурацию к заводским настройкам" << std::endl;
        std::wcout << L"l           - Посмотреть пронумерованный список фраз" << std::endl;
        std::wcout << L"q           - Закрыть программу" << std::endl;
        std::wcout << L"------------------------------------" << std::endl;
        std::wcout << L"Введите любую команду или нажмите Enter для возврата к панели." << std::endl;
        std::wcout << L"> " << std::flush;
        return;
    }

    size_t total = g_phrases.size();
    std::wstring prev = L"[Нет фраз]";
    std::wstring curr = L"[Нет фраз]";
    std::wstring next = L"[Нет фраз]";

    if (total > 0) {
        size_t idx = g_phraseIndex;
        size_t curr_idx = idx % total;
        size_t prev_idx = (idx + total - 1) % total;
        size_t next_idx = (idx + 1) % total;

        prev = g_phrases[prev_idx];
        curr = g_phrases[curr_idx];
        next = g_phrases[next_idx];
    }

    std::wcout << L"Предыдущая: " << prev << std::endl;
    
    std::wcout << L"ТЕКУЩАЯ   : ";
    SetConsoleTextAttribute(hConsole, BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);
    std::wcout << curr;
    SetConsoleTextAttribute(hConsole, originalAttrs);
    std::wcout << std::endl;

    std::wcout << L"Следующая : " << next << std::endl;
    std::wcout << L"----------------------------------------" << std::endl;
    
    std::wstring modeStr = L"Одиночный";
    if (g_holdRepeat) {
        modeStr = L"Удержание (Зажатие) [интервал: ";
        if (g_randHr.enabled) {
            modeStr += L"Ранд. " + std::to_wstring(g_randHr.central) + L"мс (" + std::to_wstring(g_randHr.left) + L"-" + std::to_wstring(g_randHr.right) + L", откл. " + std::to_wstring(g_randHr.deviation) + L"%)]";
        } else {
            modeStr += std::to_wstring(g_holdInterval) + L"мс]";
        }
    } else if (g_toggleRepeat) {
        modeStr = L"Триггер (Toggle) [интервал: ";
        if (g_randTr.enabled) {
            modeStr += L"Ранд. " + std::to_wstring(g_randTr.central) + L"мс (" + std::to_wstring(g_randTr.left) + L"-" + std::to_wstring(g_randTr.right) + L", откл. " + std::to_wstring(g_randTr.deviation) + L"%)]";
        } else {
            modeStr += std::to_wstring(g_toggleInterval) + L"мс]";
        }
        if (g_isToggleActive) {
            modeStr += L" [АКТИВЕН]";
        } else {
            modeStr += L" [ОСТАНОВЛЕН]";
        }
    }

    std::wstring dStr = L"Выкл";
    if (g_randD.enabled) {
        dStr = L"Ранд. " + std::to_wstring(g_randD.central) + L"мс (" + std::to_wstring(g_randD.left) + L"-" + std::to_wstring(g_randD.right) + L", откл. " + std::to_wstring(g_randD.deviation) + L"%)";
    } else if (g_typingDelay > 0) {
        dStr = std::to_wstring(g_typingDelay) + L"мс";
    } else {
        dStr = L"Мгновенно";
    }

    std::wstring methodStr = L"SendInput (Активное окно)";
    if (g_sendMethod == METHOD_POSTMESSAGE) methodStr = L"PostMessage (Фоновый ввод)";
    else if (g_sendMethod == METHOD_UIAUTOMATION) methodStr = L"UI Automation (Фоновый умный)";

    std::wcout << L"[Инфо ] ";
    std::wcout << L"Индекс: " << (total > 0 ? (g_phraseIndex % total) + 1 : 0) << L"/" << total << L" (Абс: " << g_phraseIndex << L") | ";
    std::wcout << L"Кнопка: " << KeyToString(g_hotkey) << L" | Файл: " << g_phrasesFile << std::endl;
    std::wcout << L"[Режим] " << modeStr << L" | Окно: " << (g_checkWindow ? g_targetProcess : L"БЕЗ ПРОВЕРКИ") << L" [Стоп-фокус: " << (g_stopOnFocusLoss ? L"ВКЛ" : L"ВЫКЛ") << L"]" << std::endl;
    
    std::wcout << L"[Опции] ";
    std::wcout << L"Enter до: " << (g_enterBefore ? (std::to_wstring(g_enterBeforeDelay) + L"мс") : L"Выкл")
               << L" | Enter после: " << (g_enterAfter ? (std::to_wstring(g_enterAfterDelay) + L"мс") : L"Выкл")
               << L" | Задержка букв: " << dStr << L" | Реестр: " << (g_useRegistry ? L"ВКЛ" : L"ВЫКЛ") << std::endl;
    std::wcout << L"[Метод] " << methodStr << std::endl;
    
    std::wcout << L"----------------------------------------" << std::endl;
    std::wcout << L"Введите 'h' для вывода списка всех коротких команд." << std::endl;
    std::wcout << L"> " << std::flush;
}

// Единый фоновый поток для точного отсчета времени
void WorkerThreadFunc() {
    const int stepMs = 10; // Шаг проверки таймера
    while (true) {
        Sleep(stepMs);

        bool isActive = false;
        int defaultInterval = 500;
        RandomSettings randSet;
        std::wstring phrase;
        TypingOptions opts;
        bool lostWindow = false;

        std::wstring target;
        bool check = true;
        bool stopOnFocusLoss = true;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_holdRepeat) {
                isActive = g_isHotkeyHeld;
                defaultInterval = g_holdInterval;
                randSet = g_randHr;
            } else if (g_toggleRepeat) {
                isActive = g_isToggleActive;
                defaultInterval = g_toggleInterval;
                randSet = g_randTr;
            }
            target = g_targetProcess;
            check = g_checkWindow;
            stopOnFocusLoss = g_stopOnFocusLoss;
        }

        // Проверяем потерю фокуса, только если включена опция g_stopOnFocusLoss
        if (isActive && stopOnFocusLoss) {
            if (!IsTargetWindow(GetForegroundWindow(), target, check)) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_isHotkeyHeld = false;
                g_isToggleActive = false;
                g_accumulatedTimeMs = 0;
                g_needNewInterval = true;
                isActive = false;
                lostWindow = true;
            }
        }

        if (isActive) {
            std::lock_guard<std::mutex> lock(g_mutex);
            
            bool stillActive = false;
            if (g_holdRepeat) stillActive = g_isHotkeyHeld;
            else if (g_toggleRepeat) stillActive = g_isToggleActive;

            if (stillActive) {
                if (g_needNewInterval) {
                    g_currentTargetInterval = GetActiveInterval(randSet, defaultInterval);
                    g_needNewInterval = false;
                }

                g_accumulatedTimeMs += stepMs;

                if (g_accumulatedTimeMs >= g_currentTargetInterval) {
                    if (!g_phrases.empty()) {
                        phrase = g_phrases[g_phraseIndex % g_phrases.size()];
                        g_phraseIndex++;
                        opts = { g_enterBefore, g_enterBeforeDelay, g_enterAfter, g_enterAfterDelay, g_typingDelay };
                    }
                    g_accumulatedTimeMs = 0;
                    g_needNewInterval = true;
                }
            }
        }

        if (lostWindow) {
            RedrawUI();
        }

        if (!phrase.empty()) {
            DispatchSendString(phrase, opts);
            RedrawUI();
        }
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKeyBoard = (KBDLLHOOKSTRUCT*)lParam;
        
        DWORD curHotkey;
        std::wstring target;
        bool check;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            curHotkey = g_hotkey;
            target = g_targetProcess;
            check = g_checkWindow;
        }

        if (pKeyBoard->vkCode == curHotkey) {
            bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            HWND hwnd = GetForegroundWindow();
            bool isTarget = IsTargetWindow(hwnd, target, check);

            if (isKeyDown) {
                bool isRepeat = false;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (g_isHotkeyPhysicallyDown) {
                        isRepeat = true; // Игнорируем автоповтор ОС
                    } else {
                        g_isHotkeyPhysicallyDown = true;
                    }
                }

                if (!isRepeat) {
                    if (isTarget) {
                        std::wstring phrase;
                        TypingOptions opts;
                        bool triggerRedraw = false;

                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            if (g_holdRepeat) {
                                g_isHotkeyHeld = true;
                            } else if (g_toggleRepeat) {
                                g_isToggleActive = !g_isToggleActive;
                                triggerRedraw = true;
                            } else {
                                if (!g_phrases.empty()) {
                                    phrase = g_phrases[g_phraseIndex % g_phrases.size()];
                                    g_phraseIndex++;
                                }
                                opts = { g_enterBefore, g_enterBeforeDelay, g_enterAfter, g_enterAfterDelay, g_typingDelay };
                                g_uiState = UI_DASHBOARD;
                                triggerRedraw = true;
                            }
                        }

                        if (triggerRedraw) {
                            RedrawUI();
                        }

                        if (!phrase.empty()) {
                            std::thread([phrase, opts]() {
                                DispatchSendString(phrase, opts);
                                RedrawUI();
                            }).detach();
                        }
                    }
                }
                if (isTarget) {
                    return 1; // Блокируем нажатие клавиши только в целевом окне
                }
            }
            else if (isKeyUp) {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_isHotkeyPhysicallyDown = false;
                    if (g_holdRepeat) {
                        g_isHotkeyHeld = false;
                    }
                }
                if (isTarget) {
                    return 1; // Блокируем отпускание клавиши только в целевом окне
                }
            }
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

void ConsoleThreadFunc() {
    std::wstring input;
    while (true) {
        if (!std::getline(std::wcin, input)) {
            break;
        }

        while (!input.empty() && iswspace(input.front())) input.erase(input.begin());
        while (!input.empty() && iswspace(input.back())) input.pop_back();

        if (input.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_uiState != UI_DASHBOARD) {
                    g_uiState = UI_DASHBOARD;
                }
            } 
            RedrawUI();
            continue;
        }

        size_t space_pos = input.find(L' ');
        std::wstring cmd = (space_pos == std::wstring::npos) ? input : input.substr(0, space_pos);
        std::wstring arg = (space_pos == std::wstring::npos) ? L"" : input.substr(space_pos + 1);
        for (auto& c : cmd) c = towlower(c);

        std::wstring feedback = L"";
        UIState nextState = UI_DASHBOARD; 

        if (cmd == L"q" || cmd == L"exit") {
            // ОШИБКА 1 ИСПРАВЛЕНА: Чистим хук и корректно выходим из процесса через ExitProcess, так как это фоновый поток
            if (hHook) {
                UnhookWindowsHookEx(hHook);
                hHook = NULL;
            }
            ExitProcess(0);
            break;
        }
        else if (cmd == L"g" || cmd == L"goto") {
            try {
                int val = std::stoi(arg);
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_phrases.empty()) {
                    if (val < 1) val = 1;
                    g_phraseIndex = static_cast<size_t>(val - 1);
                    feedback = L"Индекс установлен на фразу #" + std::to_wstring(val);
                } else {
                    feedback = L"Список фраз пуст!";
                }
            } catch (...) {
                feedback = L"Ошибка! Укажите число после g, например: g 3";
            }
        }
        else if (cmd == L"b" || cmd == L"back") {
            int steps = 1;
            try { if (!arg.empty()) steps = std::stoi(arg); } catch (...) {}
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_phrases.empty()) {
                size_t total = g_phrases.size();
                size_t current = g_phraseIndex % total;
                g_phraseIndex = (current + total - (steps % total)) % total;
                feedback = L"Откат назад на " + std::to_wstring(steps) + L" поз.";
            } else {
                feedback = L"Список фраз пуст!";
            }
        }
        else if (cmd == L"f" || cmd == L"forward") {
            int steps = 1;
            try { if (!arg.empty()) steps = std::stoi(arg); } catch (...) {}
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_phrases.empty()) {
                size_t total = g_phrases.size();
                size_t current = g_phraseIndex % total;
                g_phraseIndex = (current + steps) % total;
                feedback = L"Смещение вперед на " + std::to_wstring(steps) + L" поз.";
            } else {
                feedback = L"Список фраз пуст!";
            }
        }
        else if (cmd == L"r" || cmd == L"reset") {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_phraseIndex = 0;
            feedback = L"Счетчик сброшен на фразу #1";
        }
        else if (cmd == L"w" || cmd == L"window") {
            if (arg.empty()) {
                feedback = L"Ошибка! Укажите имя процесса, например: w telegram.exe";
            } else {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_targetProcess = arg;
                    g_checkWindow = true;
                    g_cachedHwnd = NULL; // Оптимизация: Сбрасываем кэш HWND при смене процесса
                }
                feedback = L"Целевое окно изменено на: " + arg;
                AutoSave();
            }
        }
        else if (cmd == L"wc" || cmd == L"wincheck") {
            bool currentCheck = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_checkWindow = !g_checkWindow;
                currentCheck = g_checkWindow;
            }
            feedback = L"Проверка активности целевого окна: " + std::wstring(currentCheck ? L"ВКЛ" : L"ВЫКЛ");
            AutoSave();
        }
        else if (cmd == L"k" || cmd == L"key") {
            DWORD parsed = ParseKey(arg);
            if (parsed != 0) {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_hotkey = parsed;
                    g_isHotkeyPhysicallyDown = false;
                    g_isHotkeyHeld = false;
                    g_isToggleActive = false;
                    g_accumulatedTimeMs = 0;
                    g_needNewInterval = true;
                }
                feedback = L"Горячая клавиша изменена на: " + KeyToString(parsed);
                AutoSave();
            } else {
                feedback = L"Неизвестная кнопка! Примеры: f2, f5, enter, space";
            }
        }
        else if (cmd == L"rl" || cmd == L"reload") {
            std::wstring filePath = GetPhrasesFilePath();
            std::vector<std::wstring> loadedPhrases = ReadPhrases(filePath);
            
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_phrases = std::move(loadedPhrases);
                g_phraseIndex = 0;
                g_accumulatedTimeMs = 0;
                g_needNewInterval = true;
                count = g_phrases.size();
            }
            feedback = L"Файл перезагружен! Найдено фраз: " + std::to_wstring(count);
        }
        else if (cmd == L"file") {
            if (arg.empty()) {
                feedback = L"Ошибка! Укажите имя файла, например: file words.txt";
            } else {
                std::wstring newFile = arg;
                if (newFile.size() < 4 || newFile.substr(newFile.size() - 4) != L".txt") {
                    newFile += L".txt";
                }
                std::wstring filePath = GetExecutableDir() + newFile;
                CreateDefaultPhrasesFile(filePath);
                std::vector<std::wstring> loadedPhrases = ReadPhrases(filePath);

                size_t count = 0;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_phrasesFile = newFile;
                    g_phrases = std::move(loadedPhrases);
                    
                    if (g_phraseIndex >= g_phrases.size()) {
                        g_phraseIndex = 0;
                    }
                    g_accumulatedTimeMs = 0;
                    g_needNewInterval = true;
                    g_cachedHwnd = NULL; // Оптимизация: Сбрасываем кэш HWND
                    count = g_phrases.size();
                }
                feedback = L"Активный файл изменен на: " + newFile + L" (Найдено фраз: " + std::to_wstring(count) + L")";
                AutoSave();
            }
        }
        else if (cmd == L"eb" || cmd == L"enterbefore") {
            bool ebVal;
            int ebDelay;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (arg.empty()) {
                    g_enterBefore = !g_enterBefore;
                } else {
                    try {
                        g_enterBeforeDelay = std::stoi(arg);
                        g_enterBefore = true;
                    } catch (...) {}
                }
                ebVal = g_enterBefore;
                ebDelay = g_enterBeforeDelay;
            }
            feedback = L"Enter ДО ввода: " + std::wstring(ebVal ? L"ВКЛ (" + std::to_wstring(ebDelay) + L"мс)" : L"ВЫКЛ");
            AutoSave();
        }
        else if (cmd == L"ea" || cmd == L"enterafter") {
            bool eaVal;
            int eaDelay;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (arg.empty()) {
                    g_enterAfter = !g_enterAfter;
                } else {
                    try {
                        g_enterAfterDelay = std::stoi(arg);
                        g_enterAfter = true;
                    } catch (...) {}
                }
                eaVal = g_enterAfter;
                eaDelay = g_enterAfterDelay;
            }
            feedback = L"Enter ПОСЛЕ ввода: " + std::wstring(eaVal ? L"ВКЛ (" + std::to_wstring(eaDelay) + L"мс)" : L"ВЫКЛ");
            AutoSave();
        }
        else if (cmd == L"d" || cmd == L"delay") {
            try {
                int delayVal = std::stoi(arg);
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_typingDelay = delayVal;
                }
                feedback = L"Задержка букв: " + std::to_wstring(delayVal) + L"мс";
                AutoSave();
            } catch (...) {
                feedback = L"Ошибка! Укажите число задержки (в мс). Пример: d 10";
            }
        }
        else if (cmd == L"hr" || cmd == L"holdrepeat") {
            bool hrVal;
            int hrInterval;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (arg.empty()) {
                    g_holdRepeat = !g_holdRepeat;
                } else {
                    try {
                        g_holdInterval = std::stoi(arg);
                        if (g_holdInterval < 10) g_holdInterval = 10;
                        g_holdRepeat = true;
                    } catch (...) {}
                }
                if (g_holdRepeat) {
                    g_toggleRepeat = false; 
                    g_isToggleActive = false;
                }
                g_isHotkeyHeld = false;
                g_isHotkeyPhysicallyDown = false;
                g_accumulatedTimeMs = 0;
                g_needNewInterval = true;
                hrVal = g_holdRepeat;
                hrInterval = g_holdInterval;
            }
            feedback = L"Режим удержания (Hold Repeat): " + std::wstring(hrVal ? L"ВКЛ (" + std::to_wstring(hrInterval) + L"мс)" : L"ВЫКЛ");
            AutoSave();
        }
        else if (cmd == L"tr" || cmd == L"togglerepeat") {
            bool trVal;
            int trInterval;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (arg.empty()) {
                    g_toggleRepeat = !g_toggleRepeat;
                } else {
                    try {
                        g_toggleInterval = std::stoi(arg);
                        if (g_toggleInterval < 10) g_toggleInterval = 10;
                        g_toggleRepeat = true;
                    } catch (...) {}
                }
                if (g_toggleRepeat) {
                    g_holdRepeat = false; 
                    g_isHotkeyHeld = false;
                }
                g_isToggleActive = false; 
                g_isHotkeyPhysicallyDown = false;
                g_accumulatedTimeMs = 0;
                g_needNewInterval = true;
                trVal = g_toggleRepeat;
                trInterval = g_toggleInterval;
            }
            feedback = L"Режим триггера (Toggle Repeat): " + std::wstring(trVal ? L"ВКЛ (" + std::to_wstring(trInterval) + L"мс)" : L"ВЫКЛ");
            AutoSave();
        }
        else if (cmd == L"rtr") {
            ProcessRandomSettings(g_randTr, arg, feedback, L"tr");
            AutoSave();
        }
        else if (cmd == L"rhr") {
            ProcessRandomSettings(g_randHr, arg, feedback, L"hr");
            AutoSave();
        }
        else if (cmd == L"rd") {
            ProcessRandomSettings(g_randD, arg, feedback, L"d");
            AutoSave();
        }
        else if (cmd == L"sm" || cmd == L"sendmethod") {
            std::wstring methodStr = arg;
            for (auto& c : methodStr) c = towlower(c);

            bool isMethodChanged = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (methodStr == L"si" || methodStr == L"sendinput" || methodStr == L"1") {
                    g_sendMethod = METHOD_SENDINPUT;
                    feedback = L"Метод отправки изменен на: SendInput (активное окно)";
                    isMethodChanged = true;
                } else if (methodStr == L"pm" || methodStr == L"postmessage" || methodStr == L"2") {
                    g_sendMethod = METHOD_POSTMESSAGE;
                    feedback = L"Метод отправки изменен на: PostMessage (фоновый ввод)";
                    isMethodChanged = true;
                } else if (methodStr == L"uia" || methodStr == L"uiautomation" || methodStr == L"3") {
                    g_sendMethod = METHOD_UIAUTOMATION;
                    feedback = L"Метод отправки изменен на: UI Automation (фоновый умный)";
                    isMethodChanged = true;
                } else {
                    feedback = L"Ошибка! Неизвестный метод. Доступны: si (SendInput), pm (PostMessage), uia (UI Automation)";
                }
            } // Локальная область видимости закрыта — мьютекс безопасно освобождается перед AutoSave()

            if (isMethodChanged) {
                AutoSave();
            }
        }
        else if (cmd == L"sfl" || cmd == L"stopfocus") {
            bool currentSfl = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stopOnFocusLoss = !g_stopOnFocusLoss;
                currentSfl = g_stopOnFocusLoss;
            }
            feedback = L"Остановка цикла при потере фокуса целевого окна: " + std::wstring(currentSfl ? L"ВКЛЮЧЕНА" : L"ВЫКЛЮЧЕНА");
            AutoSave();
        }
        else if (cmd == L"reg") {
            bool currentReg = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_useRegistry = !g_useRegistry;
                currentReg = g_useRegistry;
            }
            if (currentReg) {
                SaveSettingsToRegistry();
                feedback = L"Сохранение в реестр ВКЛЮЧЕНО. Настройки записаны.";
            } else {
                HKEY hKey;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhraseSender", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                    DWORD val = 0;
                    RegSetValueExW(hKey, L"UseRegistry", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
                    RegCloseKey(hKey);
                }
                feedback = L"Сохранение в реестр ВЫКЛЮЧЕНО.";
            }
        }
        else if (cmd == L"df" || cmd == L"resetcfg") {
            bool wasUsingRegistry = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_targetProcess = L"telegram.exe";
                g_checkWindow = true;
                g_hotkey = VK_F1;
                g_enterBefore = false;
                g_enterBeforeDelay = 25;
                g_enterAfter = false;
                g_enterAfterDelay = 15;
                g_typingDelay = 0;
                g_holdRepeat = false;
                g_holdInterval = 500;
                g_toggleRepeat = false;
                g_toggleInterval = 500;
                g_isHotkeyHeld = false;
                g_isToggleActive = false;
                g_sendMethod = METHOD_SENDINPUT;
                g_stopOnFocusLoss = true;
                g_cachedHwnd = NULL; // Оптимизация: Сброс кэша HWND
                g_randTr = RandomSettings{};
                g_randHr = RandomSettings{};
                g_randD = RandomSettings{};
                g_accumulatedTimeMs = 0;
                g_needNewInterval = true;
                g_isHotkeyPhysicallyDown = false;
                g_phrasesFile = L"phrases.txt";
                wasUsingRegistry = g_useRegistry;
                g_useRegistry = false;
            }

            std::wstring defaultPath = GetExecutableDir() + L"phrases.txt";
            CreateDefaultPhrasesFile(defaultPath);
            std::vector<std::wstring> loaded = ReadPhrases(defaultPath);

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_phrases = std::move(loaded);
                g_phraseIndex = 0;
            }

            if (wasUsingRegistry) {
                HKEY hKey;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhraseSender", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                    DWORD val = 0;
                    RegSetValueExW(hKey, L"UseRegistry", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
                    RegCloseKey(hKey);
                }
            }

            feedback = L"Настройки сброшены к заводским!";
        }
        else if (cmd == L"l" || cmd == L"list") {
            nextState = UI_LIST;
        }
        else if (cmd == L"h" || cmd == L"?" || cmd == L"help") {
            nextState = UI_HELP;
        }
        else {
            feedback = L"Неизвестная команда '" + cmd + L"'. Введите 'h' для справки.";
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_uiState = nextState;
        }
        
        RedrawUI();
        
        if (!feedback.empty()) {
            std::wcout << L"\n[Результат] " << feedback << std::endl;
            std::wcout << L"> " << std::flush;
        }
    }
}

int main() {
    setlocale(LC_ALL, "Russian");
    std::wcout.imbue(std::locale(""));
    std::wcin.imbue(std::locale(""));
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);

    srand(static_cast<unsigned int>(time(NULL)));

    // Проверяем сохраненную конфигурацию реестра перед инициализацией
    BootRegistrySettings();

    std::wstring phrasesPath = GetPhrasesFilePath();
    CreateDefaultPhrasesFile(phrasesPath);
    
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_phrases = ReadPhrases(phrasesPath);
    }

    RedrawUI();

    std::thread consoleThread(ConsoleThreadFunc);
    consoleThread.detach();

    // Запуск единого фонового потока времени
    std::thread workerThread(WorkerThreadFunc);
    workerThread.detach();

    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!hHook) {
        std::wcerr << L"Ошибка установки хука!" << std::endl;
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hHook);
    return 0;
}