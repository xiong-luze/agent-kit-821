#!/bin/sh
# V821 实时多轮语音聊天机器人 —— 启动脚本（busybox ash）
# 把前置条件一次做好：停官方应用 → 对时 → 起机器人
#
# 用法：
#   ./run_chatbot.sh              跑机器人（默认凭据 = SDK 例程的测试设备）
#   ./run_chatbot.sh --tune       只标定麦克风电平，用来定 VAD 阈值
#   ./run_chatbot.sh <devid> <secret_key> <local_key>

cd "$(dirname "$0")" || exit 1

echo "===== 1. 释放声卡 ====="
# ★ 先杀我们自己的旧进程。adb 断开或 adb server 重启**不会**杀掉板上的进程，
# 于是上一次的实例还活着占着 playback。而它按设计在播放时会关掉 arecord，
# 所以症状是「录音和文字都正常，就是没声音」——踩过一次，很难查。
OLD=$(pgrep -f '[v]821_chatbot')
if [ -n "$OLD" ]; then
    echo "发现上一次的 v821_chatbot 还在跑（PID: $OLD），先杀掉"
    kill $OLD 2>/dev/null; sleep 1
    kill -9 $(pgrep -f '[v]821_chatbot') 2>/dev/null
fi
# 它的子进程可能被留下（父进程被 -9 时来不及回收）
killall aplay arecord 2>/dev/null

# daemon.sh 每 30 秒会把官方 xstar 拉起来，必须先杀看护
PIDS=$(pgrep -f daemon.sh)
[ -n "$PIDS" ] && kill $PIDS 2>/dev/null
killall xstar 2>/dev/null
sleep 1

BUSY=$(pgrep -f '[x]star|[a]play|[a]record|[v]821_chatbot')
if [ -n "$BUSY" ]; then
    echo "!! 还有进程占着声卡："
    echo "$BUSY"
else
    echo "OK，声卡已释放"
fi

echo
echo "===== 2. 对时（板子无 RTC，冷启动时间回 1970，TLS 会必挂）====="
date
if [ "$(date +%Y)" -lt 2025 ]; then
    ntpd -q -n -p ntp.aliyun.com || ntpd -q -n -p 203.107.6.88
    date
fi
if [ "$(date +%Y)" -lt 2025 ]; then
    echo "!! 对时失败，TLS 握手会失败。检查网络后重试"
    exit 1
fi

echo
echo "===== 3. 网络 ====="
ifconfig wlan0 2>/dev/null | grep "inet addr" || { echo "!! wlan0 没有 IP，先 wifi -c 连网"; exit 1; }

echo
echo "===== 4. 启动聊天机器人 ====="
VOICE_DUPLEX=${V821_FULL_DUPLEX:-0}
for VOICE_ARG in "$@"; do
    case "$VOICE_ARG" in
        --full-duplex) VOICE_DUPLEX=1 ;;
        --half-duplex) VOICE_DUPLEX=0 ;;
    esac
done
if [ "$VOICE_DUPLEX" = 1 ]; then
    echo "（持续采集 + AEC；本地唤醒/语音打断按配置启用。Ctrl+C 退出）"
else
    echo "（说话 → 自动识别 → 回答会边收边放；播放期间麦克风关闭。Ctrl+C 退出）"
fi
echo
exec ./v821_chatbot "$@"
