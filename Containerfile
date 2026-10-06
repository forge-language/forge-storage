FROM alpine:3.20 AS builder
RUN apk add --no-cache build-base cmake pkgconf json-c-dev libmicrohttpd-dev curl-dev openssl-dev
WORKDIR /src
COPY vendor-toolchain vendor/toolchain
COPY vendor-web vendor/forge-web
COPY . storage
RUN cmake -S vendor/toolchain -B build/toolchain && cmake --build build/toolchain -j4
RUN cmake -S vendor/forge-web -B build/web -DBUILD_TESTING=OFF && cmake --build build/web -j4
RUN cmake -S storage -B build/storage && cmake --build build/storage -j4
RUN build/toolchain/bin/forge storage/service/main.fg --emit-c -o build/main.c -I storage -I storage/service -I vendor/forge-web --forge-root vendor/toolchain --lib-dir build/toolchain/lib \
 && cat storage/src/http.c >> build/main.c \
 && cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -I vendor/toolchain/include build/main.c build/storage/libforge_storage.a build/web/libforge_web.a build/toolchain/lib/libforge_runtime.a build/toolchain/lib/libforge_std.a $(pkg-config --cflags --libs libmicrohttpd json-c libcurl openssl) -lpthread -lm -o build/forge-storage
FROM alpine:3.20
RUN apk add --no-cache libmicrohttpd json-c libcurl openssl ca-certificates curl && mkdir /data && chown 65534:65534 /data
COPY --from=builder /src/build/forge-storage /usr/local/bin/forge-storage
USER 65534:65534
EXPOSE 8090
HEALTHCHECK --interval=10s --timeout=3s CMD curl -fsS http://127.0.0.1:8090/health || exit 1
CMD ["forge-storage"]
