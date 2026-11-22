#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "audio/mpx_audio.h"
#include "display/display_glue.h"
#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "network/chat_ws.h"
#include "network/http_server.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "pi/pi_link.h"
#if CONFIG_APP_POLICY_ENABLE
#include "rl/mpx_policy.h"
#endif
#include "agent/agent_runtime.h"
#include "mpx_enroll.h"
#include "mpx_identity.h"
#include "mpx_provision.h"
#include "mpx_selfprov.h"
#include "robot/battery.h"
#include "robot/mpx_robot.h"
#include "robot/robot.h"
#include "sdk/wasm_host_functions.h"
#include "util/sys_monitor.h"
#include "voice/voice_glue.h"
#include "wasm/registry.h"
#include "wasm/wasm_sandbox.h"

static const char *TAG = "main";

/* ── The robot's view of the WASM skill runtime ───────────────
 * robot.cc (mpv2's) does not include the sandbox; it asks these three
 * questions through mpx_robot_set_skill_hooks() so the gait loop knows when
 * a skill owns the servos and so the Pi link can stop one when it takes
 * control. The struct must outlive the registration: it is static. */
static const mpx_robot_skill_hooks_t s_skill_hooks = {
	/* The HTTP/network flag covers acceptance and teardown, when the WASM
	 * runtime itself is not entered yet (or has already exited). */
	.skill_is_running = [] { return network::skill_running() || wasm::is_running(); },
	.skill_owns_pose  = [] { return sdk::control_owner_is_pose(); },
	.request_stop     = [] { wasm::request_stop(); },
};

/* The Raspberry Pi took (true) or released (false) the robot. Runs on the
 * Pi link task: nothing here may block. The screen's "ROS" badge goes
 * through display_glue's queue; voice and music off (and hands-free back
 * when the Pi lets go) through voice_glue's. */
static void on_master_changed(bool active)
{
	ESP_LOGW(TAG, "Raspberry Pi %s control of the robot",
	         active ? "took" : "released");
	display_glue::on_master_changed(active);
	voice_glue::on_master_changed(active);
}

/* Internal RAM after each boot stage. The Wi-Fi driver needs internal RAM
 * for every packet; when this runs out the AP stays up but phones time out. */
static void log_internal_heap(const char *stage)
{
	ESP_LOGI(TAG, "internal RAM after %s: %u free, %u largest", stage,
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	ESP_LOGI(TAG, "PSRAM after %s: %u free, %u largest", stage,
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void init_nvs()
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
}

extern "C" void app_main(void)
{
	init_nvs();

	// ── Identity: the 16-byte ID and the AES key ──────────────
	// Straight after NVS, because everything below (chat, agent, WASM, HTTP,
	// the Pi link) either needs one of the two or needs to know there is none.
	// Failure is not fatal: a robot with no identity still walks, it just cannot
	// talk to the cloud.
	if (mpx_identity_init() != ESP_OK) {
		ESP_LOGW(TAG, "No robot identity -- cloud chat, robot token and MPXE are off");
	}
	// Factory image only (CONFIG_APP_FACTORY_SELF_PROVISION, off everywhere but
	// sdkconfig.defaults.factory): a blank board mints and burns its own ID and
	// root secret here, so one image flashed onto a tray of boards needs no
	// per-unit station run. The burn is irreversible and the HMAC peripheral
	// cannot use a key burned in the running boot, so a successful burn restarts
	// instead of continuing with the identity it just created; the next boot
	// loads it and enrolment seals and announces it. A failure is not fatal --
	// the robot boots without an identity, and the log says which board to
	// inspect.
	if (mpx_identity_state() == MPX_ID_UNPROVISIONED) {
		const esp_err_t selfprov = mpx_selfprov_boot();
		if (selfprov == ESP_OK) {
			esp_restart();
		} else if (selfprov != ESP_ERR_NOT_SUPPORTED &&
		           selfprov != ESP_ERR_INVALID_STATE) {
			ESP_LOGE(TAG, "Factory self-provisioning failed: %s",
			         esp_err_to_name(selfprov));
		}
	}
	// Provisioning listener: a no-op unless this is a factory build, and it
	// runs before the network so a board that has never been provisioned can
	// still be given an identity (it has no other way in).
	const esp_err_t prov = mpx_provision_init();
	if (prov != ESP_OK && prov != ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG, "Provisioning listener did not start: %s", esp_err_to_name(prov));
	}
	log_internal_heap("boot");

	// ── System monitor: CPU / RAM / task load to the log and the PWA ──
	// First, so its log lines cover everything the rest of boot allocates.
#if CONFIG_APP_SYSMON_ENABLE
	if (!util::sys_monitor_start()) {
		ESP_LOGW(TAG, "System monitor failed to start");
	}
#endif

	// ── Robot HAL (servo bus, gait task on Core 1) ─────────────
	mpx_robot_set_skill_hooks(&s_skill_hooks);
	robot::set_master_observer(on_master_changed);
	// Initialise through the facade: agent tools and the face use
	// mpx_robot_ready(), whose state is set by mpx_robot_init().
	if (!mpx_robot_init()) {
		ESP_LOGE(TAG, "Robot HAL init failed — continuing without servo control");
	} else {
		mpx_battery_start();   // the battery voltage from the servo boards, every 10 s
	}

	// ── Screen: ST7789 + LVGL, the face ───────────────────────
	// Straight after the robot (the eyes follow the IMU) and before
	// anything slow: the face is the first thing on the panel, and the
	// backlight only comes on once it is drawn (no white flash).
	display_glue::start_face();
	log_internal_heap("robot + screen");

	// ── Audio: microphones + speaker ──────────────────────────
	// After the robot (the IMU and the ES7210 share the I2C bus; the IMU
	// claims it first) and before the Pi link, which plays the Pi's music.
	if (mpx_audio_init() != ESP_OK) {
		ESP_LOGW(TAG, "Audio init failed — no microphone, no speaker");
	}

	// ── Raspberry Pi (ROS 2) link on J-PI ─────────────────────
	// Straight after the robot: a Pi that takes control must always be
	// able to lose it, so the link must not wait on the network.
#if CONFIG_APP_PI_LINK_ENABLE
	if (pi_link::init() != ESP_OK) {
		ESP_LOGW(TAG, "Pi link init failed — continuing standalone");
	}
#endif
	log_internal_heap("audio + Pi link");

	// Mount LittleFS (for .wasm storage and PWA assets)
	if (!fs::init_littlefs()) {
		ESP_LOGE(TAG, "LittleFS mount failed — wasm sandbox unavailable");
		return;
	}

	// ── Screen: the Display page's GIFs live on LittleFS ──────
	display_glue::start_animations();
	display_glue::show_boot_check();   /* a servo / SPI problem found at boot: on the screen for 30 s */
	display_glue::start_temp_watch();  /* first minute > 60 C = faulty; later > 80 C = too high (screen only) */

	// ── Network: start Wi-Fi AP (Core 0, Priority 6) ──────────
	if (!network::init_wifi_ap()) {
		ESP_LOGE(TAG, "Wi-Fi AP init failed");
		return;
	}

	// ── Network: initialise Wi-Fi STA (auto-connects if saved) ─
	// Announce the key if this unit has one and the cloud has not seen it yet.
	// It runs as a retrying task, so a robot that boots before its AP is up
	// still enrols once the link comes back.
	mpx_enroll_init();

	// D9: from here on the hardware RNG is a real entropy source, so a
	// provisioning run no longer needs the bootloader's.
	mpx_provision_set_rf_ready();
	if (!network::init_wifi_sta()) {
		ESP_LOGW(TAG, "Wi-Fi STA init failed — continuing in AP-only mode");
	}

	// ── Screen: network line under the eyes, Pi address, ROS badge ─
	display_glue::start_events();
	log_internal_heap("Wi-Fi");

	// ── Deploy embedded PWA assets to LittleFS ────────────────
	if (!network::deploy_www_assets()) {
		ESP_LOGE(TAG, "PWA asset deployment failed");
		return;
	}

	// ── Skill registry: what the installed .wasm files declare ─
	// Before the HTTP server, so /v1/robot/moves and /v1/skills/registry
	// are complete from the first request. Rescanned on upload and delete.
	skills::rescan();

	// ── Start HTTP + WebSocket server (Core 0, Priority 6) ───
	if (!network::start_http_server()) {
		ESP_LOGE(TAG, "HTTP server init failed");
		return;
	}

	// ── Chat pipeline (echo / encrypted cloud proxy) ─────────
	if (!network::init_chat_pipeline()) {
		ESP_LOGW(TAG, "Chat pipeline init failed — chat UI unavailable");
	}

	// ── Wasm sandbox ──────────────────────────────────────────
	if (!wasm::init_sandbox()) {
		ESP_LOGE(TAG, "WAMR sandbox init failed");
		return;
	}

	// ── Lua scripting VM ──────────────────────────────────────
	if (lua_init() != ESP_OK) {
		ESP_LOGE(TAG, "Lua VM init failed — continuing without Lua scripting");
	} else {
		ESP_LOGI(TAG, "Lua VM ready — robot bindings registered");
	}
	log_internal_heap("HTTP + chat + WASM + Lua");

	// ── Reserve the voice front end before the agent ──────────
	// ESP-SR 2.3.4 dereferences null model buffers after an allocation
	// failure. MMNR + WakeNet must therefore take their internal/PSRAM
	// before ESP-Claw; this does not open a socket or start capture tasks.
	voice_glue::prepare();
	log_internal_heap("voice front end");

	// ── The on-device agent (ESP-Claw) ────────────────────────
	// Its tools use the HTTP server and sandbox above. Voice networking and
	// tools start afterward, so the device-brain bridge still sees a running
	// agent even though its AFE was reserved first. Never fatal: with the
	// agent down the PWA's "cloud" brain still works.
	if (agent::start() != ESP_OK) {
		ESP_LOGW(TAG, "on-device agent not running — chat uses the cloud only");
	}
	log_internal_heap("agent");

	// ── Voice: gateway, MCP tools and music ───────────────────
	// The AFE is already reserved by prepare(); start() will not construct it
	// twice. Nothing touches the voice network until the first wake word.
	voice_glue::start();

	// Policies are deliberately lazy: main/network/http_policy.cc calls
	// mpx_policy_init() only for an actual load/run/test command. Boot and
	// status polling allocate no weights and create no policy tasks/timer.
}
