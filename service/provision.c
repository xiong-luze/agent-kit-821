/* prov.c —— 见 prov.h */
#include "provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "iot_client.h"
#include "qrcodegen.h"

#define CREDS_PATH "/mnt/UDISK/tuya_creds.txt"

/* -- 凭据持久化 ---------------------------------------------------------- */

int prov_load(prov_creds_t *out)
{
    FILE *f = fopen(CREDS_PATH, "r");
    if (!f) return 0;
    memset(out, 0, sizeof(*out));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if      (!strcmp(k, "devid"))      snprintf(out->devid,      sizeof(out->devid),      "%s", v);
        else if (!strcmp(k, "secret_key")) snprintf(out->secret_key, sizeof(out->secret_key), "%s", v);
        else if (!strcmp(k, "local_key"))  snprintf(out->local_key,  sizeof(out->local_key),  "%s", v);
    }
    fclose(f);
    return out->devid[0] && out->secret_key[0] && out->local_key[0];
}

int prov_save(const prov_creds_t *in)
{
    FILE *f = fopen(CREDS_PATH, "w");
    if (!f) { fprintf(stderr, "[配网] 写不了 %s\n", CREDS_PATH); return 0; }
    fprintf(f, "devid=%s\nsecret_key=%s\nlocal_key=%s\n",
            in->devid, in->secret_key, in->local_key);
    fclose(f);
    chmod(CREDS_PATH, 0600);          /* 是密钥，别让别的进程随便读 */
    printf("[配网] 凭据已存到 %s\n", CREDS_PATH);
    return 1;
}

void prov_erase(void)
{
    if (unlink(CREDS_PATH) == 0) printf("[配网] 已清除凭据，下次启动重新配网\n");
}

/* -- 终端里画二维码（串口/adb 上也能扫） --------------------------------- */

static void print_qr_terminal(const uint8_t *qr)
{
    int size = qrcodegen_getSize(qr);
    const int quiet = 2;
    /* 一行字符画两行模块：上半块 U+2580 / 下半块 U+2584 / 全块 U+2588 */
    for (int y = -quiet; y < size + quiet; y += 2) {
        for (int x = -quiet; x < size + quiet; x++) {
            bool t = qrcodegen_getModule(qr, x, y);
            bool b = (y + 1 < size + quiet) ? qrcodegen_getModule(qr, x, y + 1) : false;
            if (t && b)      fputs("\xe2\x96\x88", stdout);
            else if (t)      fputs("\xe2\x96\x80", stdout);
            else if (b)      fputs("\xe2\x96\x84", stdout);
            else             fputc(' ', stdout);
        }
        fputc('\n', stdout);
    }
    fflush(stdout);
}

/* -- 配网主流程 ---------------------------------------------------------- */

int prov_run_qr(const char *uuid, const char *authkey, const char *product_key,
                prov_creds_t *creds,
                void (*status)(const char *),
                void (*show_qr)(const unsigned char *qr))
{
    iot_on_boarding_config_t ob = {
        .timeout_ms       = 180000,     /* 给足时间掏手机、开 App、扫码 */
        .mqtt_disable_tls = false,
        .env              = PROD,
    };
    memcpy((char *)ob.uuid,        uuid,        strlen(uuid));
    memcpy((char *)ob.authkey,     authkey,     strlen(authkey));
    memcpy((char *)ob.product_key, product_key, strlen(product_key));

    if (status) status("正在获取配网二维码...");

    iot_qrcode_request_t req = {
        .uuid = ob.uuid, .authkey = ob.authkey,
        .app_id = "", .type = 1, .env = ob.env,
    };
    iot_qrcode_response_t resp = {0};
    int rc = iot_get_qrcode_info(&req, &resp);
    if (rc != OPRT_OK || !resp.url) {
        fprintf(stderr, "[配网] 取二维码失败：%d\n", rc);
        if (status) status("取二维码失败");
        return -1;
    }
    printf("[配网] 激活 URL：%s\n", resp.url);

    /* qrcodegen 的缓冲要按最大版本给，是两个 ~3KB 的数组，放栈上太大 */
    uint8_t *qr  = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *tmp = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_MAX);
    int have_qr = 0;
    if (qr && tmp) {
        have_qr = qrcodegen_encodeText(resp.url, tmp, qr,
                                       qrcodegen_Ecc_LOW,
                                       qrcodegen_VERSION_MIN,
                                       qrcodegen_VERSION_MAX,
                                       qrcodegen_Mask_AUTO, true);
    }
    free(resp.url);
    free(tmp);

    if (have_qr) {
        printf("\n=== 用涂鸦 App 扫这个二维码激活 ===\n\n");
        print_qr_terminal(qr);
        printf("\n");
        if (show_qr) show_qr(qr);          /* 同时显示在屏上 */
        if (status) status("请用涂鸦App扫码");
    } else {
        printf("[配网] 二维码编码失败，用上面那个 URL\n");
        if (status) status("扫码失败，见日志URL");
    }

    printf("[配网] 等待 App 扫码激活（最多 %d 秒）...\n", ob.timeout_ms / 1000);
    iot_client_t *cli = iot_client_init_on_boarding(&ob);
    free(qr);

    if (!cli) {
        fprintf(stderr, "[配网] 激活失败或超时\n");
        if (status) status("激活失败/超时");
        return -1;
    }

    snprintf(creds->devid,      sizeof(creds->devid),      "%s", cli->devid);
    snprintf(creds->secret_key, sizeof(creds->secret_key), "%s", cli->secret_key);
    snprintf(creds->local_key,  sizeof(creds->local_key),  "%s", cli->local_key);
    printf("[配网] 激活成功 devid=%s\n", creds->devid);
    iot_client_deinit(cli);

    prov_save(creds);
    if (status) status("激活成功");
    return 0;
}
