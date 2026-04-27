FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ninja-build \
    pkg-config \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN if [ ! -f lib/libdatachannel/CMakeLists.txt ] || [ ! -f lib/libdatachannel/deps/usrsctp/CMakeLists.txt ]; then \
      echo "Bundled libdatachannel is missing or incomplete, cloning it..."; \
      rm -rf lib/libdatachannel; \
      mkdir -p lib; \
      git clone --depth 1 --recurse-submodules https://github.com/paullouisageneau/libdatachannel lib/libdatachannel; \
    fi

RUN cmake -S . -B build -G Ninja \
    -DBUILD_SHARED_LIBS=OFF \
    -DSKUFY_BUILD_SERVER=ON \
    -DSKUFY_BUILD_CLIENT=OFF \
    -DSKUFY_USE_BUNDLED_LIBDATACHANNEL=ON \
    && cmake --build build --target skufy_server

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/skufy_server/skufy_server /app/skufy_server

EXPOSE 8000
ENTRYPOINT ["/app/skufy_server"]
