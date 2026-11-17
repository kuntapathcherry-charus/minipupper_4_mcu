/* First-boot self-provisioning for factory images (docs/ROBOT_IDENTITY_AND_KEYS.md §6.6).
 *
 * The bench path burns an identity because someone runs tools/mpx_provision.py
 * against one board at a time. A factory that flashes a tray of boards with the
 * Flash Download Tool cannot do that: the tool writes flash only, and every
 * board receives the same image. So the image burns its own identity instead,
 * on the first boot of a blank chip.
 *
 * This is the same irreversible operation the station triggers, with the same
 * guards (one batch, WR_DIS/RD_DIS, refusal on an existing record) -- only the
 * trigger moved. It is compiled in only when CONFIG_APP_FACTORY_SELF_PROVISION
 * is set, which no shipping or bench profile does.
 */
#ifndef MPX_SELFPROV_H
#define MPX_SELFPROV_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Burns a fresh ID and root secret when this is a factory build and the board
 * has neither. Never touches a board that already carries a record.
 *
 * ESP_OK                 burned; the HMAC peripheral cannot use the new key
 *                        until a reset, so the caller must reboot before the
 *                        identity has a key.
 * ESP_ERR_NOT_SUPPORTED  not a factory build, or the identity source is not
 *                        eFuse. Nothing was read or written.
 * ESP_ERR_INVALID_STATE  board already provisioned, or half-burned (a record
 *                        without a key, or a key without a record). Logged with
 *                        the reason; re-provisioning it is a bench decision,
 *                        not something a boot should guess.
 * other                  burn or read-back failed. The board is left as the
 *                        fuses are; boot continues without an identity.
 */
esp_err_t mpx_selfprov_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* MPX_SELFPROV_H */
