/*
 * Copyright (C) 2026 The AERA Recovery Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */
#pragma once

#include <cstdint>
#include <memory>

#include "tcp_client.h"
#include "transport.h"
#include "usb_client.h"

// Presents USB FunctionFS and AERA's loopback-only TCP endpoint behind one
// explicit selector. Only the endpoint named by fastbootd.protocol can begin
// a command. That endpoint owns the complete command, including its DATA phase
// and final response, so transport changes can never interleave a write.
class ClientDualTransport : public Transport {
  public:
    ClientDualTransport();
    ~ClientDualTransport() override = default;

    ssize_t Read(void* data, size_t len) override;
    ssize_t Write(const void* data, size_t len) override;
    int Close() override;
    int Reset() override;

  private:
    enum class Active { kNone, kUsb, kTcp };

    ssize_t ReadCommand(void* data, size_t len);
    Transport* ActiveTransport();
    void ReleaseCommand();
    void AccountData(size_t bytes);

    std::unique_ptr<ClientUsbTransport> usb_;
    std::unique_ptr<ClientTcpTransport> tcp_;
    Active active_ = Active::kNone;
    uint64_t data_bytes_left_ = 0;

    DISALLOW_COPY_AND_ASSIGN(ClientDualTransport);
};
