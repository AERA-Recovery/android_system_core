/*
 * Copyright (C) 2026 The AERA Recovery Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "dual_client.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <string>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>

#include "constants.h"

ClientDualTransport::ClientDualTransport()
    : usb_(std::make_unique<ClientUsbTransport>()),
      tcp_(std::make_unique<ClientTcpTransport>(true)) {}

Transport* ClientDualTransport::ActiveTransport() {
    if (active_ == Active::kUsb) return usb_.get();
    if (active_ == Active::kTcp) return tcp_.get();
    return nullptr;
}

ssize_t ClientDualTransport::ReadCommand(void* data, size_t len) {
    while (active_ == Active::kNone) {
        const bool wifi_selected =
                android::base::GetProperty("fastbootd.protocol", "usb") == "tcp";
        pollfd descriptors[2] = {};
        // Keep both endpoints initialized, but poll only the one explicitly
        // selected by the AERA UI. The short timeout lets a pill tap change
        // transport immediately without restarting this daemon.
        descriptors[0].fd = wifi_selected ? -1 : usb_->ReadFd();
        descriptors[0].events = POLLIN;
        descriptors[1].fd = wifi_selected ? tcp_->ReadFd() : -1;
        descriptors[1].events = POLLIN;
        if (descriptors[0].fd < 0 && descriptors[1].fd < 0) {
            LOG(ERROR) << "No Fastboot transport is available";
            return -1;
        }

        const int result = TEMP_FAILURE_RETRY(poll(descriptors, 2, 100));
        if (result < 0) {
            PLOG(ERROR) << "Fastboot transport poll failed";
            return -1;
        }
        if (result == 0) continue;

        if (wifi_selected && (descriptors[1].revents & POLLIN)) {
            active_ = Active::kTcp;
            android::base::SetProperty("fastbootd.active_transport", "wifi");
            const ssize_t bytes = tcp_->Read(data, len);
            if (bytes >= 0) return bytes;
            tcp_->Reset();
            active_ = Active::kNone;
            continue;
        }

        if (!wifi_selected && (descriptors[0].revents & POLLIN)) {
            const ssize_t bytes = usb_->TryReadCommand(data, len);
            if (bytes > 0) {
                active_ = Active::kUsb;
                android::base::SetProperty("fastbootd.active_transport", "usb");
                return bytes;
            }
            if (bytes < 0) return -1;
        }

        // FunctionFS may remain spuriously readable until the first command.
        // Do not spin hard enough to starve adbd or the recovery UI.
        usleep(20000);
    }
    return -1;
}

void ClientDualTransport::AccountData(size_t bytes) {
    if (data_bytes_left_ == 0) return;
    data_bytes_left_ = bytes >= data_bytes_left_ ? 0 : data_bytes_left_ - bytes;
}

ssize_t ClientDualTransport::Read(void* data, size_t len) {
    for (;;) {
        if (active_ == Active::kNone) return ReadCommand(data, len);
        const bool reading_data = data_bytes_left_ != 0;
        const Active selected = active_;
        const ssize_t result = ActiveTransport()->Read(data, len);
        if (result >= 0) {
            if (reading_data) AccountData(static_cast<size_t>(result));
            return result;
        }

        // A lost DATA stream is fatal to the command. Before a command starts,
        // a stale TCP connection may disappear normally; return to polling so
        // USB remains immediately usable.
        if (reading_data || selected == Active::kUsb) return -1;
        tcp_->Reset();
        active_ = Active::kNone;
    }
}

ssize_t ClientDualTransport::Write(const void* data, size_t len) {
    Transport* transport = ActiveTransport();
    if (transport == nullptr) return -1;

    const bool writing_data = data_bytes_left_ != 0;
    const ssize_t result = transport->Write(data, len);
    if (result < 0) return result;
    if (writing_data) {
        AccountData(static_cast<size_t>(result));
        return result;
    }

    const char* response = static_cast<const char*>(data);
    if (len >= 12 && memcmp(response, RESPONSE_DATA, 4) == 0) {
        uint64_t size = 0;
        std::string encoded(response + 4, 8);
        if (!android::base::ParseUint("0x" + encoded, &size)) {
            LOG(ERROR) << "Invalid Fastboot DATA size: " << encoded;
            return -1;
        }
        data_bytes_left_ = size;
    } else if (len >= 4 &&
               (memcmp(response, RESPONSE_OKAY, 4) == 0 ||
                memcmp(response, RESPONSE_FAIL, 4) == 0)) {
        ReleaseCommand();
    }
    return result;
}

void ClientDualTransport::ReleaseCommand() {
    data_bytes_left_ = 0;
    active_ = Active::kNone;
    usb_->ArmCommandRead();
    android::base::SetProperty("fastbootd.active_transport", "idle");
}

int ClientDualTransport::Close() {
    const int usb_result = usb_->Close();
    const int tcp_result = tcp_->Close();
    active_ = Active::kNone;
    data_bytes_left_ = 0;
    return usb_result == 0 || tcp_result == 0 ? 0 : -1;
}

int ClientDualTransport::Reset() {
    const int usb_result = usb_->Reset();
    const int tcp_result = tcp_->Reset();
    active_ = Active::kNone;
    data_bytes_left_ = 0;
    return usb_result == 0 && tcp_result == 0 ? 0 : -1;
}
