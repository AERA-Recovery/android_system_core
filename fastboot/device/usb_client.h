/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <memory>

#include "usb.h"

#include "transport.h"

class ClientUsbTransport : public Transport {
  public:
    ClientUsbTransport();
    ~ClientUsbTransport() override;

    ssize_t Read(void* data, size_t len) override;
    ssize_t Write(const void* data, size_t len) override;
    int Close() override;
    int Reset() override;

    int ReadFd() const;
    // FunctionFS may report POLLIN when the USB function is enabled even
    // though the host has not sent a Fastboot command. Probe the command
    // endpoint without entering the blocking AIO path used for payloads.
    // Returns 0 when no command bytes are available yet.
    ssize_t TryReadCommand(void* data, size_t len);
    void ArmCommandRead();

  private:
    struct CommandReader;
    static void CommandReadLoop(std::shared_ptr<CommandReader> reader);

    std::shared_ptr<usb_handle> handle_;
    std::shared_ptr<CommandReader> command_reader_;

    DISALLOW_COPY_AND_ASSIGN(ClientUsbTransport);
};
