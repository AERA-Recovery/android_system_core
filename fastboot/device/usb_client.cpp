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

#include "usb_client.h"

#include "aera_telemetry.h"
#include "constants.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <endian.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>

#include <android-base/logging.h>
#include <android-base/properties.h>

constexpr int kMaxPacketSizeFs = 64;
constexpr int kMaxPacketSizeHs = 512;
constexpr int kMaxPacketsizeSs = 1024;

constexpr size_t kFbFfsNumBufs = 16;
constexpr size_t kFbFfsBufSize = 16384;

constexpr const char* kUsbFfsFastbootEp0 = "/dev/usb-ffs/fastboot/ep0";
constexpr const char* kUsbFfsFastbootOut = "/dev/usb-ffs/fastboot/ep1";
constexpr const char* kUsbFfsFastbootIn = "/dev/usb-ffs/fastboot/ep2";

using namespace std::chrono_literals;

struct ClientUsbTransport::CommandReader {
    std::shared_ptr<usb_handle> handle;
    android::base::unique_fd event_fd;
    std::mutex mutex;
    std::condition_variable condition;
    std::array<char, FB_RESPONSE_SZ> command{};
    ssize_t result = 0;
    bool armed = false;
    bool in_flight = false;
    bool ready = false;
    bool stop = false;
};

struct FuncDesc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio source;
    struct usb_endpoint_descriptor_no_audio sink;
} __attribute__((packed));

struct SsFuncDesc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio source;
    struct usb_ss_ep_comp_descriptor source_comp;
    struct usb_endpoint_descriptor_no_audio sink;
    struct usb_ss_ep_comp_descriptor sink_comp;
} __attribute__((packed));

struct DescV2 {
    struct usb_functionfs_descs_head_v2 header;
    // The rest of the structure depends on the flags in the header.
    __le32 fs_count;
    __le32 hs_count;
    __le32 ss_count;
    struct FuncDesc fs_descs, hs_descs;
    struct SsFuncDesc ss_descs;
} __attribute__((packed));

struct usb_interface_descriptor fastboot_interface = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = 66,
        .bInterfaceProtocol = 3,
        .iInterface = 1, /* first string from the provided table */
};

static struct FuncDesc fs_descriptors = {
        .intf = fastboot_interface,
        .source =
                {
                        .bLength = sizeof(fs_descriptors.source),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketSizeFs,
                },
        .sink =
                {
                        .bLength = sizeof(fs_descriptors.sink),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketSizeFs,
                },
};

static struct FuncDesc hs_descriptors = {
        .intf = fastboot_interface,
        .source =
                {
                        .bLength = sizeof(hs_descriptors.source),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketSizeHs,
                },
        .sink =
                {
                        .bLength = sizeof(hs_descriptors.sink),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketSizeHs,
                },
};

static struct SsFuncDesc ss_descriptors = {
        .intf = fastboot_interface,
        .source =
                {
                        .bLength = sizeof(ss_descriptors.source),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketsizeSs,
                },
        .source_comp =
                {
                        .bLength = sizeof(ss_descriptors.source_comp),
                        .bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
                        .bMaxBurst = 15,
                },
        .sink =
                {
                        .bLength = sizeof(ss_descriptors.sink),
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = kMaxPacketsizeSs,
                },
        .sink_comp =
                {
                        .bLength = sizeof(ss_descriptors.sink_comp),
                        .bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
                        .bMaxBurst = 15,
                },
};

#define STR_INTERFACE_ "fastbootd"

static const struct {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        const char str1[sizeof(STR_INTERFACE_)];
    } __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
        .header =
                {
                        .magic = htole32(FUNCTIONFS_STRINGS_MAGIC),
                        .length = htole32(sizeof(strings)),
                        .str_count = htole32(1),
                        .lang_count = htole32(1),
                },
        .lang0 =
                {
                        htole16(0x0409), /* en-us */
                        STR_INTERFACE_,
                },
};

static struct DescV2 v2_descriptor = {
        .header =
                {
                        .magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
                        .length = htole32(sizeof(v2_descriptor)),
                        .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC |
                                 FUNCTIONFS_HAS_SS_DESC,
                },
        .fs_count = 3,
        .hs_count = 3,
        .ss_count = 5,
        .fs_descs = fs_descriptors,
        .hs_descs = hs_descriptors,
        .ss_descs = ss_descriptors,
};

// Reimplementing since usb_ffs_close() does not close the control FD.
static void CloseFunctionFs(usb_handle* h) {
    h->bulk_in.reset();
    h->bulk_out.reset();
    h->control.reset();
}

static bool InitFunctionFs(usb_handle* h) {
    LOG(INFO) << "initializing functionfs";

    if (h->control < 0) {  // might have already done this before
        LOG(INFO) << "opening control endpoint " << kUsbFfsFastbootEp0;
        h->control.reset(open(kUsbFfsFastbootEp0, O_RDWR));
        if (h->control < 0) {
            PLOG(ERROR) << "cannot open control endpoint " << kUsbFfsFastbootEp0;
            goto err;
        }

        auto ret = write(h->control.get(), &v2_descriptor, sizeof(v2_descriptor));
        if (ret < 0) {
            PLOG(ERROR) << "cannot write descriptors " << kUsbFfsFastbootEp0;
            goto err;
        }

        ret = write(h->control.get(), &strings, sizeof(strings));
        if (ret < 0) {
            PLOG(ERROR) << "cannot write strings " << kUsbFfsFastbootEp0;
            goto err;
        }
        // Signal only when writing the descriptors to ffs
        android::base::SetProperty("sys.usb.ffs.ready", "1");
    }

    h->bulk_out.reset(open(kUsbFfsFastbootOut, O_RDONLY));
    if (h->bulk_out < 0) {
        PLOG(ERROR) << "cannot open bulk-out endpoint " << kUsbFfsFastbootOut;
        goto err;
    }

    h->bulk_in.reset(open(kUsbFfsFastbootIn, O_WRONLY));
    if (h->bulk_in < 0) {
        PLOG(ERROR) << "cannot open bulk-in endpoint " << kUsbFfsFastbootIn;
        goto err;
    }

    h->read_aiob.fd = h->bulk_out.get();
    h->write_aiob.fd = h->bulk_in.get();
    return true;

err:
    CloseFunctionFs(h);
    return false;
}

ClientUsbTransport::ClientUsbTransport()
    : handle_(create_usb_handle(kFbFfsNumBufs, kFbFfsBufSize)) {
    if (!InitFunctionFs(handle_.get())) {
        handle_.reset();
        return;
    }

    command_reader_ = std::make_shared<CommandReader>();
    command_reader_->handle = handle_;
    command_reader_->event_fd.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (command_reader_->event_fd < 0) {
        PLOG(ERROR) << "ClientUsbTransport: cannot create command event";
        command_reader_.reset();
        return;
    }

    std::thread(&ClientUsbTransport::CommandReadLoop, command_reader_).detach();
    ArmCommandRead();
}

ClientUsbTransport::~ClientUsbTransport() {
    if (command_reader_ != nullptr) {
        {
            std::lock_guard<std::mutex> lock(command_reader_->mutex);
            command_reader_->stop = true;
            command_reader_->armed = false;
        }
        command_reader_->condition.notify_all();
    }
    Close();
}

ssize_t ClientUsbTransport::Read(void* data, size_t len) {
    if (handle_ == nullptr) {
        LOG(ERROR) << "ClientUsbTransport: no handle";
        return -1;
    }
    if (len > SSIZE_MAX) {
        LOG(ERROR) << "ClientUsbTransport: maximum length exceeds bounds";
        return -1;
    }
    char* char_data = static_cast<char*>(data);
    size_t bytes_read_total = 0;
    while (bytes_read_total < len) {
        auto bytes_to_read = std::min(len - bytes_read_total, kFbFfsNumBufs * kFbFfsBufSize);
        auto bytes_read_now =
                handle_->read(handle_.get(), char_data, bytes_to_read, true /* allow_partial */);
        if (bytes_read_now < 0) {
            PLOG(ERROR) << "ClientUsbTransport: read failed";
            return bytes_read_total == 0 ? -1 : bytes_read_total;
        }
        bytes_read_total += bytes_read_now;
        AeraTelemetryAdvance(static_cast<uint64_t>(bytes_read_now));
        char_data += bytes_read_now;
        if (static_cast<size_t>(bytes_read_now) < bytes_to_read) {
            break;
        }
    }
    return bytes_read_total;
}

ssize_t ClientUsbTransport::Write(const void* data, size_t len) {
    if (handle_ == nullptr || len > SSIZE_MAX) {
        return -1;
    }
    const char* char_data = reinterpret_cast<const char*>(data);
    size_t bytes_written_total = 0;
    while (bytes_written_total < len) {
        auto bytes_to_write = std::min(len - bytes_written_total, kFbFfsNumBufs * kFbFfsBufSize);
        auto bytes_written_now = handle_->write(handle_.get(), char_data, bytes_to_write);
        if (bytes_written_now < 0) {
            return bytes_written_total == 0 ? -1 : bytes_written_total;
        }
        bytes_written_total += bytes_written_now;
        char_data += bytes_written_now;
        if (static_cast<size_t>(bytes_written_now) < bytes_to_write) {
            break;
        }
    }
    return bytes_written_total;
}

int ClientUsbTransport::Close() {
    if (handle_ == nullptr) {
        return -1;
    }
    CloseFunctionFs(handle_.get());
    return 0;
}

int ClientUsbTransport::Reset() {
    return 0;
}

int ClientUsbTransport::ReadFd() const {
    return command_reader_ == nullptr ? -1 : command_reader_->event_fd.get();
}

ssize_t ClientUsbTransport::TryReadCommand(void* data, size_t len) {
    if (command_reader_ == nullptr || len > SSIZE_MAX) return -1;

    uint64_t signal = 0;
    const ssize_t signal_bytes = TEMP_FAILURE_RETRY(
            read(command_reader_->event_fd.get(), &signal, sizeof(signal)));
    if (signal_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (signal_bytes != sizeof(signal)) return -1;

    std::lock_guard<std::mutex> lock(command_reader_->mutex);
    if (!command_reader_->ready || command_reader_->result <= 0) return 0;
    const size_t bytes = std::min(len, static_cast<size_t>(command_reader_->result));
    memcpy(data, command_reader_->command.data(), bytes);
    command_reader_->ready = false;
    command_reader_->result = 0;
    return static_cast<ssize_t>(bytes);
}

void ClientUsbTransport::ArmCommandRead() {
    if (command_reader_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(command_reader_->mutex);
        if (command_reader_->stop || command_reader_->ready ||
            command_reader_->in_flight) {
            return;
        }
        command_reader_->armed = true;
    }
    command_reader_->condition.notify_one();
}

void ClientUsbTransport::CommandReadLoop(std::shared_ptr<CommandReader> reader) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(reader->mutex);
            reader->condition.wait(lock, [&] { return reader->stop || reader->armed; });
            if (reader->stop) return;
            reader->armed = false;
            reader->in_flight = true;
        }

        std::array<char, FB_RESPONSE_SZ> command{};
        const ssize_t result = reader->handle->read(
                reader->handle.get(), command.data(), command.size(),
                true /* allow_partial */);
        const bool usb_selected =
                android::base::GetProperty("fastbootd.protocol", "usb") != "tcp";
        bool publish = false;
        {
            std::lock_guard<std::mutex> lock(reader->mutex);
            reader->in_flight = false;
            if (reader->stop) return;
            if (result > 0 && usb_selected) {
                std::copy_n(command.begin(), result, reader->command.begin());
                reader->result = result;
                reader->ready = true;
                publish = true;
            } else {
                // A USB command that arrived while Wi-Fi was selected is
                // deliberately discarded. It must never execute later when
                // the user returns to USB.
                reader->armed = true;
            }
        }

        if (publish) {
            uint64_t signal = 1;
            if (TEMP_FAILURE_RETRY(
                        write(reader->event_fd.get(), &signal, sizeof(signal))) !=
                sizeof(signal)) {
                PLOG(ERROR) << "ClientUsbTransport: cannot signal USB command";
            }
        } else {
            if (result <= 0) std::this_thread::sleep_for(20ms);
            reader->condition.notify_one();
        }
    }
}
