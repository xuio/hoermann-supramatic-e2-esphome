import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome import pins
from esphome.const import CONF_ID, CONF_RX_PIN, CONF_TX_PIN
from esphome.core import CORE

DEPENDENCIES = []
MULTI_CONF = True

CONF_BUTTON_PRESS_DURATION = "button_press_duration"
CONF_DE_PIN = "de_pin"
CONF_DEVICE_SIGNATURE = "device_signature"
CONF_HCP2BRIDGE_ID = "hcp2bridge_id"
CONF_HP_FALLBACK = "hp_fallback"
CONF_RE_PIN = "re_pin"
CONF_RESPONSE_DELAY = "response_delay"
CONF_SLAVE_ID = "slave_id"
CONF_UART_NUM = "uart_num"

hcp2bridge_ns = cg.esphome_ns.namespace("hcp2bridge")
HCP2Bridge = hcp2bridge_ns.class_("HCP2Bridge", cg.Component)


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


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HCP2Bridge),
            cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_DE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_UART_NUM, default=1): cv.int_range(min=0, max=2),
            cv.Optional(CONF_SLAVE_ID, default=2): cv.int_range(min=1, max=247),
            cv.Optional(CONF_DEVICE_SIGNATURE, default="00000205043010FFA855"): validate_signature,
            cv.Optional(CONF_RESPONSE_DELAY, default="4500us"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_BUTTON_PRESS_DURATION, default="100ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_HP_FALLBACK, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .add_extra(cv.only_on_esp32),
    validate_esp_idf_framework,
    esp32.only_on_variant(supported=esp32.VARIANT_ESP32C6, msg_prefix="hcp2bridge"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    rx_pin = await cg.gpio_pin_expression(config[CONF_RX_PIN])
    tx_pin = await cg.gpio_pin_expression(config[CONF_TX_PIN])
    de_pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])

    cg.add(var.set_rx_pin(rx_pin))
    cg.add(var.set_tx_pin(tx_pin))
    cg.add(var.set_de_pin(de_pin))
    if CONF_RE_PIN in config:
        re_pin = await cg.gpio_pin_expression(config[CONF_RE_PIN])
        cg.add(var.set_re_pin(re_pin))
    cg.add(var.set_uart_num(config[CONF_UART_NUM]))
    cg.add(var.set_slave_id(config[CONF_SLAVE_ID]))
    for index, byte in enumerate(config[CONF_DEVICE_SIGNATURE]):
        cg.add(var.set_signature_byte(index, byte))
    cg.add(var.set_response_delay_us(config[CONF_RESPONSE_DELAY].total_microseconds))
    cg.add(var.set_button_press_us(config[CONF_BUTTON_PRESS_DURATION].total_microseconds))
    cg.add(var.set_hp_fallback(config[CONF_HP_FALLBACK]))
