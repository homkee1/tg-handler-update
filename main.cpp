#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <uiautomation.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Uiautomationcore.lib")

#ifndef _O_U16TEXT
#define _O_U16TEXT 0x00020000
#endif

HWND FindTelegramWindow() {
    HWND hwnd = FindWindowW(L"Qt51519QWindowIcon", NULL);
    if (!hwnd) {
        hwnd = FindWindowW(NULL, L"Telegram");
    }
    return hwnd;
}

IUIAutomationElement* FindInputField(IUIAutomation* pAutomation, IUIAutomationElement* pRoot) {
    IUIAutomationCondition* pCond = nullptr;
    VARIANT var;
    var.vt = VT_BSTR;
    var.bstrVal = SysAllocString(L"class Ui::InputField::Inner");
    pAutomation->CreatePropertyCondition(UIA_ClassNamePropertyId, var, &pCond);
    SysFreeString(var.bstrVal);

    IUIAutomationElement* pEdit = nullptr;
    if (pCond) {
        pRoot->FindFirst(TreeScope_Subtree, pCond, &pEdit);
        pCond->Release();
    }
    return pEdit;
}

void WriteInputField(IUIAutomation* pAutomation, IUIAutomationElement* pRoot, const std::wstring& text) {
    IUIAutomationElement* pEdit = FindInputField(pAutomation, pRoot);
    if (!pEdit) {
        std::wcout << L"[-] поле ввода не найдено откройте какой-либо чат" << std::endl;
        return;
    }

    IUIAutomationValuePattern* pValuePattern = nullptr;
    HRESULT hr = pEdit->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern);
    if (SUCCEEDED(hr) && pValuePattern) {
        BSTR bstrText = SysAllocString(text.c_str());
        
        hr = pValuePattern->SetValue(bstrText); 
        
        if (SUCCEEDED(hr)) {
            std::wcout << L"[+] текст записан" << std::endl;
        } else {
            std::wcout << L"[-] ошибка записи текста" << std::endl;
        }
        SysFreeString(bstrText);
        pValuePattern->Release();
    } else {
        std::wcout << L"[-] нет доступа к ValuePattern поля ввода" << std::endl;
    }
    pEdit->Release();
}

void WriteAndSendInputField(IUIAutomation* pAutomation, IUIAutomationElement* pRoot, const std::wstring& text) {
    IUIAutomationElement* pEdit = FindInputField(pAutomation, pRoot);
    if (!pEdit) {
        std::wcout << L"[-] поле ввода не найдено откройте какой-либо чат" << std::endl;
        return;
    }

    std::wstring originalText;
    IUIAutomationValuePattern* pValuePattern = nullptr;
    HRESULT hr = pEdit->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern);
    if (SUCCEEDED(hr) && pValuePattern) {
        BSTR bstrVal = nullptr;
        if (SUCCEEDED(pValuePattern->get_CurrentValue(&bstrVal)) && bstrVal) {
            originalText = bstrVal;
            SysFreeString(bstrVal);
        }

        BSTR bstrText = SysAllocString(text.c_str());
        hr = pValuePattern->SetValue(bstrText); 
        SysFreeString(bstrText);
        pValuePattern->Release();
    } else {
        std::wcout << L"[-] нет доступа к ValuePattern поля ввода" << std::endl;
        pEdit->Release();
        return;
    }
    pEdit->Release();

    if (FAILED(hr)) {
        std::wcout << L"[-] ошибка записи текста" << std::endl;
        return;
    }

    //Sleep(100);

    IUIAutomationCondition* pButtonCond = nullptr;
    VARIANT varButton;
    varButton.vt = VT_I4;
    varButton.lVal = UIA_ButtonControlTypeId;
    pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varButton, &pButtonCond);

    IUIAutomationCondition* pNameSend = nullptr;
    VARIANT varSend;
    varSend.vt = VT_BSTR;
    varSend.bstrVal = SysAllocString(L"Send");
    pAutomation->CreatePropertyCondition(UIA_NamePropertyId, varSend, &pNameSend);
    SysFreeString(varSend.bstrVal);

    IUIAutomationCondition* pNameOtpravit = nullptr;
    VARIANT varOtpravit;
    varOtpravit.vt = VT_BSTR;
    varOtpravit.bstrVal = SysAllocString(L"Отправить");
    pAutomation->CreatePropertyCondition(UIA_NamePropertyId, varOtpravit, &pNameOtpravit);
    SysFreeString(varOtpravit.bstrVal);

    IUIAutomationCondition* pNameOrCond = nullptr;
    pAutomation->CreateOrCondition(pNameSend, pNameOtpravit, &pNameOrCond);
    pNameSend->Release();
    pNameOtpravit->Release();

    IUIAutomationCondition* pFullSendCond = nullptr;
    pAutomation->CreateAndCondition(pButtonCond, pNameOrCond, &pFullSendCond);
    pButtonCond->Release();
    pNameOrCond->Release();

    IUIAutomationElement* pSendButton = nullptr;
    hr = pRoot->FindFirst(TreeScope_Subtree, pFullSendCond, &pSendButton);
    pFullSendCond->Release();

    bool sentSuccessfully = false;
    if (SUCCEEDED(hr) && pSendButton) {
        IUIAutomationInvokePattern* pInvokePattern = nullptr;
        hr = pSendButton->GetCurrentPattern(UIA_InvokePatternId, (IUnknown**)&pInvokePattern);
        if (SUCCEEDED(hr) && pInvokePattern) {
            hr = pInvokePattern->Invoke();
            pInvokePattern->Release();
            std::wcout << L"[+] сообщение отправлено" << std::endl;
            sentSuccessfully = true;
        } else {
            std::wcout << L"[-] нет доступа к InvokePattern кнопки отправки" << std::endl;
        }
        pSendButton->Release();
    } else {
        std::wcout << L"[-] кнопка отправки не найдена (проверьте, что чат открыт)" << std::endl;
    }
    if (!originalText.empty()) {
        if (sentSuccessfully) {
            //Sleep(150);
        }

        IUIAutomationElement* pEditRestore = FindInputField(pAutomation, pRoot);
        if (pEditRestore) {
            IUIAutomationValuePattern* pValuePatternRestore = nullptr;
            if (SUCCEEDED(pEditRestore->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePatternRestore)) && pValuePatternRestore) {
                BSTR bstrOriginal = SysAllocString(originalText.c_str());
                pValuePatternRestore->SetValue(bstrOriginal);
                SysFreeString(bstrOriginal);
                pValuePatternRestore->Release();
            }
            pEditRestore->Release();
        }
    }
}

void ViewInputField(IUIAutomation* pAutomation, IUIAutomationElement* pRoot) {
    IUIAutomationElement* pEdit = FindInputField(pAutomation, pRoot);
    if (!pEdit) {
        std::wcout << L"[-] поле ввода не найдено откройте какой-либо чат" << std::endl;
        return;
    }

    IUIAutomationValuePattern* pValuePattern = nullptr;
    HRESULT hr = pEdit->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern);
    if (SUCCEEDED(hr) && pValuePattern) {
        BSTR bstrVal = nullptr;
        hr = pValuePattern->get_CurrentValue(&bstrVal);
        if (SUCCEEDED(hr) && bstrVal) {
            std::wcout << L"[текущий текст в поле ввода]:\n" << bstrVal << std::endl;
            SysFreeString(bstrVal);
        } else {
            std::wcout << L"[поле ввода сообщения сейчас пусто]" << std::endl;
        }
        pValuePattern->Release();
    } else {
        std::wcout << L"[-] нет доступа к ValuePattern поля ввода" << std::endl;
    }
    pEdit->Release();
}

void ReadChatMessages(IUIAutomation* pAutomation, IUIAutomationElement* pRoot, int count) {
    IUIAutomationCondition* pCond = nullptr;
    VARIANT var;
    var.vt = VT_BSTR;
    var.bstrVal = SysAllocString(L"class HistoryInner");
    pAutomation->CreatePropertyCondition(UIA_ClassNamePropertyId, var, &pCond);
    SysFreeString(var.bstrVal);

    IUIAutomationElement* pHistory = nullptr;
    if (pCond) {
        pRoot->FindFirst(TreeScope_Subtree, pCond, &pHistory);
        pCond->Release();
    }

    if (!pHistory) {
        std::wcout << L"[-] история сообщений (HistoryInner) не найдена. откройте чат" << std::endl;
        return;
    }

    IUIAutomationTreeWalker* pWalker = nullptr;
    std::vector<std::wstring> messages;
    if (SUCCEEDED(pAutomation->get_RawViewWalker(&pWalker)) && pWalker) {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pHistory, &pChild);
        while (SUCCEEDED(hr) && pChild) {
            BSTR bstrName = nullptr;
            pChild->get_CurrentName(&bstrName);
            if (bstrName) {
                messages.push_back(bstrName);
                SysFreeString(bstrName);
            }
            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pHistory->Release();

    if (messages.empty()) {
        std::wcout << L"[-] в текущем чате нет сообщений" << std::endl;
        return;
    }

    int start = std::max(0, (int)messages.size() - count);
    std::wcout << L"\n=== последние " << (messages.size() - start) << L" сообщений ===" << std::endl;
    for (size_t i = start; i < messages.size(); ++i) {
        std::wcout << L"[" << (i + 1) << L"]:\n" << messages[i] << L"\n----------------------------------" << std::endl;
    }
}

void PrintHelp() {
    std::wcout << L"=== справка по консольному управлению тг ===" << std::endl;
    std::wcout << L"h / help          - вывести эту справку" << std::endl;
    std::wcout << L"w / write <текст> - вписать текст в поле ввода сообщения (БЕЗ ФОКУСА)" << std::endl;
    std::wcout << L"s / send <текст>  - отправить текст (сохраняя уже написанный черновик)" << std::endl;
    std::wcout << L"v / view          - глянуть, что сейчас написано в поле ввода" << std::endl;
    std::wcout << L"r / read [кол-во] - считать N последних сообщений (по умолчанию: 5)" << std::endl;
    std::wcout << L"cls / clear       - очистить консоль" << std::endl;
    std::wcout << L"q / exit          - выход" << std::endl;
    std::wcout << L"==================================================" << std::endl;
}

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"[-] ошибка инициализации COM" << std::endl;
        return 1;
    }

    IUIAutomation* pAutomation = nullptr;
    hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) {
        std::wcerr << L"[-] ршибка создания инстанса ui automation" << std::endl;
        CoUninitialize();
        return 1;
    }

    IUIAutomation2* pAutomation2 = nullptr;
    hr = pAutomation->QueryInterface(__uuidof(IUIAutomation2), (void**)&pAutomation2);
    if (SUCCEEDED(hr) && pAutomation2) {
        // указываем не передавать фокус элементам при вызовах методов апи ( типа SetValue, Invoke)
        pAutomation2->put_AutoSetFocus(FALSE);
        pAutomation2->Release();
    }

    std::wcout << L"=== Telegram Quick Automation tqa ===" << std::endl;
    std::wcout << L"введите 'h' для справки.\n" << std::endl;

    std::wstring input;
    while (true) {
        std::wcout << L"TelegramControl> " << std::flush;
        if (!std::getline(std::wcin, input)) break;

        while (!input.empty() && iswspace(input.front())) input.erase(input.begin());
        while (!input.empty() && iswspace(input.back())) input.pop_back();

        if (input.empty()) continue;

        size_t space_pos = input.find(L' ');
        std::wstring cmd = (space_pos == std::wstring::npos) ? input : input.substr(0, space_pos);
        std::wstring arg = (space_pos == std::wstring::npos) ? L"" : input.substr(space_pos + 1);
        for (auto& c : cmd) c = towlower(c);

        if (cmd == L"q" || cmd == L"exit") {
            break;
        }
        else if (cmd == L"h" || cmd == L"help") {
            PrintHelp();
        }
        else if (cmd == L"cls" || cmd == L"clear") {
            HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hStdOut != INVALID_HANDLE_VALUE) {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                DWORD count;
                COORD homeCoords = { 0, 0 };
                if (GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
                    DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
                    FillConsoleOutputCharacterW(hStdOut, L' ', cellCount, homeCoords, &count);
                    FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count);
                    SetConsoleCursorPosition(hStdOut, homeCoords);
                }
            }
        }
        else {
            HWND hwnd = FindTelegramWindow();
            if (!hwnd) {
                std::wcout << L"[-] действие невозможно: тг не запущен." << std::endl;
                continue;
            }

            IUIAutomationElement* pRootElement = nullptr;
            hr = pAutomation->ElementFromHandle(hwnd, &pRootElement);
            if (FAILED(hr) || !pRootElement) {
                std::wcout << L"[-] не удалось подключиться к дереву элементов тг." << std::endl;
                continue;
            }

            if (cmd == L"w" || cmd == L"write") {
                if (arg.empty()) {
                    std::wcout << L"[-] укажите текст для ввода, например: write Привет" << std::endl;
                } else {
                    WriteInputField(pAutomation, pRootElement, arg);
                }
            }
            else if (cmd == L"s" || cmd == L"send") {
                if (arg.empty()) {
                    std::wcout << L"[-] укажите текст для отправки, например: send Привет" << std::endl;
                } else {
                    WriteAndSendInputField(pAutomation, pRootElement, arg);
                }
            }
            else if (cmd == L"v" || cmd == L"view") {
                ViewInputField(pAutomation, pRootElement);
            }
            else if (cmd == L"r" || cmd == L"read") {
                int count = 5;
                if (!arg.empty()) {
                    try { count = std::stoi(arg); } catch (...) {}
                }
                ReadChatMessages(pAutomation, pRootElement, count);
            }
            else {
                std::wcout << L"[-] неизвестная команда '" << cmd << L"'. введите 'h' для справки" << std::endl;
            }

            pRootElement->Release();
        }
    }

    pAutomation->Release();
    CoUninitialize();
    return 0;
}