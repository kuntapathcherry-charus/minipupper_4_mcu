# ── Source files (common to all designs) ─────────────────────
set(sources
    "main.cc"
    "fs/littlefs_manager.cc"
    "wasm/wasm_sandbox.cc"
    "wasm/wasm_decrypt.cc"
    "wasm/registry.cc"
    "sdk/wasm_host_functions.cc"
    "network/wifi_ap.cc"
    "network/captive.cc"
    "network/wifi_sta.cc"
    "network/http_server.cc"
    "network/www_assets.cc"
    "network/crypto.cc"
    "network/chat_ws.cc"
    "network/marketplace_proxy.cc"
    "network/preview_proxy.cc"
    # The robot: mpv2's gait, kinematics, moves and servo driver (local-claw
    # branch: these are mpv2-claw's components/mpx_robot/src/robot files, see
    # .cursor/PORT_NOTES.md). imu.cc stays this firmware's (shared I2C bus).
    "robot/robot.cc"
    "robot/battery.cc"
    "robot/driver_board.c"
    "robot/stanford_gait.cc"
    "robot/stanford_kinematics.cc"
    "robot/alive.c"
    "robot/imu.cc"
    "robot/mpx_robot_c_api.cc"
    # The Raspberry Pi link on J-PI: mpv2's ROS protocol, plus mp4-link's RL
    # host protocol on the same UART (pi/RL_LINK.md).
    "pi/pi_link.cc"
    "pi/pi_audio.cc"
    "pi/pi_video.cc"
    "pi/rl_link.cc"
    # Microphone + speaker (mpv2-claw's mpx_audio, direct I2S/codec init).
    "audio/mpx_audio.c"
    "audio/mpx_audio_tune.c"
    # The screen: mpv2-claw's cap_display (face, GIFs, orientation, the Pi's
    # camera picture) on a small display_hw service (ST7789 + esp_lvgl_port)
    # in place of ESP-Claw's display_service. Compiled with the display off
    # too (CONFIG_APP_DISPLAY_ENABLE=n): every entry point then says "no panel".
    "display/display_hw.c"
    "display/cap_display.c"
    "display/cap_display_face.c"
    "display/cap_display_orient.c"
    "display/display_assets.cc"
    "display/display_glue.cc"
    "network/http_display.cc"
    # Voice: the AI & Voice setting (/v1/ai) and the voice routes exist in
    # every build; what they can do depends on CONFIG_APP_VOICE_ENABLE.
    "network/http_voice.cc"
    "voice/voice_glue.cc"
    "platform/shared_i2c.cc"
    "util/trace_ring.cc"
    "util/sys_monitor.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
    "lua/lua_display.cc"
    "lua/lua_json.cc"
    "lua/lua_tools.cc"
    # The on-device agent (Phase 5). The owner's AI mode and the robot's
    # tool table exist in every build (the voice server's MCP and /v1/ai use
    # them); agent_runtime.cc compiles to stubs when the agent is off.
    "agent/agent_config.cc"
    "agent/robot_tools.cc"
    "agent/agent_runtime.cc"
)

# ESP-Claw itself: the filesystem roots, the tools as capabilities, and the
# link-time override of its event router stack. These include ESP-Claw
# headers and read CONFIG_APP_AGENT_* symbols that only exist when it is on.
if(CONFIG_APP_AGENT_ENABLE)
    list(APPEND sources
        "agent/agent_fs.cc"
        "agent/agent_caps.cc"
        "agent/agent_lua.cc"
        "agent/claw_overrides.c"
    )
endif()

# The voice stack itself (mpv2-claw's mpx_voice_link, mpx_voice_wake and
# cap_music, plus the native MCP bridge) only when it is on: these files
# include esp-sr, Opus and the audio player, and a board with voice off has
# no reason to compile them. Kconfig values are available here (unlike in a
# REQUIRES list, which is expanded before them), so gating SRCS is safe.
if(CONFIG_APP_VOICE_ENABLE)
    list(APPEND sources
        "voice/mpx_voice_codec.cc"
        "voice/mpx_voice_ws.c"
        "voice/mpx_voice_ota.c"
        "voice/mpx_voice_stream.c"
        "voice/mpx_voice_wake.c"
        "voice/cap_music.c"
        "voice/voice_brain.cc"
        "voice/voice_brain_text.c"
        "voice/voice_history.cc"
        "voice/voice_phone_audio.cc"
        "voice/voice_tools.cc"
    )
endif()

# ── RL policies on the ESP32-S3 itself (main/rl/, from the rl branch) ──
# The runner owns the servos through robot::host and holds voice/audio for
# the whole run. Optional policy files are embedded only when present.
set(policy_files "")
if(CONFIG_APP_POLICY_ENABLE)
    list(APPEND sources
        "rl/policy_core.c"
        "rl/policy_run.cc"
    )
    list(APPEND policy_files "rl/policies/default.mpxp")
    foreach(p
        getup climb terrain sku1 sku1_terrain sku1_terr11k sku1_terr15k
        sku1v2_stairs sku1v2_405_stairs sku1v2_405_walk sku1v2_405v2_stairs
        sku2_stairs sku2_recovery sku1v2_recovery getup_sku1
    )
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rl/policies/${p}.mpxp")
            list(APPEND policy_files "rl/policies/${p}.mpxp")
            set(policy_have_${p} 1)
        endif()
    endforeach()
endif()
# The policy files are embedded by our own .S files, not EMBED_FILES: EMBED_FILES packs every embedded file back to
# back with no alignment, so a policy could start off a 4-byte boundary, and the runtime reads its floats where
# they are (pol_parse: "not 4-byte aligned"; copying a 393 KB policy needs one free block that big, which a
# fragmented PSRAM may not have). Each file here starts 16-byte aligned, with the same symbols EMBED_FILES makes
# (_binary_<name>_mpxp_start / _end), so rl/policy_run.cc is unchanged.
set(policy_asm "")
if(policy_files AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    foreach(f ${policy_files})
        get_filename_component(pname "${f}" NAME_WE)
        file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/${f}" psrc)
        set(pasm "${CMAKE_CURRENT_BINARY_DIR}/policy_${pname}.S")
        set(ptext "/* Generated by main/CMakeLists.txt from ${f}: embedded 16-byte aligned. */\n")
        string(APPEND ptext "    .section .rodata.mpxp_${pname}, \"a\"\n")
        string(APPEND ptext "    .balign 16\n")
        string(APPEND ptext "    .global _binary_${pname}_mpxp_start\n")
        string(APPEND ptext "_binary_${pname}_mpxp_start:\n")
        string(APPEND ptext "    .incbin \"${psrc}\"\n")
        string(APPEND ptext "    .global _binary_${pname}_mpxp_end\n")
        string(APPEND ptext "_binary_${pname}_mpxp_end:\n")
        # Written only when it changes, so a reconfigure does not rebuild it.
        set(pold "")
        if(EXISTS "${pasm}")
            file(READ "${pasm}" pold)
        endif()
        if(NOT pold STREQUAL ptext)
            file(WRITE "${pasm}" "${ptext}")
        endif()
        # .incbin is not seen by the dependency scanner: rebuild when the policy file changes.
        set_source_files_properties("${pasm}" PROPERTIES OBJECT_DEPENDS "${psrc}")
        list(APPEND policy_asm "${pasm}")
    endforeach()
endif()

# The route module remains compiled when policy is off; its stub leaves the
# routes unregistered and the app's failed feature discovery hides RL Walk.
list(APPEND sources "network/http_policy.cc")

# ── Phone app assets (pwa-claw/, built into pwa-claw/www) ────
# The mpv2-claw phone app. It carries a three.js model viewer and its 157 KB
# model, so the big files are embedded as .gz ONLY: every browser that can run
# this app accepts gzip, and static_handler() falls back to the .gz even for
# one that does not.
set(pwa_www_dir "../pwa-claw/www")
set(design_assets
    "../pwa-claw/www/md.svg"
    "../pwa-claw/www/md.svg.gz"
    "../pwa-claw/www/eye-open.svg"
    "../pwa-claw/www/eye-open.svg.gz"
    "../pwa-claw/www/eye-close.svg"
    "../pwa-claw/www/eye-close.svg.gz"
    "../pwa-claw/www/m-pupper3d.js.gz"
    "../pwa-claw/www/m-preview3d.js.gz"
    # shared three.js chunk (pupper3d + preview3d)
    "../pwa-claw/www/m-three.module.js.gz"
    "../pwa-claw/www/pupper_model.json.gz"
)

# ── App shell ─────────────────────────────────────────────────
set(common_assets
    "${pwa_www_dir}/index.html"
    "${pwa_www_dir}/index.html.gz"
    "${pwa_www_dir}/index.css.gz"
    "${pwa_www_dir}/m.js.gz"
    "${pwa_www_dir}/manifest.json"
    "${pwa_www_dir}/manifest.json.gz"
    "${pwa_www_dir}/sw.js"
    "${pwa_www_dir}/sw.js.gz"
)

# ── Custom eFuse table ───────────────────────────────────────
# ESP-IDF generates main/esp_efuse_custom_table.c from
# main/esp_efuse_custom_table.csv but does not compile it: the application has
# to, and it is where the robot identity fields in BLOCK_USR_DATA live. The
# generated header lands in main/include, which is already an include dir.
if(CONFIG_EFUSE_CUSTOM_TABLE)
    # GENERATED matters: on a clean checkout the file does not exist yet when
    # CMake configures, and add_library() would fail with "Cannot find source
    # file". The build dependency added after registration makes the efuse
    # component's generator run before this component compiles.
    set(mpx_efuse_table "${CMAKE_CURRENT_LIST_DIR}/esp_efuse_custom_table.c")
    set_source_files_properties("${mpx_efuse_table}" PROPERTIES GENERATED TRUE)
    list(APPEND sources "${mpx_efuse_table}")
endif()

# main/include only holds generated output (the eFuse table header) and is
# git-ignored, so a fresh clone has no such folder. ESP-IDF refuses to
# configure with a missing INCLUDE_DIRS entry, so create it here; the
# efuse-custom-table step fills it during the build.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/include")

# ── Register component ───────────────────────────────────────
idf_component_register(
    SRCS        ${sources} ${policy_asm}
    # "include" holds generated headers (the custom eFuse table); listing
    # INCLUDE_DIRS explicitly replaces the default, so it has to be named.
    INCLUDE_DIRS "." "include" "fs" "wasm" "sdk" "network" "robot" "rl" "lua" "platform" "util" "pi" "audio" "display" "voice" "agent"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
        # The Display page's built-in animations, copied to <fs>/display at boot.
        "display/assets/blink.gif"
        "display/assets/smile.gif"
        "display/assets/sleepy.gif"
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        esp_codec_dev
        esp_lvgl_port
        lvgl
        esp_new_jpeg
        # Voice (main/voice): esp-sr for the wake word, the xiaozhi author's
        # Opus wrappers (esp-opus-encoder keeps esp-opus private, so opus.h
        # has to be asked for by name), the websocket client, the audio
        # player for music and the TLS/HTTP/NVS/app-desc pieces around them.
        esp-sr
        esp-opus-encoder
        esp-opus
        esp_websocket_client
        esp_audio_simple_player
        gmf_core
        gmf_io
        esp_http_client
        esp-tls
        app_update
        driver
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2s
        esp_driver_ledc
        esp_lcd
        esp_timer
        esp_http_server
        esp_wifi
        wpa_supplicant      # esp_eap_client.h: WPA2-Enterprise (eduroam) logins
        esp_netif
        lwip
        esp_event
        mbedtls
        json
        # The generated custom eFuse table includes esp_efuse.h.
        efuse
        # The robot's ID and AES key. Everything that needs either one asks
        # this component, so the source (Kconfig today, eFuse on a provisioned
        # unit) is decided in one place. components/mpx_identity/
        mpx_identity
        # The on-device agent: ESP-Claw (third-party/esp-claw, pulled in by
        # main/idf_component.yml). app_claw is the shell, claw_core has
        # claw_paths, claw_cap the capability types, cap_im_local the "web"
        # chat channel; claw_event_router for the stack override's header.
        app_claw
        claw_core
        claw_cap
        cap_im_local
        claw_event_router
        # claw_memory's public delete-session-history helper, used when the
        # voice panel clears a conversation's context.
        claw_memory
)

# The only references to the custom table's field descriptors come from
# components/mpx_identity, whose archive the linker opens before libmain.a;
# without a forced reference the table's object is dropped and the identity
# component fails to link. Naming one field pulls the whole object in.
if(CONFIG_EFUSE_CUSTOM_TABLE)
    add_dependencies(${COMPONENT_LIB} efuse-custom-table)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ESP_EFUSE_USER_DATA_MPX_ROBOT_ID")
endif()

# Keep optional-policy detection and the hot arithmetic consistent with rl.
foreach(p
    getup climb terrain sku1 sku1_terrain sku1_terr11k sku1_terr15k
    sku1v2_stairs sku1v2_405_stairs sku1v2_405_walk sku1v2_405v2_stairs
    sku2_stairs sku2_recovery sku1v2_recovery getup_sku1
)
    if(policy_have_${p})
        string(TOUPPER "${p}" p_upper)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MPX_POLICY_HAVE_${p_upper}=1)
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/rl/policies")
set_source_files_properties("rl/policy_core.c" PROPERTIES COMPILE_OPTIONS "-O2")

# Lua requires 32-bit mode on Xtensa/RISC-V architectures.
# This must match the Lua component's own definition so that
# including lua.h from our code sees LUA_32BITS=1.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)

# ESP-Claw starts its event router with an 8 KB stack and has no option for
# it. Rather than edit the vendored tree, the linker hands every call to
# claw_event_router_init() to __wrap_claw_event_router_init() in
# agent/claw_overrides.c, which raises the stack and calls on. The -u keeps
# the wrapper out of the linker's garbage collection (nothing references it
# by name). mpv2-claw's main/CMakeLists.txt does the same.
if(CONFIG_APP_AGENT_ENABLE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=claw_event_router_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_claw_event_router_init")
endif()