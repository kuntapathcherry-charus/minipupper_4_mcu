#include "mpx_selfprov.h"

#include "sdkconfig.h"

#if CONFIG_APP_FACTORY_SELF_PROVISION && CONFIG_APP_IDENTITY_SOURCE_EFUSE

#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"

#include "mpx_derive.h"
#include "mpx_efuse.h"
#include "mpx_efuse_record.h"
#include "mpx_id.h"

static const char *TAG = "mpx_selfprov";

/* A fresh robot starts at key version 1: the derivation message carries it, so
 * the cloud and the unit must agree, and the record's WR_DIS bit is set by the
 * burn -- it is never rewritten. Rotation raises the version in NVS (§6.9 C3). */
#define MPX_SELFPROV_KV 1

static void fill_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

esp_err_t mpx_selfprov_boot(void)
{
    mpx_efuse_record_t rec;
    mpx_efuse_record_t burnt;
    uint8_t secret[MPX_KEY_LEN];
    uint8_t block = 0;
    char robot_id[MPX_ID_STR_LEN + 1];
    esp_err_t err;

    err = mpx_efuse_read_record(&rec);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "already provisioned as %s -- not burning", rec.robot_id);
        return ESP_ERR_INVALID_STATE;
    }
    /* A key without a record is the residue of a station run that lost power
     * between the two halves (the burn commits both in one batch, so this is
     * rare, but the read path reports it too). Burning a second key here would
     * leave the first one's owner unidentifiable, and there is no way to tell a
     * half-burned board from a station mistake. Stop and say so. */
    if (mpx_efuse_find_hmac_key(&block)) {
        ESP_LOGE(TAG, "key block %d is burned but no ID record -- burned by a "
                      "station? refusing to guess", (int)block);
        return ESP_ERR_INVALID_STATE;
    }

    /* D9: at this point in the boot the radio is down, so esp_fill_random()
     * alone is not yet a true entropy source. S is the one value in this system
     * nothing can replace, so the bootloader's source is enabled for the few
     * milliseconds the ID and S take -- the same pattern mpx_provision.c uses
     * -- and disabled before anything touches Wi-Fi, because leaving it on
     * across radio init breaks RF. */
    bootloader_random_enable();

    if (mpx_id_generate(fill_random, robot_id) != 0) {
        bootloader_random_disable();
        ESP_LOGE(TAG, "could not generate an ID -- board left blank");
        return ESP_FAIL;
    }
    esp_fill_random(secret, sizeof secret);

    bootloader_random_disable();

    memset(&rec, 0, sizeof rec);
    /* The production ID fills the field exactly; the burn zero-pads anything
     * shorter, and this one is never shorter. */
    memcpy(rec.robot_id, robot_id, MPX_ID_STR_LEN);
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.flags   = 0;
    rec.kv_base = MPX_SELFPROV_KV;

    err = mpx_efuse_burn_record(&rec, secret, NULL);
    memset(secret, 0, sizeof secret);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "burn failed: %s -- board left unprovisioned",
                 esp_err_to_name(err));
        return err;
    }

    /* Read back rather than trusting the write path. This is the only check
     * between a bad burn and a tray of robots that cannot enrol, and it is
     * cheap: a Reed-Solomon-coded block is either what was written or the burn
     * failed. */
    if (mpx_efuse_read_record(&burnt) != ESP_OK ||
        strncmp(burnt.robot_id, robot_id, MPX_ID_STR_LEN) != 0 ||
        burnt.kv_base != MPX_SELFPROV_KV) {
        ESP_LOGE(TAG, "read-back does not match %s -- inspect this board",
                 robot_id);
        return ESP_FAIL;
    }

    /* Logged as a warning because it cannot be undone and whoever reads the
     * first boot of a board needs to see which unit this now is. The caller
     * reboots: the HMAC peripheral ignores a key burned in the running boot, so
     * the identity has no usable key until then. Enrolment (APP_ENROLL_ENABLE)
     * seals and announces it on the first connect afterwards. */
    ESP_LOGW(TAG, "burned %s kv=%u (irreversible) -- resetting to load it",
             robot_id, (unsigned)MPX_SELFPROV_KV);
    return ESP_OK;
}

#else

esp_err_t mpx_selfprov_boot(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
