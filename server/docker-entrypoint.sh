#!/bin/sh
# Turns the container's environment into nmn-server's command line (NMN_*; the old RRN1_* names still work). Extra arguments pass through.
exec /usr/local/bin/nmn-server --port "${NMN_PORT:-${RRN1_PORT:-27750}}" --bind 0.0.0.0 \
     --name "${NMN_NAME:-${RRN1_NAME:-Namco 22 online}}" --max-per-ip "${NMN_MAX_PER_IP:-${RRN1_MAX_PER_IP:-8}}" \
     --max-rooms "${NMN_MAX_ROOMS:-${RRN1_MAX_ROOMS:-128}}" --room-ttl-min "${NMN_ROOM_TTL_MIN:-${RRN1_ROOM_TTL_MIN:-10}}" \
     ${NMN_STATS_FILE:+--stats-file "$NMN_STATS_FILE"} "$@"
