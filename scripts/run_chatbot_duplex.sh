#!/bin/sh
# Use the existing board preflight, with explicit local voice feature configuration.
cd "$(dirname "$0")" || exit 1
if [ ! -r ./voice_duplex.env ]; then
    echo '缺少 voice_duplex.env；请先复制并编辑 voice_duplex.env.example' >&2
    exit 1
fi
set -a
. ./voice_duplex.env || exit 1
set +a
exec sh ./run_chatbot.sh --full-duplex "$@"
