#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
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

std::string read_file_or_empty(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<std::string> json_string(const std::string& json, const std::string& key) {
    const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
        return std::nullopt;
    }
    return m[1].str();
}

std::optional<int> json_int(const std::string& json, const std::string& key) {
    const std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
        return std::nullopt;
    }
    return std::stoi(m[1].str());
}

std::vector<std::string> json_string_array(const std::string& json, const std::string& key) {
    const std::regex array_re("\"" + key + "\"\\s*:\\s*\\[([\\s\\S]*?)\\]");
    std::smatch m;
    if (!std::regex_search(json, m, array_re)) {
        return {};
    }
    const std::string content = m[1].str();
    const std::regex item_re("\"([^\"]*)\"");
    std::vector<std::string> values;
    for (auto it = std::sregex_iterator(content.begin(), content.end(), item_re);
         it != std::sregex_iterator();
         ++it) {
        values.push_back((*it)[1].str());
    }
    return values;
}

std::optional<std::string> env_string(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::uint16_t parse_port_value(const std::string& name, const std::string& value) {
    std::size_t parsed = 0;
    const unsigned long port = std::stoul(value, &parsed);
    if (parsed != value.size() || port < 1 || port > 65535) {
        throw std::runtime_error(name + " must be in [1, 65535]");
    }
    return static_cast<std::uint16_t>(port);
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> items;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto item = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) {
            items.push_back(item);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return items;
}

struct ServerOptions {
    std::uint16_t signaling_port = 8000;
    std::optional<std::string> bind_address;
    rtc::Configuration peer_config {};
};

ServerOptions load_server_options_from_env() {
    ServerOptions options;
    if (const auto port = env_string("SKUFY_SIGNALING_PORT")) {
        options.signaling_port = parse_port_value("SKUFY_SIGNALING_PORT", *port);
    }
    if (const auto bind = env_string("SKUFY_BIND_ADDRESS")) {
        options.bind_address = *bind;
    }
    if (const auto ice_bind = env_string("SKUFY_ICE_BIND_ADDRESS")) {
        options.peer_config.bindAddress = *ice_bind;
    } else if (options.bind_address) {
        options.peer_config.bindAddress = *options.bind_address;
    }
    if (const auto begin = env_string("SKUFY_ICE_PORT_RANGE_BEGIN")) {
        options.peer_config.portRangeBegin = parse_port_value("SKUFY_ICE_PORT_RANGE_BEGIN", *begin);
    }
    if (const auto end = env_string("SKUFY_ICE_PORT_RANGE_END")) {
        options.peer_config.portRangeEnd = parse_port_value("SKUFY_ICE_PORT_RANGE_END", *end);
    }
    if (options.peer_config.portRangeBegin > options.peer_config.portRangeEnd) {
        throw std::runtime_error("SKUFY_ICE_PORT_RANGE_BEGIN must be <= SKUFY_ICE_PORT_RANGE_END");
    }
    if (const auto servers = env_string("SKUFY_ICE_SERVERS")) {
        for (const auto& url : split_csv(*servers)) {
            options.peer_config.iceServers.emplace_back(url);
        }
    }
    return options;
}

ServerOptions load_server_options_from_file(const std::string& config_path) {
    ServerOptions options;
    const std::string json = read_file_or_empty(config_path);
    if (json.empty()) {
        return options;
    }

    if (const auto port = json_int(json, "signaling_port")) {
        if (*port < 1 || *port > 65535) {
            throw std::runtime_error("config signaling_port must be in [1, 65535]");
        }
        options.signaling_port = static_cast<std::uint16_t>(*port);
    }
    if (const auto bind = json_string(json, "bind_address")) {
        options.bind_address = *bind;
    }
    if (const auto ice_bind = json_string(json, "ice_bind_address")) {
        options.peer_config.bindAddress = *ice_bind;
    } else if (options.bind_address) {
        options.peer_config.bindAddress = *options.bind_address;
    }
    if (const auto begin = json_int(json, "ice_port_range_begin")) {
        options.peer_config.portRangeBegin = static_cast<std::uint16_t>(*begin);
    }
    if (const auto end = json_int(json, "ice_port_range_end")) {
        options.peer_config.portRangeEnd = static_cast<std::uint16_t>(*end);
    }
    if (options.peer_config.portRangeBegin > options.peer_config.portRangeEnd) {
        throw std::runtime_error("config ice_port_range_begin must be <= ice_port_range_end");
    }
    for (const auto& url : json_string_array(json, "ice_servers")) {
        options.peer_config.iceServers.emplace_back(url);
    }
    return options;
}

class WebRtcSfuServer {
public:
    explicit WebRtcSfuServer(ServerOptions options)
        : signaling_port_(options.signaling_port),
          bind_address_(std::move(options.bind_address)),
          peer_config_(std::move(options.peer_config)) {}

    void run() {
        rtc::InitLogger(rtc::LogLevel::Info);

        rtc::WebSocketServer::Configuration config;
        config.port = signaling_port_;
        if (bind_address_) {
            config.bindAddress = *bind_address_;
        }
        websocket_server_ = std::make_unique<rtc::WebSocketServer>(config);
        websocket_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) { on_client(std::move(ws)); });

        std::cout << "[server] WebRTC signaling ws://0.0.0.0:" << signaling_port_ << "\n";

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            log_traffic_once();
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
        session.pc = std::make_shared<rtc::PeerConnection>(peer_config_);
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
        rx_frames_.fetch_add(1, std::memory_order_relaxed);
        rx_bytes_.fetch_add(static_cast<std::uint64_t>(frame.size()), std::memory_order_relaxed);

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
            tx_frames_.fetch_add(1, std::memory_order_relaxed);
            tx_bytes_.fetch_add(static_cast<std::uint64_t>(packet.size()), std::memory_order_relaxed);
        }
    }

    void log_traffic_once() {
        const auto rx_frames = rx_frames_.exchange(0, std::memory_order_relaxed);
        const auto tx_frames = tx_frames_.exchange(0, std::memory_order_relaxed);
        const auto rx_bytes = rx_bytes_.exchange(0, std::memory_order_relaxed);
        const auto tx_bytes = tx_bytes_.exchange(0, std::memory_order_relaxed);
        total_rx_frames_.fetch_add(rx_frames, std::memory_order_relaxed);
        total_tx_frames_.fetch_add(tx_frames, std::memory_order_relaxed);
        total_rx_bytes_.fetch_add(rx_bytes, std::memory_order_relaxed);
        total_tx_bytes_.fetch_add(tx_bytes, std::memory_order_relaxed);
        std::cout << "[server] traffic rx_frames/s=" << rx_frames
                  << " rx_bytes/s=" << rx_bytes
                  << " tx_frames/s=" << tx_frames
                  << " tx_bytes/s=" << tx_bytes
                  << " total_rx_bytes=" << total_rx_bytes_.load(std::memory_order_relaxed)
                  << " total_tx_bytes=" << total_tx_bytes_.load(std::memory_order_relaxed)
                  << "\n";
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
    std::optional<std::string> bind_address_;
    rtc::Configuration peer_config_ {};
    std::unique_ptr<rtc::WebSocketServer> websocket_server_;
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Session> sessions_;
    std::unordered_map<rtc::WebSocket*, std::uint32_t> ws_to_user_;
    std::unordered_map<rtc::WebSocket*, std::shared_ptr<rtc::WebSocket>> pending_ws_;
    std::atomic<std::uint64_t> rx_frames_ {0};
    std::atomic<std::uint64_t> tx_frames_ {0};
    std::atomic<std::uint64_t> rx_bytes_ {0};
    std::atomic<std::uint64_t> tx_bytes_ {0};
    std::atomic<std::uint64_t> total_rx_frames_ {0};
    std::atomic<std::uint64_t> total_tx_frames_ {0};
    std::atomic<std::uint64_t> total_rx_bytes_ {0};
    std::atomic<std::uint64_t> total_tx_bytes_ {0};
};

ServerOptions parse_server_options(int argc, char** argv) {
    std::optional<std::string> config_path;
    std::optional<std::uint16_t> port_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--config requires a file path");
            }
            config_path = argv[++i];
            continue;
        }
        port_override = parse_port_value("port", arg);
    }

    auto options = config_path ? load_server_options_from_file(*config_path) : load_server_options_from_env();
    if (port_override) {
        options.signaling_port = *port_override;
    }
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        WebRtcSfuServer server(parse_server_options(argc, argv));
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << "[server] fatal error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
