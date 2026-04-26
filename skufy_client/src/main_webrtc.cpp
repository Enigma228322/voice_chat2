#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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

#include <portaudio.h>
#include <rtc/rtc.hpp>

namespace {
constexpr std::size_t kSamplesPerFrame = 960;
constexpr std::size_t kFrameBytes = kSamplesPerFrame * sizeof(std::int16_t);
constexpr double kSampleRate = 48000.0;

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

std::optional<bool> json_bool(const std::string& json, const std::string& key) {
    const std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
        return std::nullopt;
    }
    return m[1].str() == "true";
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

class VoiceClient {
public:
    VoiceClient(std::string host,
                std::uint16_t signaling_port,
                std::uint32_t user_id,
                bool mic_enabled,
                bool speaker_enabled,
                std::optional<int> mic_device,
                std::optional<int> speaker_device,
                rtc::Configuration peer_config)
        : host_(std::move(host)),
          signaling_port_(signaling_port),
          user_id_(user_id),
          mic_enabled_(mic_enabled),
          speaker_enabled_(speaker_enabled),
          mic_device_(mic_device),
          speaker_device_(speaker_device),
          peer_config_(std::move(peer_config)) {}

    ~VoiceClient() {
        shutdown();
    }

    void run() {
        rtc::InitLogger(rtc::LogLevel::Warning);
        init_audio();
        setup_peer_connection();
        setup_signaling_socket();
        running_.store(true);

        sender_thread_ = std::thread([this]() { sender_loop(); });
        mixer_thread_ = std::thread([this]() { mixer_loop(); });
        stats_thread_ = std::thread([this]() { stats_loop(); });

        std::cout << "[client " << user_id_ << "] signaling ws://" << host_ << ":" << signaling_port_
                  << "\n";
        if (!speaker_enabled_) {
            std::cout << "[client " << user_id_ << "] speaker is OFF\n";
        }

        sender_thread_.join();
        mixer_thread_.join();
        stats_thread_.join();
    }

private:
    struct RemoteTrack {
        std::array<std::int16_t, kSamplesPerFrame> frame {};
        std::chrono::steady_clock::time_point last_frame_at {};
    };

    static int audio_callback(const void* input,
                              void* output,
                              unsigned long frame_count,
                              const PaStreamCallbackTimeInfo*,
                              PaStreamCallbackFlags,
                              void* user_data) {
        auto* self = static_cast<VoiceClient*>(user_data);
        return self->on_audio_callback(input, output, frame_count);
    }

    int on_audio_callback(const void* input, void* output, unsigned long frame_count) {
        if (frame_count != kSamplesPerFrame) {
            return paContinue;
        }
        const auto* in_samples = static_cast<const std::int16_t*>(input);
        auto* out_samples = static_cast<std::int16_t*>(output);

        if (in_samples) {
            std::scoped_lock lock(captured_frame_mutex_);
            std::memcpy(captured_frame_.data(), in_samples, kFrameBytes);
            captured_seq_.fetch_add(1);
        }
        if (out_samples) {
            std::scoped_lock lock(playback_frame_mutex_);
            std::memcpy(out_samples, playback_frame_.data(), kFrameBytes);
        }
        return paContinue;
    }

    void init_audio() {
        if (!mic_enabled_ && !speaker_enabled_) {
            return;
        }

        const PaError init_err = Pa_Initialize();
        if (init_err != paNoError) {
            throw std::runtime_error(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(init_err));
        }
        audio_initialized_ = true;

        const int device_count = Pa_GetDeviceCount();
        if (device_count < 0) {
            throw std::runtime_error(std::string("Pa_GetDeviceCount failed: ") +
                                     Pa_GetErrorText(static_cast<PaError>(device_count)));
        }

        std::optional<PaStreamParameters> input_params_storage;
        std::optional<PaStreamParameters> output_params_storage;

        if (mic_enabled_) {
            const PaDeviceIndex input_device =
                mic_device_ ? static_cast<PaDeviceIndex>(*mic_device_) : Pa_GetDefaultInputDevice();
            if (input_device == paNoDevice || input_device < 0 || input_device >= device_count) {
                throw std::runtime_error("invalid mic device");
            }
            const auto* input_info = Pa_GetDeviceInfo(input_device);
            if (!input_info || input_info->maxInputChannels < 1) {
                throw std::runtime_error("selected mic device has no input channels");
            }
            std::cout << "[client " << user_id_ << "] mic device #" << input_device
                      << " " << input_info->name << "\n";
            PaStreamParameters params {};
            params.device = input_device;
            params.channelCount = 1;
            params.sampleFormat = paInt16;
            params.suggestedLatency = input_info->defaultLowInputLatency;
            params.hostApiSpecificStreamInfo = nullptr;
            input_params_storage = params;
        }

        if (speaker_enabled_) {
            const PaDeviceIndex output_device =
                speaker_device_ ? static_cast<PaDeviceIndex>(*speaker_device_) : Pa_GetDefaultOutputDevice();
            if (output_device == paNoDevice || output_device < 0 || output_device >= device_count) {
                throw std::runtime_error("invalid speaker device");
            }
            const auto* output_info = Pa_GetDeviceInfo(output_device);
            if (!output_info || output_info->maxOutputChannels < 1) {
                throw std::runtime_error("selected speaker device has no output channels");
            }
            std::cout << "[client " << user_id_ << "] speaker device #" << output_device
                      << " " << output_info->name << "\n";
            PaStreamParameters params {};
            params.device = output_device;
            params.channelCount = 1;
            params.sampleFormat = paInt16;
            params.suggestedLatency = output_info->defaultLowOutputLatency;
            params.hostApiSpecificStreamInfo = nullptr;
            output_params_storage = params;
        }

        const PaError open_err = Pa_OpenStream(&stream_,
                                               input_params_storage ? &*input_params_storage : nullptr,
                                               output_params_storage ? &*output_params_storage : nullptr,
                                               kSampleRate,
                                               kSamplesPerFrame,
                                               paClipOff,
                                               &VoiceClient::audio_callback,
                                               this);
        if (open_err != paNoError) {
            throw std::runtime_error(std::string("Pa_OpenStream failed: ") + Pa_GetErrorText(open_err));
        }

        const PaError start_err = Pa_StartStream(stream_);
        if (start_err != paNoError) {
            throw std::runtime_error(std::string("Pa_StartStream failed: ") + Pa_GetErrorText(start_err));
        }
    }

    void setup_peer_connection() {
        peer_connection_ = std::make_shared<rtc::PeerConnection>(peer_config_);
        peer_connection_->onLocalDescription([this](rtc::Description description) {
            if (description.typeString() == "answer") {
                send_signaling("ANSWER " + base64_encode(std::string(description)));
            }
        });
        peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
            send_signaling("CAND " + base64_encode(candidate.mid()) + " " +
                           base64_encode(candidate.candidate()));
        });
        peer_connection_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            if (dc->label() == "audio") {
                bind_audio_channel(std::move(dc));
            }
        });
    }

    void setup_signaling_socket() {
        signaling_socket_ = std::make_shared<rtc::WebSocket>();
        signaling_socket_->onOpen([this]() { send_signaling("JOIN " + std::to_string(user_id_)); });
        signaling_socket_->onClosed([this]() {
            std::cerr << "[client " << user_id_ << "] signaling closed\n";
            running_.store(false);
        });
        signaling_socket_->onMessage([this](const rtc::message_variant& message) {
            if (std::holds_alternative<std::string>(message)) {
                on_signaling_message(std::get<std::string>(message));
            }
        });
        signaling_socket_->open("ws://" + host_ + ":" + std::to_string(signaling_port_) + "/");
    }

    void bind_audio_channel(const std::shared_ptr<rtc::DataChannel>& dc) {
        dc->onOpen([this]() {
            std::scoped_lock lock(audio_dc_mutex_);
            audio_dc_open_ = true;
        });
        dc->onClosed([this]() {
            std::scoped_lock lock(audio_dc_mutex_);
            audio_dc_open_ = false;
            audio_dc_.reset();
        });
        dc->onMessage([this](const rtc::message_variant& message) { on_audio_packet(message); });

        std::scoped_lock lock(audio_dc_mutex_);
        audio_dc_ = dc;
    }

    void on_signaling_message(const std::string& text) {
        if (starts_with(text, "OFFER ")) {
            const auto sdp = base64_decode(text.substr(6));
            if (sdp) {
                peer_connection_->setRemoteDescription(rtc::Description(*sdp, "offer"));
                peer_connection_->setLocalDescription();
            }
            return;
        }
        if (starts_with(text, "CAND ")) {
            const auto payload = text.substr(5);
            const auto delim = payload.find(' ');
            if (delim == std::string::npos) {
                return;
            }
            const auto mid = base64_decode(payload.substr(0, delim));
            const auto candidate = base64_decode(payload.substr(delim + 1));
            if (mid && candidate) {
                peer_connection_->addRemoteCandidate(rtc::Candidate(*candidate, *mid));
            }
        }
    }

    void on_audio_packet(const rtc::message_variant& message) {
        if (!std::holds_alternative<rtc::binary>(message)) {
            return;
        }
        const auto& packet = std::get<rtc::binary>(message);
        if (packet.size() != 4 + kFrameBytes) {
            return;
        }
        rx_net_frames_.fetch_add(1, std::memory_order_relaxed);
        rx_net_bytes_.fetch_add(static_cast<std::uint64_t>(packet.size()), std::memory_order_relaxed);
        const auto b0 = static_cast<std::uint32_t>(std::to_integer<unsigned char>(packet[0]));
        const auto b1 = static_cast<std::uint32_t>(std::to_integer<unsigned char>(packet[1]));
        const auto b2 = static_cast<std::uint32_t>(std::to_integer<unsigned char>(packet[2]));
        const auto b3 = static_cast<std::uint32_t>(std::to_integer<unsigned char>(packet[3]));
        const std::uint32_t source_user_id = b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u);
        if (source_user_id == user_id_) {
            return;
        }

        std::array<std::int16_t, kSamplesPerFrame> frame {};
        std::memcpy(frame.data(), packet.data() + 4, kFrameBytes);

        std::scoped_lock lock(remote_tracks_mutex_);
        auto& track = remote_tracks_[source_user_id];
        track.frame = frame;
        track.last_frame_at = std::chrono::steady_clock::now();
    }

    void sender_loop() {
        using namespace std::chrono;
        const auto interval = milliseconds(20);
        auto next_tick = steady_clock::now() + interval;
        const std::array<std::int16_t, kSamplesPerFrame> silence {};

        while (running_.load()) {
            std::shared_ptr<rtc::DataChannel> dc;
            {
                std::scoped_lock lock(audio_dc_mutex_);
                if (audio_dc_open_) {
                    dc = audio_dc_;
                }
            }
            if (dc) {
                const auto frame = mic_enabled_ ? latest_captured_frame() : silence;
                rtc::binary payload(kFrameBytes);
                std::memcpy(payload.data(), frame.data(), kFrameBytes);
                dc->send(payload);
                tx_net_frames_.fetch_add(1, std::memory_order_relaxed);
                tx_net_bytes_.fetch_add(static_cast<std::uint64_t>(payload.size()), std::memory_order_relaxed);
            }
            std::this_thread::sleep_until(next_tick);
            next_tick += interval;
        }
    }

    std::array<std::int16_t, kSamplesPerFrame> latest_captured_frame() {
        std::scoped_lock lock(captured_frame_mutex_);
        return captured_frame_;
    }

    void mixer_loop() {
        using namespace std::chrono;
        const auto interval = milliseconds(20);
        auto next_tick = steady_clock::now() + interval;
        while (running_.load()) {
            mix_once();
            std::this_thread::sleep_until(next_tick);
            next_tick += interval;
        }
    }

    void mix_once() {
        std::vector<std::array<std::int16_t, kSamplesPerFrame>> tracks;
        {
            std::scoped_lock lock(remote_tracks_mutex_);
            const auto now = std::chrono::steady_clock::now();
            constexpr auto timeout = std::chrono::milliseconds(200);
            std::vector<std::uint32_t> stale;
            for (const auto& [user_id, track] : remote_tracks_) {
                if (now - track.last_frame_at > timeout) {
                    stale.push_back(user_id);
                } else {
                    tracks.push_back(track.frame);
                }
            }
            for (const auto user_id : stale) {
                remote_tracks_.erase(user_id);
            }
            active_tracks_.store(static_cast<std::uint32_t>(tracks.size()));
        }

        std::array<std::int16_t, kSamplesPerFrame> mix {};
        for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
            std::int32_t sum = 0;
            for (const auto& track : tracks) {
                sum += track[i];
            }
            sum = std::clamp(sum,
                             static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
                             static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max()));
            mix[i] = static_cast<std::int16_t>(sum);
        }
        {
            std::scoped_lock lock(playback_frame_mutex_);
            playback_frame_ = mix;
        }
        double energy = 0.0;
        for (const auto sample : mix) {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
        energy /= static_cast<double>(kSamplesPerFrame);
        rx_rms_.store(std::sqrt(energy));
        rx_frames_.fetch_add(1);
    }

    void stats_loop() {
        using namespace std::chrono;
        while (running_.load()) {
            std::this_thread::sleep_for(seconds(1));
            const auto tx_frames = tx_net_frames_.exchange(0, std::memory_order_relaxed);
            const auto rx_frames = rx_net_frames_.exchange(0, std::memory_order_relaxed);
            const auto tx_bytes = tx_net_bytes_.exchange(0, std::memory_order_relaxed);
            const auto rx_bytes = rx_net_bytes_.exchange(0, std::memory_order_relaxed);
            total_tx_net_bytes_.fetch_add(tx_bytes, std::memory_order_relaxed);
            total_rx_net_bytes_.fetch_add(rx_bytes, std::memory_order_relaxed);
            std::cout << "[client " << user_id_
                      << "] rx frames/s=" << rx_frames_.exchange(0)
                      << " net_tx_frames/s=" << tx_frames
                      << " net_rx_frames/s=" << rx_frames
                      << " net_tx_bytes/s=" << tx_bytes
                      << " net_rx_bytes/s=" << rx_bytes
                      << " total_net_tx_bytes=" << total_tx_net_bytes_.load(std::memory_order_relaxed)
                      << " total_net_rx_bytes=" << total_rx_net_bytes_.load(std::memory_order_relaxed)
                      << " mixed_tracks=" << active_tracks_.load()
                      << " mic_callbacks/s=" << captured_seq_.exchange(0)
                      << " rms=" << static_cast<int>(rx_rms_.load()) << "\n";
        }
    }

    void send_signaling(const std::string& text) {
        if (signaling_socket_) {
            signaling_socket_->send(text);
        }
    }

    void shutdown() {
        running_.store(false);
        {
            std::scoped_lock lock(audio_dc_mutex_);
            audio_dc_.reset();
            audio_dc_open_ = false;
        }
        peer_connection_.reset();
        signaling_socket_.reset();
        if (stream_ != nullptr) {
            (void)Pa_StopStream(stream_);
            (void)Pa_CloseStream(stream_);
            stream_ = nullptr;
        }
        if (audio_initialized_) {
            (void)Pa_Terminate();
            audio_initialized_ = false;
        }
    }

private:
    std::string host_;
    std::uint16_t signaling_port_ = 8000;
    std::uint32_t user_id_ = 1;
    bool mic_enabled_ = true;
    bool speaker_enabled_ = true;
    std::optional<int> mic_device_;
    std::optional<int> speaker_device_;
    rtc::Configuration peer_config_ {};

    std::atomic<bool> running_ {false};
    std::thread sender_thread_;
    std::thread mixer_thread_;
    std::thread stats_thread_;

    std::shared_ptr<rtc::PeerConnection> peer_connection_;
    std::shared_ptr<rtc::WebSocket> signaling_socket_;
    std::mutex audio_dc_mutex_;
    std::shared_ptr<rtc::DataChannel> audio_dc_;
    bool audio_dc_open_ = false;

    std::atomic<double> rx_rms_ {0.0};
    std::atomic<std::uint64_t> rx_frames_ {0};
    std::atomic<std::uint32_t> active_tracks_ {0};
    std::atomic<std::uint64_t> tx_net_frames_ {0};
    std::atomic<std::uint64_t> rx_net_frames_ {0};
    std::atomic<std::uint64_t> tx_net_bytes_ {0};
    std::atomic<std::uint64_t> rx_net_bytes_ {0};
    std::atomic<std::uint64_t> total_tx_net_bytes_ {0};
    std::atomic<std::uint64_t> total_rx_net_bytes_ {0};

    std::mutex remote_tracks_mutex_;
    std::unordered_map<std::uint32_t, RemoteTrack> remote_tracks_;

    bool audio_initialized_ = false;
    PaStream* stream_ = nullptr;
    std::mutex captured_frame_mutex_;
    std::array<std::int16_t, kSamplesPerFrame> captured_frame_ {};
    std::atomic<std::uint64_t> captured_seq_ {0};
    std::mutex playback_frame_mutex_;
    std::array<std::int16_t, kSamplesPerFrame> playback_frame_ {};
};

struct CliOptions {
    std::string host = "127.0.0.1";
    std::uint16_t signaling_port = 8000;
    std::uint32_t user_id = 1;
    bool mic_enabled = true;
    bool speaker_enabled = true;
    bool list_audio_devices = false;
    std::string config_path = "config.json";
    std::optional<int> mic_device;
    std::optional<int> speaker_device;
    std::optional<std::string> ice_bind_address;
    std::optional<std::uint16_t> ice_port_range_begin;
    std::optional<std::uint16_t> ice_port_range_end;
    std::vector<std::string> ice_servers;
};

CliOptions load_client_config(const std::string& path) {
    CliOptions options;
    options.config_path = path;

    const std::string json = read_file_or_empty(path);
    if (json.empty()) {
        return options;
    }
    if (const auto host = json_string(json, "host")) {
        options.host = *host;
    }
    if (const auto port = json_int(json, "signaling_port")) {
        if (*port < 1 || *port > 65535) {
            throw std::runtime_error("config signaling_port must be in [1, 65535]");
        }
        options.signaling_port = static_cast<std::uint16_t>(*port);
    }
    if (const auto id = json_int(json, "user_id")) {
        if (*id < 1) {
            throw std::runtime_error("config user_id must be >= 1");
        }
        options.user_id = static_cast<std::uint32_t>(*id);
    }
    if (const auto v = json_bool(json, "mic_enabled")) {
        options.mic_enabled = *v;
    }
    if (const auto v = json_bool(json, "speaker_enabled")) {
        options.speaker_enabled = *v;
    }
    if (const auto v = json_int(json, "mic_device")) {
        options.mic_device = *v;
    }
    if (const auto v = json_int(json, "speaker_device")) {
        options.speaker_device = *v;
    }
    if (const auto v = json_string(json, "ice_bind_address")) {
        options.ice_bind_address = *v;
    }
    if (const auto v = json_int(json, "ice_port_range_begin")) {
        options.ice_port_range_begin = static_cast<std::uint16_t>(*v);
    }
    if (const auto v = json_int(json, "ice_port_range_end")) {
        options.ice_port_range_end = static_cast<std::uint16_t>(*v);
    }
    options.ice_servers = json_string_array(json, "ice_servers");
    return options;
}

std::uint32_t parse_user_id(const std::string& value) {
    const auto id = std::stoul(value);
    if (id == 0 || id > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("user_id must be in [1, 2^32-1]");
    }
    return static_cast<std::uint32_t>(id);
}

rtc::Configuration build_peer_config(const CliOptions& options) {
    rtc::Configuration config;
    if (options.ice_bind_address) {
        config.bindAddress = *options.ice_bind_address;
    }
    if (options.ice_port_range_begin) {
        config.portRangeBegin = *options.ice_port_range_begin;
    }
    if (options.ice_port_range_end) {
        config.portRangeEnd = *options.ice_port_range_end;
    }
    if (config.portRangeBegin > config.portRangeEnd) {
        throw std::runtime_error("ice_port_range_begin must be <= ice_port_range_end");
    }
    for (const auto& server : options.ice_servers) {
        config.iceServers.emplace_back(server);
    }
    return config;
}

void list_audio_devices() {
    const PaError init_err = Pa_Initialize();
    if (init_err != paNoError) {
        throw std::runtime_error(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(init_err));
    }

    const PaDeviceIndex default_input = Pa_GetDefaultInputDevice();
    const PaDeviceIndex default_output = Pa_GetDefaultOutputDevice();
    const int device_count = Pa_GetDeviceCount();
    if (device_count < 0) {
        const auto err = static_cast<PaError>(device_count);
        Pa_Terminate();
        throw std::runtime_error(std::string("Pa_GetDeviceCount failed: ") + Pa_GetErrorText(err));
    }

    std::cout << "PortAudio devices:\n";
    for (int i = 0; i < device_count; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info) {
            continue;
        }
        const bool is_default_in = i == default_input;
        const bool is_default_out = i == default_output;
        std::cout << "  [" << i << "] " << info->name
                  << " in=" << info->maxInputChannels
                  << " out=" << info->maxOutputChannels;
        if (is_default_in || is_default_out) {
            std::cout << " default(";
            if (is_default_in) {
                std::cout << "mic";
            }
            if (is_default_in && is_default_out) {
                std::cout << ",";
            }
            if (is_default_out) {
                std::cout << "speaker";
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }
    Pa_Terminate();
}

CliOptions parse_args(int argc, char** argv) {
    std::string config_path = "config.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--config requires a file path");
            }
            config_path = argv[++i];
        }
    }
    CliOptions options = load_client_config(config_path);

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            ++i;
        } else if (arg == "--mic-off") {
            options.mic_enabled = false;
        } else if (arg == "--no-speaker") {
            options.speaker_enabled = false;
        } else if (arg == "--list-audio-devices") {
            options.list_audio_devices = true;
        } else if (arg == "--user-id") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--user-id requires integer id");
            }
            options.user_id = parse_user_id(argv[++i]);
        } else if (arg == "--mic-device") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--mic-device requires integer id");
            }
            options.mic_device = std::stoi(argv[++i]);
        } else if (arg == "--speaker-device") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--speaker-device requires integer id");
            }
            options.speaker_device = std::stoi(argv[++i]);
        } else {
            if (starts_with(arg, "--")) {
                throw std::runtime_error("unknown flag: " + arg +
                                         " (supported: --config PATH --user-id N --mic-off --no-speaker --list-audio-devices --mic-device N --speaker-device N)");
            }
            positional.push_back(arg);
        }
    }
    if (!positional.empty()) {
        options.host = positional[0];
    }
    if (positional.size() >= 2) {
        const int port = std::stoi(positional[1]);
        if (port < 1 || port > 65535) {
            throw std::runtime_error("signaling_port must be in [1, 65535]");
        }
        options.signaling_port = static_cast<std::uint16_t>(port);
    }
    if (positional.size() >= 3) {
        options.user_id = parse_user_id(positional[2]);
    }
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        if (options.list_audio_devices) {
            list_audio_devices();
            return 0;
        }
        VoiceClient client(options.host,
                           options.signaling_port,
                           options.user_id,
                           options.mic_enabled,
                           options.speaker_enabled,
                           options.mic_device,
                           options.speaker_device,
                           build_peer_config(options));
        client.run();
    } catch (const std::exception& ex) {
        std::cerr << "[client] fatal error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
