#include "makcu_wrapper.h"
#include <windows.h>
#include <string>
#include <cstdio>
#include <thread>
#include <atomic>

namespace makcu_wrapper {
    static HANDLE hSerial = INVALID_HANDLE_VALUE;
    std::atomic<bool> connected(false);
    std::unique_ptr<makcu::Device> device;

    static std::string lastError;

    // Flat arrays instead of std::map — O(1) lookup, no heap allocation
    static bool s_curState   [256] = {};
    static bool s_prevState  [256] = {};
    static bool s_justPressed[256] = {};
    static bool s_justReleased[256] = {};

    // ── Button state reader ────────────────────────────────────────────────
    // Bitmask updated by background thread from MAKCU serial stream.
    // bit 0 = LMB, bit 1 = RMB, bit 2 = MMB, bit 3 = X1, bit 4 = X2
    static std::atomic<uint8_t> s_buttonMask{ 0 };
    static std::atomic<bool>    s_readerStop{ false };
    static std::thread          s_readerThread;

    static void ButtonReaderThread() {
        uint8_t buf[64];
        DWORD bytesRead;
        while (!s_readerStop.load() && hSerial != INVALID_HANDLE_VALUE) {
            COMSTAT cs{}; DWORD err;
            if (!ClearCommError(hSerial, &err, &cs) || cs.cbInQue == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            DWORD toRead = cs.cbInQue < sizeof(buf) ? cs.cbInQue : (DWORD)sizeof(buf);
            if (ReadFile(hSerial, buf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                for (DWORD i = 0; i < bytesRead; i++) {
                    uint8_t b = buf[i];
                    // MAKCU button state bytes are raw non-printable bytes < 32,
                    // excluding CR (0x0D) and LF (0x0A)
                    if (b < 32 && b != 0x0D && b != 0x0A) {
                        s_buttonMask.store(b);
                    }
                }
            }
        }
    }

    static void StartReaderThread() {
        s_readerStop.store(false);
        if (s_readerThread.joinable()) s_readerThread.join();
        s_readerThread = std::thread(ButtonReaderThread);
    }

    static void StopReaderThread() {
        s_readerStop.store(true);
        if (s_readerThread.joinable()) s_readerThread.join();
    }

    std::vector<std::string> GetAvailablePorts() {
        std::vector<std::string> result;
        for (int i = 1; i <= 20; i++) {
            std::string portName = "COM" + std::to_string(i);
            std::string fullName = "\\\\.\\" + portName;
            HANDLE h = CreateFileA(fullName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                result.push_back(portName);
            }
        }
        if (result.empty()) {
            result = {"COM1", "COM2", "COM3", "COM4"};
        }
        return result;
    }

    static bool SendCommand(const std::string& cmd) {
        if (hSerial == INVALID_HANDLE_VALUE) return false;
        std::string full = cmd + "\r\n";
        DWORD written;
        return WriteFile(hSerial, full.c_str(), (DWORD)full.length(), &written, NULL) && written > 0;
    }

    void MakcuInitialize(const std::string& port, bool useHighSpeed) {
        connected = false;
        lastError.clear();

        if (port.empty()) {
            lastError = "No port specified";
            return;
        }

        // Close existing connection
        StopReaderThread();
        s_buttonMask.store(0);
        if (hSerial != INVALID_HANDLE_VALUE) {
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            Sleep(100);
        }

        std::string fullPort = "\\\\.\\" + port;
        hSerial = CreateFileA(fullPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        
        if (hSerial == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            lastError = "Failed to open " + port + " (err=" + std::to_string(err) + ")";
            return;
        }

        // Configure serial port
        DCB dcb = {0};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(hSerial, &dcb)) {
            lastError = "GetCommState failed";
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            return;
        }

        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;

        if (!SetCommState(hSerial, &dcb)) {
            lastError = "SetCommState failed";
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            return;
        }

        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 100;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 100;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(hSerial, &timeouts);

        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        // Try high speed if requested
        if (useHighSpeed) {
            BYTE baudCmd[] = {0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00};
            DWORD written;
            WriteFile(hSerial, baudCmd, sizeof(baudCmd), &written, NULL);
            FlushFileBuffers(hSerial);
            Sleep(100);

            CloseHandle(hSerial);
            Sleep(50);

            hSerial = CreateFileA(fullPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hSerial != INVALID_HANDLE_VALUE) {
                dcb.BaudRate = 4000000;
                if (!SetCommState(hSerial, &dcb)) {
                    dcb.BaudRate = 115200;
                    SetCommState(hSerial, &dcb);
                }
                SetCommTimeouts(hSerial, &timeouts);
            }
        }

        if (hSerial == INVALID_HANDLE_VALUE) {
            lastError = "Failed to configure port";
            return;
        }

        // Enable button monitoring.
        // NOTE: km.echo(0) is NOT sent — on many KMBOX/MAKCU firmware variants
        // echo(0) suppresses ALL serial output including button state bytes.
        // Command echo is printable ASCII (>= 32), already filtered by ReaderLoop.
        SendCommand("km.buttons(1)");
        Sleep(30); // let the command take effect and drain any echo response
        PurgeComm(hSerial, PURGE_RXCLEAR); // discard any echo bytes before reader starts

        connected = true;
        lastError = "Connected to " + port + " @ " + std::to_string(dcb.BaudRate);

        // Start background thread to read button state from gaming PC
        StartReaderThread();
    }

    void MakcuAutoDetect(const std::string& port) {
        MakcuInitialize(port, false);
    }

    void move(int x, int y) {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return;
        if (x < -127) x = -127; if (x > 127) x = 127;
        if (y < -127) y = -127; if (y > 127) y = 127;
        // Static buffer avoids snprintf heap alloc on every aimbot dispatch
        static char cmd[24];
        int len = snprintf(cmd, sizeof(cmd) - 2, "km.move(%d,%d)", x, y);
        cmd[len]   = '\r'; cmd[len+1] = '\n';
        DWORD written;
        WriteFile(hSerial, cmd, len + 2, &written, NULL);
    }

    void left_click() {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return;
        SendCommand("km.left(1)");
    }

    void left_click_release() {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return;
        SendCommand("km.left(0)");
    }

    void UpdateKeyHistory() {
        static const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        uint8_t mask = s_buttonMask.load(std::memory_order_relaxed);
        for (int k = 0; k < 5; k++) {
            int vk = keys[k];
            int bit = k;  // bits: 0=LMB,1=RMB,2=MMB,3=X1,4=X2
            bool cur = connected ? ((mask >> bit) & 1) != 0
                                 : ((GetAsyncKeyState(vk) & 0x8000) != 0);
            s_justPressed [vk] = cur && !s_prevState[vk];
            s_justReleased[vk] = !cur && s_prevState[vk];
            s_prevState   [vk] = cur;
            s_curState    [vk] = cur;
        }
    }

    bool IsDown(int virtual_key) {
        if (virtual_key < 256 && s_curState[virtual_key]) return true;
        // MAKCU mouse buttons are polled by UpdateKeyHistory — check direct mask for safety
        int bit = -1;
        switch (virtual_key) {
            case VK_LBUTTON:  bit = 0; break;
            case VK_RBUTTON:  bit = 1; break;
            case VK_MBUTTON:  bit = 2; break;
            case VK_XBUTTON1: bit = 3; break;
            case VK_XBUTTON2: bit = 4; break;
        }
        if (bit >= 0 && connected)
            return (s_buttonMask.load() & (1 << bit)) != 0;
        return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
    }

    bool IsKeyJustPressed(int virtual_key) {
        if (virtual_key >= 0 && virtual_key < 256)
            return s_justPressed[virtual_key];
        return false;
    }

    bool IsKeyJustReleased(int virtual_key) {
        if (virtual_key >= 0 && virtual_key < 256)
            return s_justReleased[virtual_key];
        return false;
    }

    std::vector<std::string> GetAvailableMouseButtons() {
        return {"Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5"};
    }

    int GetMouseButtonKeyCode(const std::string& button_name) {
        if (button_name == "Left Mouse") return 1;
        if (button_name == "Right Mouse") return 2;
        if (button_name == "Middle Mouse") return 4;
        if (button_name == "Mouse 4") return 5;
        if (button_name == "Mouse 5") return 6;
        return 2;
    }

    bool IsConnected() {
        return connected && hSerial != INVALID_HANDLE_VALUE;
    }

    bool keyDown(uint8_t hidKeycode) {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return false;
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "km.kdown(%d)", hidKeycode);
        return SendCommand(cmd);
    }

    bool keyUp(uint8_t hidKeycode) {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return false;
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "km.kup(%d)", hidKeycode);
        return SendCommand(cmd);
    }

    bool releaseAllKeys() {
        if (!connected || hSerial == INVALID_HANDLE_VALUE) return false;
        return SendCommand("km.kup(0)");
    }

    std::string GetConnectionStatus() {
        return lastError;
    }
}
