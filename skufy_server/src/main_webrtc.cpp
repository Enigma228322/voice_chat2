#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <rtc/rtc.hpp>

namespace {
constexpr std::size_t kSamplesPerFrame = 960;
constexpr std::size_t kFrameBytes = kSamplesPerFrame * sizeof(std::int16_t);

std::string base64_encode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    std::uint32_t value = 0;
    int bits = -6;
    for (unsigned char c : input) {
        value = (value << 8) + c;
        bits += 8;
        while (bits >= 0) {
            output.push_back(kAlphabet[(value >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        output.push_back(kAlphabet[((value << 8) >> (bits + 8)) & 0x3F]);
    }
    while (output.size() % 4 != 0) {
        output.push_back('=');
    }
    return output;
}

std::optional<std::string> base64_decode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table {};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    }

    std::string output;
    output.reserve((input.size() / 4) * 3);
    std::uint32_t value = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (c == '=') {
            break;
        }
        const int idx = table[c];
        if (idx < 0) {
            return std::nullopt;
        }
        value = (value << 6) + static_cast<std::uint32_t>(idx);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

class WebRtcSfuServer {
public:
    explicit WebRtcSfuServer(std::uint16_t signaling_port)
        : signaling_port_(signaling_port) {}

    void run() {
        rtc::InitLogger(rtc::LogLevel::Info);

        rtc::WebSocketServer::Configuration config;
        config.port = signaling_port_;
        websocket_server_ = std::make_unique<rtc::WebSocketServer>(config);
        websocket_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) { on_client(std::move(ws)); });

        std::cout << "[server] WebRTC signaling ws://0.0.0.0:" << signaling_port_ << "\n";

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

private:
    struct Session {
        std::uint32_t user_id = 0;
        std::shared_ptr<rtc::WebSocket> ws;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> audio_dc;
    };

    void on_client(const std::shared_ptr<rtc::WebSocket>& ws) {
        {
            std::scoped_lock lock(mutex_);
            pending_ws_[ws.get()] = ws;
        }
        std::cout << "[server] websocket client accepted\n";

        ws->onMessage([this, weak_ws = std::weak_ptr<rtc::WebSocket>(ws)](const rtc::message_variant& message) {
            const auto strong_ws = weak_ws.lock();
            if (!strong_ws || !std::holds_alternative<std::string>(message)) {
                return;
            }
            on_signaling_message(strong_ws, std::get<std::string>(message));
        });
        ws->onClosed([this, weak_ws = std::weak_ptr<rtc::WebSocket>(ws)]() {
            const auto strong_ws = weak_ws.lock();
            if (strong_ws) {
                on_disconnect(strong_ws.get());
            }
        });
    }

    void on_signaling_message(const std::shared_ptr<rtc::WebSocket>& ws, const std::string& text) {
        std::cout << "[server] signaling message: " << text << "\n";
        if (starts_with(text, "JOIN ")) {
            create_or_replace_session(ws, static_cast<std::uint32_t>(std::stoul(text.substr(5))));
            return;
        }
        if (starts_with(text, "ANSWER ")) {
            apply_answer(ws.get(), text.substr(7));
            return;
        }
        if (starts_with(text, "CAND ")) {
            apply_candidate(ws.get(), text.substr(5));
        }
    }

    void create_or_replace_session(const std::shared_ptr<rtc::WebSocket>& ws, std::uint32_t user_id) {
        std::scoped_lock lock(mutex_);

        pending_ws_.erase(ws.get());
        ws_to_user_[ws.get()] = user_id;
        sessions_.erase(user_id);

        Session session;
        session.user_id = user_id;
        session.ws = ws;
        session.pc = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
        session.pc->onLocalDescription([this, user_id](rtc::Description description) {
            send_offer(user_id, description);
        });
        session.pc->onLocalCandidate([this, user_id](rtc::Candidate candidate) {
            send_candidate(user_id, candidate);
        });
        session.audio_dc = session.pc->createDataChannel("audio");
        session.audio_dc->onMessage([this, user_id](const rtc::message_variant& message) {
            on_audio_from_user(user_id, message);
        });
        sessions_[user_id] = session;

        sessions_[user_id].pc->setLocalDescription();
        ws->send(std::string("HELLO ") + std::to_string(user_id));
        std::cout << "[server] user " << user_id << " joined\n";
    }

    void send_offer(std::uint32_t user_id, const rtc::Description& description) {
        std::scoped_lock lock(mutex_);
        auto it = sessions_.find(user_id);
        if (it != sessions_.end() && it->second.ws) {
            it->second.ws->send("OFFER " + base64_encode(std::string(description)));
        }
    }

    void send_candidate(std::uint32_t user_id, const rtc::Candidate& candidate) {
        std::scoped_lock lock(mutex_);
        auto it = sessions_.find(user_id);
        if (it != sessions_.end() && it->second.ws) {
            it->second.ws->send("CAND " + base64_encode(candidate.mid()) + " " +
                                base64_encode(candidate.candidate()));
        }
    }

    void apply_answer(rtc::WebSocket* ws_ptr, const std::string& encoded_sdp) {
        std::scoped_lock lock(mutex_);
        const auto user_it = ws_to_user_.find(ws_ptr);
        if (user_it == ws_to_user_.end()) {
            return;
        }
        auto session_it = sessions_.find(user_it->second);
        if (session_it == sessions_.end() || !session_it->second.pc) {
            return;
        }
        const auto decoded = base64_decode(encoded_sdp);
        if (decoded) {
            session_it->second.pc->setRemoteDescription(rtc::Description(*decoded, "answer"));
        }
    }

    void apply_candidate(rtc::WebSocket* ws_ptr, const std::string& payload) {
        const auto delimiter = payload.find(' ');
        if (delimiter == std::string::npos) {
            return;
        }
        const auto mid = base64_decode(payload.substr(0, delimiter));
        const auto candidate = base64_decode(payload.substr(delimiter + 1));
        if (!mid || !candidate) {
            return;
        }

        std::scoped_lock lock(mutex_);
        const auto user_it = ws_to_user_.find(ws_ptr);
        if (user_it == ws_to_user_.end()) {
            return;
        }
        auto session_it = sessions_.find(user_it->second);
        if (session_it == sessions_.end() || !session_it->second.pc) {
            return;
        }
        session_it->second.pc->addRemoteCandidate(rtc::Candidate(*candidate, *mid));
    }

    void on_audio_from_user(std::uint32_t source_user_id, const rtc::message_variant& message) {
        if (!std::holds_alternative<rtc::binary>(message)) {
            return;
        }
        const auto& frame = std::get<rtc::binary>(message);
        if (frame.size() != kFrameBytes) {
            return;
        }

        rtc::binary packet(4 + frame.size());
        packet[0] = static_cast<std::byte>(source_user_id & 0xFFu);
        packet[1] = static_cast<std::byte>((source_user_id >> 8u) & 0xFFu);
        packet[2] = static_cast<std::byte>((source_user_id >> 16u) & 0xFFu);
        packet[3] = static_cast<std::byte>((source_user_id >> 24u) & 0xFFu);
        std::copy(frame.begin(), frame.end(), packet.begin() + 4);

        std::vector<std::shared_ptr<rtc::DataChannel>> recipients;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& [user_id, session] : sessions_) {
                if (user_id == source_user_id || !session.audio_dc || !session.audio_dc->isOpen()) {
                    continue;
                }
                recipients.push_back(session.audio_dc);
            }
        }
        for (const auto& dc : recipients) {
            dc->send(packet);
        }
    }

    void on_disconnect(rtc::WebSocket* ws_ptr) {
        std::scoped_lock lock(mutex_);
        pending_ws_.erase(ws_ptr);
        const auto it = ws_to_user_.find(ws_ptr);
        if (it == ws_to_user_.end()) {
            std::cout << "[server] websocket disconnected before JOIN\n";
            return;
        }
        const auto user_id = it->second;
        ws_to_user_.erase(it);
        sessions_.erase(user_id);
        std::cout << "[server] user " << user_id << " disconnected\n";
    }

private:
    std::uint16_t signaling_port_ = 8000;
    std::unique_ptr<rtc::WebSocketServer> websocket_server_;
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Session> sessions_;
    std::unordered_map<rtc::WebSocket*, std::uint32_t> ws_to_user_;
    std::unordered_map<rtc::WebSocket*, std::shared_ptr<rtc::WebSocket>> pending_ws_;
};

std::uint16_t parse_port(int argc, char** argv) {
    if (argc < 2) {
        return 8000;
    }
    const int value = std::stoi(argv[1]);
    if (value < 1 || value > 65535) {
        throw std::runtime_error("port must be in [1, 65535]");
    }
    return static_cast<std::uint16_t>(value);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        WebRtcSfuServer server(parse_port(argc, argv));
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << "[server] fatal error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
