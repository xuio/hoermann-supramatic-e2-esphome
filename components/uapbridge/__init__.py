import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome import pins

DEPENDENCIES = ["uart"]
MULTI_CONF = True
CONF_RTS_PIN = "rts_pin"
CONF_AUTO_CORRECTION = "auto_correction"
CONF_ALLOW_REMOTE_CLOSE = "allow_remote_close"
CONF_ALLOW_REMOTE_IMPULSE = "allow_remote_impulse"
CONF_USE_UNVERIFIED_STOP_COMMAND = "use_unverified_stop_command"
CONF_REQUIRE_FRESH_BROADCAST_FOR_COMMANDS = "require_fresh_broadcast_for_commands"
CONF_COMMAND_TIMEOUT = "command_timeout"
CONF_DIAGNOSTIC_MODE = "diagnostic_mode"
CONF_LISTEN_ONLY = "listen_only"
CONF_VALID_BROADCAST_TIMEOUT = "valid_broadcast_timeout"

# Create UAPBridge namespace
uapbridge_ns = cg.esphome_ns.namespace("uapbridge")
UAPBridge = uapbridge_ns.class_("UAPBridge", cg.Component, uart.UARTDevice)

CONF_UAPBRIDGE_ID = "uapbridge_id"

CONFIG_SCHEMA_BASE = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UAPBridge),
        cv.Optional(CONF_RTS_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_AUTO_CORRECTION, default=False): cv.boolean,
        cv.Optional(CONF_ALLOW_REMOTE_CLOSE, default=False): cv.boolean,
        cv.Optional(CONF_ALLOW_REMOTE_IMPULSE, default=False): cv.boolean,
        cv.Optional(CONF_USE_UNVERIFIED_STOP_COMMAND, default=False): cv.boolean,
        cv.Optional(CONF_REQUIRE_FRESH_BROADCAST_FOR_COMMANDS, default=True): cv.boolean,
        cv.Optional(CONF_COMMAND_TIMEOUT, default="1200ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DIAGNOSTIC_MODE, default=False): cv.boolean,
        cv.Optional(CONF_LISTEN_ONLY, default=False): cv.boolean,
        cv.Optional(CONF_VALID_BROADCAST_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
    }
).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA)

def CONFIG_SCHEMA(conf):
    if conf:
        raise cv.Invalid(
            "Invalid operation to use baseclass in config\n"
            "either use uapbridge_esp or uapbridge_pic16"
        )


async def to_code_base(var, config):
    await cg.register_component(var, config)

    if CONF_RTS_PIN in config:
        rts_pin = await cg.gpio_pin_expression(config[CONF_RTS_PIN])
        cg.add(var.set_rts_pin(rts_pin))

    cg.add(var.set_auto_correction(config[CONF_AUTO_CORRECTION]))
    cg.add(var.set_allow_remote_close(config[CONF_ALLOW_REMOTE_CLOSE]))
    cg.add(var.set_allow_remote_impulse(config[CONF_ALLOW_REMOTE_IMPULSE]))
    cg.add(var.set_use_unverified_stop_command(config[CONF_USE_UNVERIFIED_STOP_COMMAND]))
    cg.add(var.set_require_fresh_broadcast_for_commands(config[CONF_REQUIRE_FRESH_BROADCAST_FOR_COMMANDS]))
    cg.add(var.set_command_timeout(config[CONF_COMMAND_TIMEOUT].total_milliseconds))
    cg.add(var.set_diagnostic_mode(config[CONF_DIAGNOSTIC_MODE]))
    cg.add(var.set_listen_only(config[CONF_LISTEN_ONLY]))
    cg.add(var.set_valid_broadcast_timeout(config[CONF_VALID_BROADCAST_TIMEOUT].total_milliseconds))
