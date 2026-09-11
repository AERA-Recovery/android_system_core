/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aera_telemetry.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>

namespace {
std::mutex g_lock;
android::base::unique_fd g_socket;
std::string g_phase = "idle";
std::string g_target;
uint64_t g_current = 0;
uint64_t g_total = 0;
uint64_t g_last_publish_ms = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

std::string Safe(const std::string& value) {
    std::string result = value.substr(0, 96);
    std::replace_if(result.begin(), result.end(),
                    [](char c) { return c == '|' || c == '\n' || c == '\r'; }, '_');
    return result;
}

bool EnsureConnected() {
    if (g_socket.ok()) return true;
    g_socket.reset(socket_local_client("recovery", ANDROID_SOCKET_NAMESPACE_RESERVED,
                                       SOCK_STREAM));
    return g_socket.ok();
}

void PublishLocked(int result, bool force) {
    const uint64_t now = NowMs();
    if (!force && now - g_last_publish_ms < 50) return;
    g_last_publish_ms = now;
    if (!EnsureConnected()) return;

    const std::string line = android::base::StringPrintf(
            "AERA1|%s|%llu|%llu|%d|%s\n", g_phase.c_str(),
            static_cast<unsigned long long>(g_current),
            static_cast<unsigned long long>(g_total), result,
            Safe(g_target).c_str());
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = send(g_socket.get(), line.data() + offset,
                                     line.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            g_socket.reset();
            return;
        }
        offset += static_cast<size_t>(written);
    }
}
}  // namespace

void AeraTelemetryBegin(const char* phase, const std::string& target,
                        uint64_t total) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_phase = phase == nullptr ? "working" : phase;
    g_target = target;
    g_current = 0;
    g_total = total;
    PublishLocked(-1, true);
}

void AeraTelemetrySetTotal(uint64_t total) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_total = total;
    g_current = std::min(g_current, g_total);
    PublishLocked(-1, true);
}

void AeraTelemetryAdvance(uint64_t bytes) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_phase != "receiving" && g_phase != "flashing") return;
    g_current = g_total == 0 ? g_current + bytes
                             : std::min(g_total, g_current + bytes);
    PublishLocked(-1, g_total != 0 && g_current >= g_total);
}

void AeraTelemetryComplete(const char* phase, const std::string& target,
                           bool success) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_phase = phase == nullptr ? "complete" : phase;
    g_target = target;
    if (success && g_total != 0) g_current = g_total;
    PublishLocked(success ? 0 : 1, true);
}

