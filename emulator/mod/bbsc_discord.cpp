// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC Discord status: "Playing BBSC - In Central Yharnam - Hosting - Arcane build (2 of 5)", the
// boss and a fight timer during a boss fight, "Dead - watching hunter 1 of 2" while spectating.
// Talks to the Discord app's local pipe (\\.\pipe\discord-ipc-N; on Linux the socket
// $XDG_RUNTIME_DIR/discord-ipc-N) itself - no Discord library: a frame is {u32 opcode, u32 length, JSON}; opcode 0 = handshake with the application ID, opcode 1 =
// a command (SET_ACTIVITY). The game state comes from bloodborne_re.cpp's game tick
// (GetBbscPresence). Off with the Mods tab switch "Discord status" (SHADPS4_BBSC_DISCORD=0).
// No Discord running = nothing happens; it tries again every 20 s.
// 2026-10-20: compiled into the emulator build too (before, only the mod showed it - the shadPS4
// package had only shadPS4's own "Playing Bloodborne"); the emulator then leaves its own status off.

#include "core/bloodborne_re.h"

#if defined(_WIN32) || defined(__linux__)

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/bbsc_persistence.h"

namespace {

constexpr const char* DiscordApplicationId = "1554935855674691645"; // "BBSC" (CLS_AMG)

std::atomic<bool> discord_running{false};

bool DiscordStatusEnabled() {
    const char* value = std::getenv("SHADPS4_BBSC_DISCORD");
    return value == nullptr || (std::string{value} != "0" && std::string{value} != "false");
}

std::string Json(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + '"';
}

class DiscordPipe {
public:
    ~DiscordPipe() {
        Close();
    }

#ifdef _WIN32
    bool Connect() {
        for (int i = 0; i < 10; ++i) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                break;
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }
        if (!Send(0, fmt::format("{{\"v\":1,\"client_id\":\"{}\"}}", DiscordApplicationId))) {
            Close();
            return false;
        }
        return Drain(true);
    }

    bool Send(std::uint32_t opcode, const std::string& json) {
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }
        std::string frame(8, '\0');
        const auto length = static_cast<std::uint32_t>(json.size());
        std::memcpy(frame.data(), &opcode, 4);
        std::memcpy(frame.data() + 4, &length, 4);
        frame += json;
        DWORD written = 0;
        return WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
               written == frame.size();
    }

    // Reads whatever Discord sent (answers, READY) so the pipe never fills. wait = block for one
    // frame (the handshake answer). False = the pipe is gone or Discord closed it (opcode 2).
    bool Drain(bool wait) {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                Close();
                return false;
            }
            if (available < 8 && !wait) {
                return true;
            }
            std::uint32_t header[2]{};
            DWORD got = 0;
            if (!ReadFile(pipe, header, 8, &got, nullptr) || got != 8) {
                Close();
                return false;
            }
            std::string body(header[1], '\0');
            if (header[1] != 0 &&
                (!ReadFile(pipe, body.data(), header[1], &got, nullptr) || got != header[1])) {
                Close();
                return false;
            }
            if (header[0] == 2) { // CLOSE: wrong application ID or Discord shutting down
                LOG_INFO(Debug, "Discord status: Discord closed the connection: {}", body);
                Close();
                return false;
            }
            wait = false;
        }
    }

    bool Connected() const {
        return pipe != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    // The socket is in the runtime folder, or in the folder of a Flatpak / Snap Discord.
    bool Connect() {
        std::vector<std::string> folders;
        for (const char* env : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            if (const char* value = std::getenv(env); value != nullptr && value[0] != '\0') {
                folders.emplace_back(value);
            }
        }
        folders.emplace_back("/tmp");
        for (const std::string& folder : folders) {
            for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord"}) {
                for (int i = 0; i < 10; ++i) {
                    const std::string name = folder + sub + "/discord-ipc-" + std::to_string(i);
                    sockaddr_un address{};
                    if (name.size() >= sizeof(address.sun_path)) {
                        continue;
                    }
                    address.sun_family = AF_UNIX;
                    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
                    socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                    if (socket_fd >= 0 && connect(socket_fd, reinterpret_cast<sockaddr*>(&address),
                                                  sizeof(address)) == 0) {
                        return Handshake();
                    }
                    Close();
                }
            }
        }
        return false;
    }

    bool Send(std::uint32_t opcode, const std::string& json) {
        if (socket_fd < 0) {
            return false;
        }
        std::string frame(8, '\0');
        const auto length = static_cast<std::uint32_t>(json.size());
        std::memcpy(frame.data(), &opcode, 4);
        std::memcpy(frame.data() + 4, &length, 4);
        frame += json;
        // MSG_NOSIGNAL: Discord quitting must not end the game with SIGPIPE.
        return send(socket_fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(frame.size());
    }

    // As the Windows pipe version: reads what Discord sent so the socket never fills.
    bool Drain(bool wait) {
        for (;;) {
            int available = 0;
            if (ioctl(socket_fd, FIONREAD, &available) != 0) {
                Close();
                return false;
            }
            if (available < 8 && !wait) {
                return true;
            }
            std::uint32_t header[2]{};
            if (!ReadAll(header, 8)) {
                Close();
                return false;
            }
            std::string body(header[1], '\0');
            if (header[1] != 0 && !ReadAll(body.data(), header[1])) {
                Close();
                return false;
            }
            if (header[0] == 2) { // CLOSE: wrong application ID or Discord shutting down
                LOG_INFO(Debug, "Discord status: Discord closed the connection: {}", body);
                Close();
                return false;
            }
            wait = false;
        }
    }

    bool Connected() const {
        return socket_fd >= 0;
    }

    void Close() {
        if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
        }
    }

private:
    bool Handshake() {
        if (!Send(0, fmt::format("{{\"v\":1,\"client_id\":\"{}\"}}", DiscordApplicationId))) {
            Close();
            return false;
        }
        return Drain(true);
    }

    bool ReadAll(void* data, size_t size) {
        auto* at = static_cast<char*>(data);
        while (size > 0) {
            const ssize_t got = recv(socket_fd, at, size, 0);
            if (got <= 0) {
                return false;
            }
            at += got;
            size -= static_cast<size_t>(got);
        }
        return true;
    }

    int socket_fd = -1;
#endif
};

std::uint64_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string ActivityJson(const Core::Bloodborne::BbscPresence& p, std::int64_t game_start_s) {
    std::string details;
    std::string state;
    std::string large_text = "Bloodborne Seamless Co-op";
    std::int64_t start = game_start_s;
    if (!p.in_game) {
        details = "At the title screen";
    } else if (!p.boss.empty()) {
        // The timer counts from when the boss bar appeared (Discord shows it as "mm:ss elapsed").
        details = "Boss fight: " + p.boss;
        large_text = "In " + p.area;
        start = p.boss_since_s;
    } else {
        details = "In " + p.area;
    }
    if (p.in_game) {
        state = p.role == 1 ? "Hosting" : p.role == 2 ? "Co-op guest" : "Solo hunter";
        if (p.dead) {
            if (p.spectating && p.watch_count > 0) {
                state += fmt::format(" - dead, watching hunter {} of {}", p.watching, p.watch_count);
            } else if (p.spectating) {
                state += " - dead, spectating";
            } else {
                state += " - dead";
            }
        } else if (!p.build.empty()) {
            state += " - " + p.build;
        }
        if (!p.build.empty() && (p.dead || !p.boss.empty())) {
            large_text += " - " + p.build;
        }
    }
    std::string activity = "{\"details\":" + Json(details);
    if (!state.empty()) {
        activity += ",\"state\":" + Json(state);
    }
    activity += fmt::format(",\"timestamps\":{{\"start\":{}}}", start);
    activity += ",\"assets\":{\"large_image\":\"bbsc\",\"large_text\":" + Json(large_text) + "}";
    if (p.in_game && p.role != 0) {
        activity += fmt::format(",\"party\":{{\"id\":\"bbsc-{}\",\"size\":[{},{}]}}",
                                ProcessId(), p.hunters, p.max_hunters);
    }
    activity += "}";
    return activity;
}

void DiscordThread() {
    const std::int64_t game_start_s = std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    DiscordPipe discord;
    std::string sent;
    std::uint64_t nonce = 0;
    auto next_try = std::chrono::steady_clock::now();
    bool logged_connect = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!discord.Connected()) {
            if (std::chrono::steady_clock::now() < next_try) {
                continue;
            }
            if (!discord.Connect()) {
                next_try = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                continue;
            }
            if (!logged_connect) {
                LOG_INFO(Debug, "Discord status: connected to Discord");
                logged_connect = true;
            }
            sent.clear();
        }
        if (!discord.Drain(false)) {
            continue;
        }
        const std::string activity = ActivityJson(Core::Bloodborne::GetBbscPresence(), game_start_s);
        if (activity == sent) {
            continue;
        }
        const std::string command = fmt::format(
            "{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":{}}},\"nonce\":\"{}\"}}",
            ProcessId(), activity, ++nonce);
        if (discord.Send(1, command)) {
            sent = activity;
        } else {
            discord.Close();
        }
    }
}

} // namespace

namespace Core::Bbsc {

bool DiscordStatusRunning() {
    return discord_running.load();
}

void StartDiscordStatus(std::string_view serial) {
#ifndef BBSC_MOD
    // The emulator build: only for the game BBSC knows, and only while its own BBSC runs (the mod
    // shows the status itself when bbsc-launch switches these off).
    if (serial != "CUSA03173" || !SwitchEnabled("SHADPS4_BLOODBORNE_RE_TRACE") ||
        !SwitchEnabled("SHADPS4_BBSC_PERSIST")) {
        return;
    }
#endif
    if (!DiscordStatusEnabled()) {
        LOG_INFO(Debug, "Discord status: off (Mods tab)");
        return;
    }
    if (discord_running.exchange(true)) {
        return;
    }
    std::thread(DiscordThread).detach();
    LOG_INFO(Debug, "Discord status: on - shows when the Discord app is running");
}

} // namespace Core::Bbsc

#else // no Discord pipe on this system

namespace Core::Bbsc {
bool DiscordStatusRunning() {
    return false;
}
void StartDiscordStatus(std::string_view) {}
} // namespace Core::Bbsc

#endif

#ifdef BBSC_MOD
namespace BbscMod {
void StartDiscordStatus() {
    Core::Bbsc::StartDiscordStatus("CUSA03173");
}
} // namespace BbscMod
#endif
