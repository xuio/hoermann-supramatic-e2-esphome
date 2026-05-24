import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import socket, uart
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network", "uart"]
AUTO_LOAD = ["socket"]

CONF_GAP_THRESHOLD = "gap_threshold"
CONF_HISTORY_SIZE = "history_size"
CONF_SEND_GAPS = "send_gaps"

rs485_http_monitor_ns = cg.esphome_ns.namespace("rs485_http_monitor")
RS485HTTPMonitor = rs485_http_monitor_ns.class_("RS485HTTPMonitor", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RS485HTTPMonitor),
            cv.Optional(CONF_PORT, default=8080): cv.port,
            cv.Optional(CONF_SEND_GAPS, default=True): cv.boolean,
            cv.Optional(CONF_GAP_THRESHOLD, default="3ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_HISTORY_SIZE, default=120): cv.int_range(min=0, max=500),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    socket.consume_sockets(2, "rs485_http_monitor"),
    socket.consume_sockets(1, "rs485_http_monitor", socket.SocketType.TCP_LISTEN),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "rs485_http_monitor",
    require_tx=False,
    require_rx=True,
    baud_rate=19200,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_send_gaps(config[CONF_SEND_GAPS]))
    cg.add(var.set_gap_threshold_us(config[CONF_GAP_THRESHOLD].total_microseconds))
    cg.add(var.set_history_size(config[CONF_HISTORY_SIZE]))
