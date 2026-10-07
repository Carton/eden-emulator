// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>

#include "input_common/drivers/udp_client.h"
#include "input_common/helpers/udp_protocol.h"

class FakeCemuhookServer {
public:
    FakeCemuhookServer()
        : socket(io_context, {boost::asio::ip::address_v4::loopback(), 0}) {}

    ~FakeCemuhookServer() {
        io_context.stop();
        if (handler.joinable()) {
            handler.join();
        }
    }

    u16 GetPort() const {
        return socket.local_endpoint().port();
    }

    std::string GetHost() const {
        return socket.local_endpoint().address().to_string();
    }

    void Run(std::vector<InputCommon::CemuhookUDP::Response::TouchPad> touch_movement_path) {
        REQUIRE(!touch_movement_path.empty());
        touch_path = std::move(touch_movement_path);
        StartReceive();
        handler = std::jthread([this] { io_context.run(); });
    }

private:
    void StartReceive() {
        socket.async_receive_from(
            boost::asio::buffer(receive_buffer), sender_endpoint,
            [this](const boost::system::error_code& error, size_t received_size) {
                if (error) {
                    return;
                }
                if (received_size >= sizeof(InputCommon::CemuhookUDP::Header)) {
                    InputCommon::CemuhookUDP::Header header{};
                    std::memcpy(&header, receive_buffer.data(), sizeof(header));
                    if (header.type == InputCommon::CemuhookUDP::Type::PadData) {
                        InputCommon::CemuhookUDP::Response::PadData pad_data{};
                        pad_data.touch[0] = touch_path[touch_index];
                        const auto message = InputCommon::CemuhookUDP::CreateMessage(
                            InputCommon::CemuhookUDP::SERVER_MAGIC, pad_data, 0);
                        boost::system::error_code send_error;
                        socket.send_to(boost::asio::buffer(&message, sizeof(message)),
                                       sender_endpoint, 0, send_error);
                        if (!send_error && touch_index + 1 < touch_path.size()) {
                            ++touch_index;
                        }
                    }
                }
                StartReceive();
            });
    }

    boost::asio::io_context io_context;
    boost::asio::ip::udp::socket socket;
    boost::asio::ip::udp::endpoint sender_endpoint;
    std::array<u8, InputCommon::CemuhookUDP::MAX_PACKET_SIZE> receive_buffer{};
    std::vector<InputCommon::CemuhookUDP::Response::TouchPad> touch_path;
    size_t touch_index = 0;
    std::jthread handler;
};

TEST_CASE("CalibrationConfigurationJob completed", "[input_common]") {
    Common::Event complete_event;
    FakeCemuhookServer server;
    server.Run({{.is_active = 1, .x = 0, .y = 0},
                {.is_active = 1, .x = 200, .y = 200}});

    using Job = InputCommon::CemuhookUDP::CalibrationConfigurationJob;
    Job::Status status{};
    u16 min_x{};
    u16 min_y{};
    u16 max_x{};
    u16 max_y{};
    {
        Job job(
            server.GetHost(), server.GetPort(),
            [&](Job::Status next_status) {
                status = next_status;
                if (status == Job::Status::Completed) {
                    complete_event.Set();
                }
            },
            [&](u16 min_x_, u16 min_y_, u16 max_x_, u16 max_y_) {
                min_x = min_x_;
                min_y = min_y_;
                max_x = max_x_;
                max_y = max_y_;
            });
        REQUIRE(complete_event.WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    }
    // Destroying the job joins its workers before callback state is read.
    REQUIRE(status == Job::Status::Completed);
    REQUIRE(min_x == 0);
    REQUIRE(min_y == 0);
    REQUIRE(max_x == 200);
    REQUIRE(max_y == 200);
}

TEST_CASE("CalibrationConfigurationJob cancels before a response", "[input_common]") {
    FakeCemuhookServer server;
    using Job = InputCommon::CemuhookUDP::CalibrationConfigurationJob;
    std::atomic<size_t> callback_count{};
    {
        Job job(server.GetHost(), server.GetPort(),
                [&](Job::Status) { ++callback_count; },
                [&](u16, u16, u16, u16) { ++callback_count; });
        job.Stop();
        job.Stop();
    }
    REQUIRE(callback_count.load() == 0);
}

TEST_CASE("CalibrationConfigurationJob joins an active callback", "[input_common]") {
    FakeCemuhookServer server;
    server.Run({{.is_active = 1, .x = 0, .y = 0}});
    using Job = InputCommon::CemuhookUDP::CalibrationConfigurationJob;
    Common::Event callback_entered;
    Common::Event release_callback;
    std::atomic<bool> callback_finished{};
    auto job = std::make_unique<Job>(
        server.GetHost(), server.GetPort(),
        [&](Job::Status status) {
            if (status == Job::Status::Stage1Completed) {
                callback_entered.Set();
                release_callback.Wait();
                callback_finished = true;
            }
        },
        [](u16, u16, u16, u16) {});

    if (!callback_entered.WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
        release_callback.Set();
        FAIL("Calibration callback did not start");
    }
    auto destruction = std::async(std::launch::async, [job = std::move(job)]() mutable {
        job.reset();
    });
    const bool destroyed_early =
        destruction.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
    // Release before asserting, so a failed assertion cannot strand a worker.
    release_callback.Set();
    destruction.get();
    REQUIRE_FALSE(destroyed_early);
    REQUIRE(callback_finished.load());
}
