/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>

// Lightweight recovery-socket telemetry used only by the bundled AERA UI.
// Standard fastboot hosts remain completely protocol-compatible.
void AeraTelemetryBegin(const char* phase, const std::string& target,
                        uint64_t total);
void AeraTelemetrySetTotal(uint64_t total);
void AeraTelemetryAdvance(uint64_t bytes);
void AeraTelemetryComplete(const char* phase, const std::string& target,
                           bool success);

