/*
 * prov.h —— 涂鸦 App 扫码配网激活 + 凭据持久化
 *
 * 流程（对应 SDK 的 examples/posix/pair/scan-by-app）：
 *   iot_get_qrcode_info()  向云端要一个激活 URL
 *     → 编成二维码显示在屏上（320×240 够放）
 *     → iot_client_init_on_boarding()  阻塞等 App 扫码，最长 120 秒
 *     → 拿到 devid / secret_key / local_key，存盘
 *   下次开机直接读盘，不再配网。
 *
 * 注意这是"设备已联网"的绑定流程：板子必须先连上 WiFi（run_chatbot.sh /
 * xstar 已保证），二维码只负责把设备绑到你的涂鸦账号并激活。
 * WiFi 本身的配网（App 下发 SSID/密码）是另一回事，需要 BLE 或 SoftAP，
 * 本板有 BT 硬件（XRADIO combo）但还没做。
 */
#ifndef PROVISION_H
#define PROVISION_H

typedef struct {
    char devid[40];
    char secret_key[40];
    char local_key[40];
} prov_creds_t;

/* 从 /mnt/UDISK/tuya_creds.txt 读。1 = 读到完整的一组。 */
int  prov_load(prov_creds_t *out);
/* 写盘（0600）。1 = 成功。 */
int  prov_save(const prov_creds_t *in);
/* 删除（重新配网用） */
void prov_erase(void);

/* 跑一次扫码配网：显二维码 → 等 App 扫 → 存盘。
 * 0 = 成功（creds 已填好并落盘）。status 可为 NULL（用于上屏）。
 * show_qr 把二维码交给 UI 显示，可为 NULL（那就只打终端）。 */
int  prov_run_qr(const char *uuid, const char *authkey, const char *product_key,
                 prov_creds_t *creds,
                 void (*status)(const char *),
                 void (*show_qr)(const unsigned char *qr));

#endif /* PROVISION_H */
