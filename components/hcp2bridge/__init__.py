import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import esp32, socket
from esphome import pins
from esphome.const import (
    CONF_API,
    CONF_DISABLED,
    CONF_FRAMEWORK,
    CONF_ID,
    CONF_OTA,
    CONF_PLATFORM,
    CONF_REBOOT_TIMEOUT,
    CONF_RX_PIN,
    CONF_SAFE_MODE,
    CONF_TX_PIN,
    CONF_TYPE,
    CONF_VARIANT,
    CONF_WIFI,
)
from esphome.core import CORE

DEPENDENCIES = []
AUTO_LOAD = ["socket"]
MULTI_CONF = True

CONF_BACKEND = "backend"
CONF_BENCH_ALLOW_AUTO_RESTART = "bench_allow_auto_restart"
CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS = "bench_allow_destructive_debug_actions"
CONF_BENCH_ENABLE_ASM_DMA_PROBE = "bench_enable_asm_dma_probe"
CONF_BENCH_ENABLE_REALTIME_UART = "bench_enable_realtime_uart"
CONF_BENCH_ALLOW_PIN_CONFLICTS = "bench_allow_pin_conflicts"
CONF_BENCH_ALLOW_REBOOT_SOURCES = "bench_allow_reboot_sources"
CONF_BENCH_ALLOW_UART0 = "bench_allow_uart0"
CONF_BENCH_ALLOW_UNSAFE_OTA = "bench_allow_unsafe_ota"
CONF_BUTTON_PRESS_DURATION = "button_press_duration"
CONF_DE_PIN = "de_pin"
CONF_DEVICE_SIGNATURE = "device_signature"
CONF_ESP32_REALTIME_BOARD_PROFILE = "esp32_realtime_board_profile"
CONF_HCP2BRIDGE_ID = "hcp2bridge_id"
CONF_HP_FALLBACK = "hp_fallback"
CONF_HTTP_DEBUG_PORT = "http_debug_port"
CONF_LP_UART_CLOCK_SOURCE = "lp_uart_clock_source"
CONF_PROTOCOL_LOG = "protocol_log"
CONF_RE_PIN = "re_pin"
CONF_RESPONSE_DELAY = "response_delay"
CONF_RESTART_POLICY = "restart_policy"
CONF_RS485_MODE = "rs485_mode"
CONF_SLAVE_ID = "slave_id"
CONF_UART_NUM = "uart_num"

BACKEND_ESP32C6_LP = "esp32c6_lp"
BACKEND_HP_FALLBACK = "hp_fallback"
BACKEND_ESP32_REALTIME = "esp32_realtime"
BACKEND_ESP32C6_HP_REALTIME = "esp32c6_hp_realtime"
BACKEND_ESP32C6_HP_ASM_DMA = "esp32c6_hp_asm_dma"

RS485_MODE_DE_RE = "de_re"
RS485_MODE_AUTO_DIRECTION = "auto_direction"

ESP32_REALTIME_BOARD_PROFILE_WROOM_NO_PSRAM = "esp32_wroom_no_psram"

RESTART_POLICY_NO_AUTO_RESTART = "no_auto_restart"
RESTART_POLICY_AUTO_RESTART = "auto_restart"

BACKENDS = {
    BACKEND_ESP32C6_LP: BACKEND_ESP32C6_LP,
    BACKEND_HP_FALLBACK: BACKEND_HP_FALLBACK,
    BACKEND_ESP32_REALTIME: BACKEND_ESP32_REALTIME,
    BACKEND_ESP32C6_HP_REALTIME: BACKEND_ESP32C6_HP_REALTIME,
    BACKEND_ESP32C6_HP_ASM_DMA: BACKEND_ESP32C6_HP_ASM_DMA,
}

RS485_MODES = {
    RS485_MODE_DE_RE: RS485_MODE_DE_RE,
    RS485_MODE_AUTO_DIRECTION: RS485_MODE_AUTO_DIRECTION,
}

ESP32_REALTIME_BOARD_PROFILES = {
    ESP32_REALTIME_BOARD_PROFILE_WROOM_NO_PSRAM: ESP32_REALTIME_BOARD_PROFILE_WROOM_NO_PSRAM,
}

RESTART_POLICIES = {
    RESTART_POLICY_NO_AUTO_RESTART: RESTART_POLICY_NO_AUTO_RESTART,
    RESTART_POLICY_AUTO_RESTART: RESTART_POLICY_AUTO_RESTART,
}

BACKEND_ENUMS = {
    BACKEND_ESP32C6_LP: "ESP32C6_LP",
    BACKEND_HP_FALLBACK: "HP_FALLBACK",
    BACKEND_ESP32_REALTIME: "ESP32_REALTIME",
    BACKEND_ESP32C6_HP_REALTIME: "ESP32C6_HP_REALTIME",
    BACKEND_ESP32C6_HP_ASM_DMA: "ESP32C6_HP_ASM_DMA",
}

RS485_MODE_ENUMS = {
    RS485_MODE_DE_RE: "DE_RE",
    RS485_MODE_AUTO_DIRECTION: "AUTO_DIRECTION",
}

ESP32_REALTIME_BOARD_PROFILE_ENUMS = {
    ESP32_REALTIME_BOARD_PROFILE_WROOM_NO_PSRAM: "ESP32_WROOM_NO_PSRAM",
}

RESTART_POLICY_ENUMS = {
    RESTART_POLICY_NO_AUTO_RESTART: "NO_AUTO_RESTART",
    RESTART_POLICY_AUTO_RESTART: "AUTO_RESTART",
}

LP_UART_CLOCK_SOURCES = {
    "xtal_d2": False,
    "default": True,
}

hcp2bridge_ns = cg.esphome_ns.namespace("hcp2bridge")
HCP2Bridge = hcp2bridge_ns.class_("HCP2Bridge", cg.Component)


def _enum_expression(enum_type, value):
    return cg.RawExpression(f"esphome::hcp2bridge::{enum_type}::{value}")


def _raw_backend(config):
    if config.get(CONF_HP_FALLBACK, False):
        return BACKEND_HP_FALLBACK
    return config.get(CONF_BACKEND, BACKEND_ESP32C6_LP)


def _apply_backend_defaults(config):
    config = dict(config)
    backend = _raw_backend(config)
    config.setdefault(CONF_BACKEND, backend)
    config.setdefault(CONF_RS485_MODE, RS485_MODE_DE_RE)
    config.setdefault(CONF_RESTART_POLICY, RESTART_POLICY_NO_AUTO_RESTART)
    if backend == BACKEND_ESP32_REALTIME:
        config.setdefault(CONF_RX_PIN, "GPIO16")
        config.setdefault(CONF_TX_PIN, "GPIO17")
        if config[CONF_RS485_MODE] == RS485_MODE_DE_RE:
            config.setdefault(CONF_DE_PIN, "GPIO18")
            config.setdefault(CONF_RE_PIN, "GPIO19")
    if backend in (BACKEND_ESP32C6_HP_REALTIME, BACKEND_ESP32C6_HP_ASM_DMA):
        config.setdefault(CONF_RX_PIN, "GPIO4")
        config.setdefault(CONF_TX_PIN, "GPIO5")
        if config[CONF_RS485_MODE] == RS485_MODE_DE_RE:
            config.setdefault(CONF_DE_PIN, "GPIO0")
            config.setdefault(CONF_RE_PIN, "GPIO1")
    return config


def validate_signature(value):
    value = cv.string_strict(value).replace(" ", "").replace(":", "").replace("-", "")
    if len(value) != 20:
        raise cv.Invalid("device_signature must contain exactly 10 bytes")
    try:
        return bytes.fromhex(value)
    except ValueError as err:
        raise cv.Invalid("device_signature must be hexadecimal") from err


def validate_esp_idf_framework(config):
    if CORE.using_arduino:
        raise cv.Invalid("hcp2bridge requires the ESP-IDF framework")
    return config


def validate_backend_config(config):
    if config[CONF_HP_FALLBACK] and config[CONF_BACKEND] != BACKEND_HP_FALLBACK:
        raise cv.Invalid("hp_fallback: true conflicts with backend; use backend: hp_fallback")

    backend = config[CONF_BACKEND]
    rs485_mode = config[CONF_RS485_MODE]

    realtime_backends = (BACKEND_ESP32_REALTIME, BACKEND_ESP32C6_HP_REALTIME)
    if backend not in realtime_backends and config.get(CONF_BENCH_ENABLE_REALTIME_UART, False):
        raise cv.Invalid("bench_enable_realtime_uart is only valid with realtime HCP2 backends")
    if backend == BACKEND_ESP32C6_HP_REALTIME and not config.get(CONF_BENCH_ENABLE_REALTIME_UART, False):
        raise cv.Invalid("backend: esp32c6_hp_realtime requires bench_enable_realtime_uart: true")
    if backend != BACKEND_ESP32C6_HP_ASM_DMA and config.get(CONF_BENCH_ENABLE_ASM_DMA_PROBE, False):
        raise cv.Invalid("bench_enable_asm_dma_probe is only valid with backend: esp32c6_hp_asm_dma")
    if backend == BACKEND_ESP32C6_HP_ASM_DMA and not config.get(CONF_BENCH_ENABLE_ASM_DMA_PROBE, False):
        raise cv.Invalid("backend: esp32c6_hp_asm_dma requires bench_enable_asm_dma_probe: true")

    if backend in (BACKEND_ESP32C6_LP, BACKEND_HP_FALLBACK) and rs485_mode != RS485_MODE_DE_RE:
        raise cv.Invalid(f"{backend} requires rs485_mode: de_re")

    if backend in (BACKEND_ESP32C6_LP, BACKEND_HP_FALLBACK, BACKEND_ESP32C6_HP_REALTIME, BACKEND_ESP32C6_HP_ASM_DMA):
        for key in (CONF_RX_PIN, CONF_TX_PIN, CONF_DE_PIN):
            if key not in config:
                raise cv.Invalid(f"{key} is required for backend: {backend}")

    if rs485_mode == RS485_MODE_DE_RE:
        if CONF_DE_PIN not in config or CONF_RE_PIN not in config:
            raise cv.Invalid("rs485_mode: de_re requires both de_pin and re_pin")

    return config


def _time_period_is_zero(value):
    if value is None:
        return True
    if hasattr(value, "total_microseconds"):
        return value.total_microseconds == 0
    if hasattr(value, "total_milliseconds"):
        return value.total_milliseconds == 0
    return str(value) in ("0", "0s", "0ms", "0us")


def _pin_number(pin_config):
    if pin_config is None:
        return None
    if isinstance(pin_config, dict):
        number = pin_config.get("number")
        if number is not None:
            try:
                return int(number)
            except (TypeError, ValueError):
                return None
    if isinstance(pin_config, str):
        upper = pin_config.upper()
        if upper.startswith("GPIO"):
            try:
                return int(upper[4:])
            except ValueError:
                return None
    return None


def _full_config_list(full_config, key):
    value = full_config.get(key, [])
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def _validate_single_instance(full_config):
    instances = _full_config_list(full_config, "hcp2bridge")
    if len(instances) > 1:
        raise cv.Invalid("hcp2bridge currently supports only one instance")


def _validate_esp32_realtime_final(config, full_config, esp32_config):
    if esp32_config.get(CONF_VARIANT) != esp32.VARIANT_ESP32:
        raise cv.Invalid("backend: esp32_realtime requires variant: ESP32")

    framework = esp32_config.get(CONF_FRAMEWORK, {})
    if framework.get(CONF_TYPE) != "esp-idf":
        raise cv.Invalid("backend: esp32_realtime requires the ESP-IDF framework")
    sdkconfig_options = framework.get("sdkconfig_options", {})
    if str(sdkconfig_options.get("CONFIG_FREERTOS_UNICORE", "")).lower() in ("1", "true", "yes", "y"):
        raise cv.Invalid("backend: esp32_realtime requires dual-core FreeRTOS; CONFIG_FREERTOS_UNICORE is unsupported")

    if CONF_ESP32_REALTIME_BOARD_PROFILE not in config:
        raise cv.Invalid("backend: esp32_realtime requires esp32_realtime_board_profile")

    if "psram" in full_config:
        raise cv.Invalid("backend: esp32_realtime supports ESP32-WROOM/no-PSRAM boards only")

    if config[CONF_UART_NUM] == 0 and not config[CONF_BENCH_ALLOW_UART0]:
        raise cv.Invalid("backend: esp32_realtime must not use UART0 unless bench_allow_uart0 is true")

    if (
        config[CONF_RESTART_POLICY] == RESTART_POLICY_AUTO_RESTART
        and not config[CONF_BENCH_ALLOW_AUTO_RESTART]
    ):
        raise cv.Invalid("restart_policy: auto_restart requires bench_allow_auto_restart: true")

    if full_config.get(CONF_OTA) and not config[CONF_BENCH_ALLOW_UNSAFE_OTA]:
        raise cv.Invalid("backend: esp32_realtime disables OTA by default; set bench_allow_unsafe_ota for bench-only tests")

    if not config[CONF_BENCH_ALLOW_REBOOT_SOURCES]:
        for domain in (CONF_API, CONF_WIFI):
            section = full_config.get(domain)
            if isinstance(section, dict) and not _time_period_is_zero(section.get(CONF_REBOOT_TIMEOUT)):
                raise cv.Invalid(f"backend: esp32_realtime requires {domain}.reboot_timeout: 0s")

        safe_mode = full_config.get(CONF_SAFE_MODE)
        if isinstance(safe_mode, dict) and not safe_mode.get(CONF_DISABLED, False):
            raise cv.Invalid("backend: esp32_realtime requires safe_mode.disabled: true")

    if not config[CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS]:
        for button in _full_config_list(full_config, "button"):
            if isinstance(button, dict) and button.get(CONF_PLATFORM) == "restart":
                raise cv.Invalid("backend: esp32_realtime rejects restart buttons by default")

    if not config[CONF_BENCH_ALLOW_PIN_CONFLICTS]:
        risky_pins = {0, 1, 2, 3, 5, 12, 15}
        for key in (CONF_RX_PIN, CONF_TX_PIN, CONF_DE_PIN, CONF_RE_PIN):
            number = _pin_number(config.get(key))
            if number in risky_pins:
                raise cv.Invalid(
                    f"backend: esp32_realtime pin {key}=GPIO{number} is boot/UART0 sensitive; "
                    "set bench_allow_pin_conflicts only for bench tests"
                )


def _validate_esp32c6_hp_realtime_final(config, full_config, esp32_config):
    if esp32_config.get(CONF_VARIANT) != esp32.VARIANT_ESP32C6:
        raise cv.Invalid("backend: esp32c6_hp_realtime requires variant: ESP32C6")

    framework = esp32_config.get(CONF_FRAMEWORK, {})
    if framework.get(CONF_TYPE) != "esp-idf":
        raise cv.Invalid("backend: esp32c6_hp_realtime requires the ESP-IDF framework")

    if config[CONF_UART_NUM] >= 2:
        raise cv.Invalid("backend: esp32c6_hp_realtime requires HP UART0 or UART1; UART2 is the 16-byte LP-UART")
    if config[CONF_UART_NUM] == 0 and not config[CONF_BENCH_ALLOW_UART0]:
        raise cv.Invalid("backend: esp32c6_hp_realtime must not use UART0 unless bench_allow_uart0 is true")

    if (
        config[CONF_RESTART_POLICY] == RESTART_POLICY_AUTO_RESTART
        and not config[CONF_BENCH_ALLOW_AUTO_RESTART]
    ):
        raise cv.Invalid("restart_policy: auto_restart requires bench_allow_auto_restart: true")

    if full_config.get(CONF_OTA) and not config[CONF_BENCH_ALLOW_UNSAFE_OTA]:
        raise cv.Invalid(
            "backend: esp32c6_hp_realtime disables OTA by default; set bench_allow_unsafe_ota for bench-only tests"
        )

    if not config[CONF_BENCH_ALLOW_REBOOT_SOURCES]:
        for domain in (CONF_API, CONF_WIFI):
            section = full_config.get(domain)
            if isinstance(section, dict) and not _time_period_is_zero(section.get(CONF_REBOOT_TIMEOUT)):
                raise cv.Invalid(f"backend: esp32c6_hp_realtime requires {domain}.reboot_timeout: 0s")

        safe_mode = full_config.get(CONF_SAFE_MODE)
        if isinstance(safe_mode, dict) and not safe_mode.get(CONF_DISABLED, False):
            raise cv.Invalid("backend: esp32c6_hp_realtime requires safe_mode.disabled: true")

    if not config[CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS]:
        for button in _full_config_list(full_config, "button"):
            if isinstance(button, dict) and button.get(CONF_PLATFORM) == "restart":
                raise cv.Invalid("backend: esp32c6_hp_realtime rejects restart buttons by default")


def _validate_esp32c6_hp_asm_dma_final(config, full_config, esp32_config):
    if esp32_config.get(CONF_VARIANT) != esp32.VARIANT_ESP32C6:
        raise cv.Invalid("backend: esp32c6_hp_asm_dma requires variant: ESP32C6")

    framework = esp32_config.get(CONF_FRAMEWORK, {})
    if framework.get(CONF_TYPE) != "esp-idf":
        raise cv.Invalid("backend: esp32c6_hp_asm_dma requires the ESP-IDF framework")

    if config[CONF_UART_NUM] >= 2:
        raise cv.Invalid("backend: esp32c6_hp_asm_dma requires HP UART0 or UART1; UART2 is the 16-byte LP-UART")
    if config[CONF_UART_NUM] == 0 and not config[CONF_BENCH_ALLOW_UART0]:
        raise cv.Invalid("backend: esp32c6_hp_asm_dma must not use UART0 unless bench_allow_uart0 is true")

    if (
        config[CONF_RESTART_POLICY] == RESTART_POLICY_AUTO_RESTART
        and not config[CONF_BENCH_ALLOW_AUTO_RESTART]
    ):
        raise cv.Invalid("restart_policy: auto_restart requires bench_allow_auto_restart: true")

    if full_config.get(CONF_OTA) and not config[CONF_BENCH_ALLOW_UNSAFE_OTA]:
        raise cv.Invalid(
            "backend: esp32c6_hp_asm_dma disables OTA by default; set bench_allow_unsafe_ota for bench-only tests"
        )

    if not config[CONF_BENCH_ALLOW_REBOOT_SOURCES]:
        for domain in (CONF_API, CONF_WIFI):
            section = full_config.get(domain)
            if isinstance(section, dict) and not _time_period_is_zero(section.get(CONF_REBOOT_TIMEOUT)):
                raise cv.Invalid(f"backend: esp32c6_hp_asm_dma requires {domain}.reboot_timeout: 0s")

        safe_mode = full_config.get(CONF_SAFE_MODE)
        if isinstance(safe_mode, dict) and not safe_mode.get(CONF_DISABLED, False):
            raise cv.Invalid("backend: esp32c6_hp_asm_dma requires safe_mode.disabled: true")

    if not config[CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS]:
        for button in _full_config_list(full_config, "button"):
            if isinstance(button, dict) and button.get(CONF_PLATFORM) == "restart":
                raise cv.Invalid("backend: esp32c6_hp_asm_dma rejects restart buttons by default")


def final_validate_schema(config):
    full_config = fv.full_config.get()
    _validate_single_instance(full_config)

    esp32_config = full_config.get("esp32", {})
    backend = config[CONF_BACKEND]
    variant = esp32_config.get(CONF_VARIANT)

    if backend == BACKEND_ESP32C6_LP:
        if variant != esp32.VARIANT_ESP32C6:
            raise cv.Invalid("backend: esp32c6_lp requires variant: ESP32C6")
        return

    if backend == BACKEND_HP_FALLBACK:
        if variant != esp32.VARIANT_ESP32C6:
            raise cv.Invalid("backend: hp_fallback currently requires variant: ESP32C6")
        return

    if backend == BACKEND_ESP32_REALTIME:
        _validate_esp32_realtime_final(config, full_config, esp32_config)
        return

    if backend == BACKEND_ESP32C6_HP_REALTIME:
        _validate_esp32c6_hp_realtime_final(config, full_config, esp32_config)
        return

    if backend == BACKEND_ESP32C6_HP_ASM_DMA:
        _validate_esp32c6_hp_asm_dma_final(config, full_config, esp32_config)
        return

    raise cv.Invalid(f"unsupported hcp2bridge backend: {backend}")


CONFIG_SCHEMA = cv.All(
    _apply_backend_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HCP2Bridge),
            cv.Optional(CONF_BACKEND, default=BACKEND_ESP32C6_LP): cv.enum(BACKENDS, lower=True),
            cv.Optional(CONF_RX_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_UART_NUM, default=1): cv.int_range(min=0, max=2),
            cv.Optional(CONF_SLAVE_ID, default=2): cv.int_range(min=1, max=247),
            cv.Optional(CONF_DEVICE_SIGNATURE, default="00000205043010FFA855"): validate_signature,
            cv.Optional(CONF_RESPONSE_DELAY, default="4200us"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_BUTTON_PRESS_DURATION, default="100ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_HP_FALLBACK, default=False): cv.boolean,
            cv.Optional(CONF_RS485_MODE, default=RS485_MODE_DE_RE): cv.enum(RS485_MODES, lower=True),
            cv.Optional(
                CONF_ESP32_REALTIME_BOARD_PROFILE,
            ): cv.enum(ESP32_REALTIME_BOARD_PROFILES, lower=True),
            cv.Optional(CONF_RESTART_POLICY, default=RESTART_POLICY_NO_AUTO_RESTART): cv.enum(RESTART_POLICIES, lower=True),
            cv.Optional(CONF_BENCH_ALLOW_UNSAFE_OTA, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ALLOW_AUTO_RESTART, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ALLOW_REBOOT_SOURCES, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ALLOW_UART0, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ALLOW_PIN_CONFLICTS, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ENABLE_ASM_DMA_PROBE, default=False): cv.boolean,
            cv.Optional(CONF_BENCH_ENABLE_REALTIME_UART, default=False): cv.boolean,
            cv.Optional(CONF_LP_UART_CLOCK_SOURCE, default="xtal_d2"): cv.enum(
                LP_UART_CLOCK_SOURCES, lower=True
            ),
            cv.Optional(CONF_HTTP_DEBUG_PORT, default=0): cv.int_range(min=0, max=65535),
            cv.Optional(CONF_PROTOCOL_LOG, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .add_extra(cv.only_on_esp32),
    validate_backend_config,
    validate_esp_idf_framework,
    socket.consume_sockets(2, "hcp2bridge_http_debug"),
    socket.consume_sockets(1, "hcp2bridge_http_debug", socket.SocketType.TCP_LISTEN),
)

FINAL_VALIDATE_SCHEMA = final_validate_schema


async def to_code(config):
    esp32.include_builtin_idf_component("driver")
    if config[CONF_BACKEND] == BACKEND_ESP32C6_LP:
        cg.add_build_flag("-DHCP2_EMBED_LP_BLOB=1")
        esp32.include_builtin_idf_component("ulp")
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_ENABLED", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_TYPE_LP_CORE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_RESERVE_MEM", 16320)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_SHARED_MEM", "0x10")
    if config[CONF_BACKEND] in (BACKEND_ESP32C6_HP_REALTIME, BACKEND_ESP32C6_HP_ASM_DMA):
        esp32.include_builtin_idf_component("ulp")
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_ENABLED", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_TYPE_LP_CORE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_COPROC_RESERVE_MEM", 16320)
        esp32.add_idf_sdkconfig_option("CONFIG_ULP_SHARED_MEM", "0x10")
    if config[CONF_BACKEND] in (BACKEND_ESP32_REALTIME, BACKEND_ESP32C6_HP_REALTIME) and config[CONF_BENCH_ENABLE_REALTIME_UART]:
        cg.add_build_flag("-DHCP2_ESP32_REALTIME_HOT_PATH=1")
        esp32.include_builtin_idf_component("esp_driver_gptimer")
        esp32.add_idf_sdkconfig_option("CONFIG_UART_ISR_IN_IRAM", True)
        esp32.add_idf_sdkconfig_option("CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM", True)
        esp32.add_idf_sdkconfig_option("CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM", True)
        esp32.add_idf_sdkconfig_option("CONFIG_GPTIMER_ISR_CACHE_SAFE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_GPIO_CTRL_FUNC_IN_IRAM", True)
    if config[CONF_BACKEND] == BACKEND_ESP32C6_HP_ASM_DMA and config[CONF_BENCH_ENABLE_ASM_DMA_PROBE]:
        cg.add_build_flag("-DHCP2_ESP32C6_ASM_DMA_EXPERIMENT=1")
        esp32.add_idf_sdkconfig_option("CONFIG_UART_ISR_IN_IRAM", True)
        esp32.add_idf_sdkconfig_option("CONFIG_GPIO_CTRL_FUNC_IN_IRAM", True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    rx_pin = await cg.gpio_pin_expression(config[CONF_RX_PIN])
    tx_pin = await cg.gpio_pin_expression(config[CONF_TX_PIN])

    cg.add(var.set_rx_pin(rx_pin))
    cg.add(var.set_tx_pin(tx_pin))
    if CONF_DE_PIN in config:
        de_pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])
        cg.add(var.set_de_pin(de_pin))
    if CONF_RE_PIN in config:
        re_pin = await cg.gpio_pin_expression(config[CONF_RE_PIN])
        cg.add(var.set_re_pin(re_pin))
    cg.add(
        var.set_backend_kind(
            _enum_expression("HCP2BackendKind", BACKEND_ENUMS[config[CONF_BACKEND]])
        )
    )
    cg.add(
        var.set_rs485_mode(
            _enum_expression("HCP2RS485Mode", RS485_MODE_ENUMS[config[CONF_RS485_MODE]])
        )
    )
    if CONF_ESP32_REALTIME_BOARD_PROFILE in config:
        cg.add(
            var.set_esp32_realtime_board_profile(
                _enum_expression(
                    "HCP2RealtimeBoardProfile",
                    ESP32_REALTIME_BOARD_PROFILE_ENUMS[config[CONF_ESP32_REALTIME_BOARD_PROFILE]],
                )
            )
        )
    cg.add(
        var.set_restart_policy(
            _enum_expression(
                "HCP2RestartPolicy",
                RESTART_POLICY_ENUMS[config[CONF_RESTART_POLICY]],
            )
        )
    )
    cg.add(var.set_bench_allow_destructive_debug_actions(config[CONF_BENCH_ALLOW_DESTRUCTIVE_DEBUG_ACTIONS]))
    cg.add(var.set_bench_enable_asm_dma_probe(config[CONF_BENCH_ENABLE_ASM_DMA_PROBE]))
    cg.add(var.set_bench_enable_realtime_uart(config[CONF_BENCH_ENABLE_REALTIME_UART]))
    cg.add(var.set_uart_num(config[CONF_UART_NUM]))
    cg.add(var.set_slave_id(config[CONF_SLAVE_ID]))
    for index, byte in enumerate(config[CONF_DEVICE_SIGNATURE]):
        cg.add(var.set_signature_byte(index, byte))
    cg.add(var.set_response_delay_us(config[CONF_RESPONSE_DELAY].total_microseconds))
    cg.add(var.set_button_press_us(config[CONF_BUTTON_PRESS_DURATION].total_microseconds))
    cg.add(var.set_lp_uart_clock_source_default(config[CONF_LP_UART_CLOCK_SOURCE]))
    cg.add(var.set_http_debug_port(config[CONF_HTTP_DEBUG_PORT]))
    cg.add(var.set_protocol_log_enabled(config[CONF_PROTOCOL_LOG]))
