#!/bin/sh
# V821 实时多轮语音聊天机器人 —— 启动脚本（busybox ash）
# 把前置条件一次做好：停官方应用 → 对时 → 起机器人
#
# 用法：
#   ./run_chatbot.sh              跑机器人（默认凭据 = SDK 例程的测试设备）
#   ./run_chatbot.sh --tune       只标定麦克风电平，用来定 VAD 阈值
#   ./run_chatbot.sh <devid> <secret_key> <local_key>

cd "$(dirname "$0")" || exit 1

echo "===== 1. 停掉官方应用，释放声卡 ====="
# daemon.sh 每 30 秒会把 xstar 拉起来，必须先杀看护
PIDS=$(pgrep -f daemon.sh)
[ -n "$PIDS" ] && kill $PIDS 2>/dev/null
killall xstar 2>/dev/null
sleep 1
if pgrep -f xstar >/dev/null; then
    echo "!! xstar 还在跑，声卡会被占用"
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
echo "（说话 → 自动识别 → 回答会边收边放；播放期间麦克风关闭。Ctrl+C 退出）"
echo
exec ./v821_chatbot "$@"
