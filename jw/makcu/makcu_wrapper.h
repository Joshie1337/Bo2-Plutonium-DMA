#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "makcu.h"

namespace makcu_wrapper {
    // Global state
    extern std::unique_ptr<makcu::Device> device;
    extern std::atomic<bool> connected;

    // Initialize MAKCU device
    // useHighSpeed: if true, switches to 4Mbaud after connecting (may not work on all devices)
    void MakcuInitialize(const std::string& port = "", bool useHighSpeed = false);

    // Auto-detect connection: tries all common baud rates until a valid MAKCU signature is found
    void MakcuAutoDetect(const std::string& port = "");

    // Get available COM ports for MAKCU devices
    std::vector<std::string> GetAvailablePorts();

    // Mouse movement
    void move(int x, int y);

    // Mouse clicks
    void left_click();
    void left_click_release();

    // Key/button detection (maps to MAKCU mouse buttons)
    void UpdateKeyHistory();
    bool IsDown(int virtual_key);
    bool IsKeyJustPressed(int virtual_key);
    bool IsKeyJustReleased(int virtual_key);

    // Keyboard output via MAKCU HID emulation (sends real key events to gaming PC)
    // Uses USB HID keycodes (not Windows VK codes)
    bool keyDown(uint8_t hidKeycode);
    bool keyUp(uint8_t hidKeycode);
    bool releaseAllKeys();

    // Helper functions for mouse button management
    std::vector<std::string> GetAvailableMouseButtons();
    int GetMouseButtonKeyCode(const std::string& button_name);

    // Connection status
    bool IsConnected();
    std::string GetConnectionStatus();
}