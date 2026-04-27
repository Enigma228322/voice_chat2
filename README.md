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
SKUFY_BIND_ADDRESS=10.129.0.26 \
SKUFY_ICE_BIND_ADDRESS=10.129.0.26 \
./build/skufy_server/skufy_server
```

Terminal 2:
```bash
./build/skufy_client/skufy_client --config skufy_client/config.json --user-id 1 127.0.0.1 8000
```

Terminal 3:
```bash
./build/skufy_client/skufy_client --config skufy_client/config.json --user-id 2 127.0.0.1 8000
```

More clients can be started with unique user IDs.

## Run server in Docker

Build image:
```bash
docker build -f skufy_server/Dockerfile -t skufy-server:local .
```

Run container:
```bash
docker run --rm --network host \
  -e SKUFY_SIGNALING_PORT=8000 \
  -e SKUFY_BIND_ADDRESS=10.129.0.26 \
  -e SKUFY_ICE_BIND_ADDRESS=10.129.0.26 \
  -e SKUFY_ICE_PORT_RANGE_BEGIN=40000 \
  -e SKUFY_ICE_PORT_RANGE_END=40100 \
  -e SKUFY_ICE_SERVERS=stun:stun.l.google.com:19302 \
  skufy-server:local
```

Run container with custom signaling port:
```bash
docker run --rm --network host \
  -e SKUFY_SIGNALING_PORT=50000 \
  -e SKUFY_BIND_ADDRESS=10.129.0.26 \
  -e SKUFY_ICE_BIND_ADDRESS=10.129.0.26 \
  skufy-server:local
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

For custom signaling port:
```bash
SKUFY_SIGNALING_PORT=50000 docker compose up --build --force-recreate
```

The compose file uses `network_mode: host` on Linux so WebRTC ICE sees the host
network interfaces directly. With host networking, Docker `ports` mappings are
not used; open `signaling_port`/TCP and the configured ICE UDP port range on the
host firewall instead.

## Environment Variables

Server reads:
- `SKUFY_SIGNALING_PORT`
- `SKUFY_BIND_ADDRESS`
- `SKUFY_ICE_BIND_ADDRESS`
- `SKUFY_ICE_PORT_RANGE_BEGIN` / `SKUFY_ICE_PORT_RANGE_END`
- `SKUFY_ICE_SERVERS` (comma-separated STUN/TURN URLs)

Example for your remote server:
```bash
export SKUFY_BIND_ADDRESS=10.129.0.26
export SKUFY_ICE_BIND_ADDRESS=10.129.0.26
docker compose up --build --force-recreate
```

`--config skufy_server/config.json` is still supported for manual runs, but Docker
and Compose use environment variables by default.

## config.json (client setup)

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
./build/skufy_client/skufy_client 127.0.0.1 8000 --user-id 3 --mic-off
```

Run without speaker playback:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 --user-id 4 --no-speaker
```

List audio devices:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 --user-id 1 --list-audio-devices
```

Pick explicit mic/speaker by device id:
```bash
./build/skufy_client/skufy_client 127.0.0.1 8000 --user-id 1 --mic-device 2 --speaker-device 4
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
