import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, socket, uart
from ..uapbridge import to_code_base, CONFIG_SCHEMA_BASE, UAPBridge
from esphome.const import CONF_ID

AUTO_LOAD = ["uapbridge", "socket", "watchdog"]
MULTI_CONF = True

CONF_HTTP_DEBUG_GAP_THRESHOLD = "http_debug_gap_threshold"
CONF_HTTP_DEBUG_HISTORY_SIZE = "http_debug_history_size"
CONF_HTTP_DEBUG_PORT = "http_debug_port"
CONF_HTTP_DEBUG_SEND_GAPS = "http_debug_send_gaps"
CONF_PERSISTENT_LOG = "persistent_log"

# Create UAPBridge_esp namespace
uapbridge_esp_ns = cg.esphome_ns.namespace("uapbridge_esp")
UAPBridge_esp = uapbridge_esp_ns.class_("UAPBridge_esp", UAPBridge)

CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA_BASE.extend(
        {
            cv.GenerateID(): cv.declare_id(UAPBridge_esp),
            cv.Optional(CONF_HTTP_DEBUG_PORT, default=0): cv.int_range(min=0, max=65535),
            cv.Optional(CONF_HTTP_DEBUG_SEND_GAPS, default=True): cv.boolean,
            cv.Optional(CONF_HTTP_DEBUG_GAP_THRESHOLD, default="3ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_HTTP_DEBUG_HISTORY_SIZE, default=200): cv.int_range(min=0, max=500),
            cv.Optional(CONF_PERSISTENT_LOG, default=False): cv.boolean,
        }
    ).add_extra(cv.only_on_esp32),
    socket.consume_sockets(2, "uapbridge_esp_http_debug"),
    socket.consume_sockets(1, "uapbridge_esp_http_debug", socket.SocketType.TCP_LISTEN),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "uapbridge_uart",
    require_tx=True,
    require_rx=True,
    baud_rate=19200
)

async def to_code(config):
    esp32.include_builtin_idf_component("spiffs")
    esp32.add_partition("hcp_logs", "data", "spiffs", 4 * 1024 * 1024)
    var = cg.new_Pvariable(config[CONF_ID])
    await to_code_base(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_http_debug_port(config[CONF_HTTP_DEBUG_PORT]))
    cg.add(var.set_http_debug_send_gaps(config[CONF_HTTP_DEBUG_SEND_GAPS]))
    cg.add(var.set_http_debug_gap_threshold_us(config[CONF_HTTP_DEBUG_GAP_THRESHOLD].total_microseconds))
    cg.add(var.set_http_debug_history_size(config[CONF_HTTP_DEBUG_HISTORY_SIZE]))
    cg.add(var.set_persistent_log_enabled(config[CONF_PERSISTENT_LOG]))
