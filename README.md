# skufy

Simple C++ voice prototype with:
- `skufy_server`: single-process WebRTC SFU-like forwarder
- `skufy_client`: WebRTC client with microphone + speaker I/O

## What this prototype does

- Uses `libdatachannel` for WebRTC PeerConnection + DataChannel transport.
- Server hosts WebSocket signaling and creates one WebRTC peer per user.
- Clients send 20ms PCM16 mono frames (48kHz) through WebRTC data channels.
- Server forwards each user's frame to all other users; client mixes remote tracks locally.

This is a **WebRTC transport prototype**. Audio is still raw PCM over DataChannel (no Opus/RTP).

## Dependencies

`skufy_client` uses PortAudio for real-time microphone capture and speaker playback.
Both `skufy_server` and `skufy_client` use `libdatachannel` for WebRTC.

Setup bundled `libdatachannel` as git submodule:
```bash
git submodule add https://github.com/paullouisageneau/libdatachannel lib/libdatachannel
git submodule update --init --recursive
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

Terminal 1:
```bash
./build/skufy_server/skufy_server --config skufy_server/config.json
```

Terminal 2:
```bash
./build/skufy_client/skufy_client --config skufy_client/config.json 127.0.0.1 8000 1
```

Terminal 3:
```bash
./build/skufy_client/skufy_client --config skufy_client/config.json 127.0.0.1 8000 2
```

More clients can be started with unique user IDs.

## Run server in Docker

Build image:
```bash
docker build -f skufy_server/Dockerfile -t skufy-server:local .
```

Run container:
```bash
docker run --rm -p 8000:8000 skufy-server:local
```

Run container with custom signaling port:
```bash
docker run --rm -p 50000:50000 skufy-server:local 50000
```

## Run server with Docker Compose

Start:
```bash
docker compose up --build --force-recreate
```

Stop:
```bash
docker compose down
```

For custom signaling port, edit `docker-compose.yml`:
- change `ports` mapping (`50000:50000`)
- change `command` to `["50000"]`

## config.json (remote host setup)

Server reads `--config skufy_server/config.json`:
- `signaling_port`
- `bind_address`
- `ice_bind_address`
- `ice_port_range_begin` / `ice_port_range_end`
- `ice_servers` (STUN/TURN URLs)

Client reads `--config skufy_client/config.json`:
- `host`
- `signaling_port`
- `user_id`
- `mic_enabled` / `speaker_enabled`
- `mic_device` / `speaker_device` (optional)
- `ice_bind_address`
- `ice_port_range_begin` / `ice_port_range_end`
- `ice_servers` (STUN/TURN URLs)

CLI args still override config values.

Run a listener-only client (no microphone transmit):
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 3 --mic-off
```

Run without speaker playback:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 4 --no-speaker
```

List audio devices:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 1 --list-audio-devices
```

Pick explicit mic/speaker by device id:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 1 --mic-device 2 --speaker-device 4
```

Each client prints local mixed-audio metrics:
- `rx frames/s`: packets received each second
- `mixed_tracks`: number of remote tracks currently being mixed
- `mic_callbacks/s`: number of captured mic frames each second
- `rms`: output mixed signal intensity

## How to evolve this into production-grade WebRTC

1. Move audio from DataChannel to WebRTC media tracks (RTP/RTCP).
2. Add Opus encode/decode and jitter buffer per stream.
3. Add TURN/STUN config, ICE restart policy, and robust reconnect.
4. Add per-stream VAD, AGC, noise suppression and active-speaker prioritization.
5. Add proper backpressure, pacing, congestion control and telemetry.
