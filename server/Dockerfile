# nmn-server: the online lobby/relay of every game (UDP 27750) in a small image.
#   docker build -t nmn-server .
#   docker run -d --init --name nmn --restart unless-stopped --network host nmn-server
# Options are environment variables (see docker-compose.yml): NMN_PORT, NMN_NAME, NMN_MAX_PER_IP, NMN_MAX_ROOMS, NMN_ROOM_TTL_MIN.
FROM rust:1-slim AS build
WORKDIR /src
COPY Cargo.toml ./
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim
RUN useradd --system --no-create-home nmn
COPY --from=build /src/target/release/nmn-server /usr/local/bin/nmn-server
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
USER nmn
EXPOSE 27750/udp
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
