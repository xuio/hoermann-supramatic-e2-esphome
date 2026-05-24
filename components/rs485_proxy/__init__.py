import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import socket, uart
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network", "uart"]
AUTO_LOAD = ["socket"]

CONF_ALLOW_TX = "allow_tx"
CONF_AUTH_TOKEN = "auth_token"
CONF_GAP_THRESHOLD = "gap_threshold"
CONF_RTS_PIN = "rts_pin"
CONF_SEND_GAPS = "send_gaps"

rs485_proxy_ns = cg.esphome_ns.namespace("rs485_proxy")
RS485Proxy = rs485_proxy_ns.class_("RS485Proxy", cg.Component, uart.UARTDevice)


def validate_auth(config):
    if config[CONF_ALLOW_TX] and not config[CONF_AUTH_TOKEN]:
        raise cv.Invalid("auth_token is required when allow_tx is true")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RS485Proxy),
            cv.Optional(CONF_PORT, default=6638): cv.port,
            cv.Optional(CONF_ALLOW_TX, default=False): cv.boolean,
            cv.Optional(CONF_AUTH_TOKEN, default=""): cv.string_strict,
            cv.Optional(CONF_RTS_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_SEND_GAPS, default=True): cv.boolean,
            cv.Optional(CONF_GAP_THRESHOLD, default="3ms"): cv.positive_time_period_microseconds,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    validate_auth,
    socket.consume_sockets(1, "rs485_proxy"),
    socket.consume_sockets(1, "rs485_proxy", socket.SocketType.TCP_LISTEN),
)

def final_validate(config):
    return uart.final_validate_device_schema(
        "rs485_proxy",
        require_rx=True,
        require_tx=config[CONF_ALLOW_TX],
        baud_rate=19200,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    )(config)


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_allow_tx(config[CONF_ALLOW_TX]))
    cg.add(var.set_auth_token(config[CONF_AUTH_TOKEN]))
    cg.add(var.set_send_gaps(config[CONF_SEND_GAPS]))
    cg.add(var.set_gap_threshold_us(config[CONF_GAP_THRESHOLD].total_microseconds))

    if CONF_RTS_PIN in config:
        rts_pin = await cg.gpio_pin_expression(config[CONF_RTS_PIN])
        cg.add(var.set_rts_pin(rts_pin))
