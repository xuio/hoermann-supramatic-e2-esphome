from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMMON_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge.cpp"
C6_LP_BACKEND_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge_backend_lp.cpp"
ESP32_REALTIME_BACKEND_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge_backend_esp32_realtime.cpp"
ESP32C6_ASM_DMA_BACKEND_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge_backend_esp32c6_asm_dma.cpp"
ESP32C6_ASM_DMA_HEADER = ROOT / "components" / "hcp2bridge" / "hcp2_c6_asm_dma_probe.h"
HTTP_DEBUG_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge_http_debug.cpp"


def test_c6_lp_loader_dependencies_are_isolated() -> None:
    common = COMMON_SOURCE.read_text()
    backend = C6_LP_BACKEND_SOURCE.read_text()

    forbidden_in_common = (
        '#include "hcp2_lp_blob.h"',
        '#include "lp_core_uart.h"',
        '#include "ulp_lp_core.h"',
        "HCP2_LP_MAILBOX_ADDR",
        "lp_core_uart_init",
        "ulp_lp_core_load_binary",
        "ulp_lp_core_run",
        "ulp_lp_core_stop",
    )
    for token in forbidden_in_common:
        assert token not in common

    required_in_backend = (
        '#include "hcp2_lp_blob.h"',
        '#include "lp_core_uart.h"',
        '#include "ulp_lp_core.h"',
        "HCP2_LP_MAILBOX_ADDR",
        "lp_core_uart_init",
        "ulp_lp_core_load_binary",
        "ulp_lp_core_run",
    )
    for token in required_in_backend:
        assert token in backend


def test_common_component_routes_through_backend_capabilities() -> None:
    common = COMMON_SOURCE.read_text()

    assert "switch (this->backend_kind_)" in common
    assert "this->backend_survives_hp_restart_() && this->is_continuity_healthy()" in common
    assert "this->backend_uses_mailbox_()" in common


def test_http_health_verdict_tracks_continuity_not_ota_restart_safety() -> None:
    source = HTTP_DEBUG_SOURCE.read_text()
    health = source.split("std::string HCP2Bridge::http_debug_health_json_()", 1)[1].split(
        "std::string HCP2Bridge::http_debug_stats_json_()", 1
    )[0]

    assert "const bool continuity_ok = first_reason;" in health
    assert "json += continuity_ok ? \"ok\" : \"fail\";" in health
    assert "continuity_verdict" in health
    assert "safe_for_ota_restart" in health


def test_esp32_realtime_backend_defaults_to_mailbox_prototype() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    assert "esp32_realtime_phase_d_ready" in backend
    assert "esp32_realtime_mailbox_" in backend
    assert "esp32_realtime_task_loop_" in backend
    assert "backend_ready_ = true" in backend
    assert "this->bench_enable_realtime_uart_ && !this->setup_esp32_realtime_uart_timer_()" in backend

    default_setup = backend.split("bool HCP2Bridge::setup_esp32_realtime_()", 1)[1].split(
        "bool HCP2Bridge::setup_esp32_realtime_uart_timer_()", 1
    )[0]
    forbidden_in_default = (
        "uart_param_config",
        "uart_set_pin",
        "esp_intr_alloc_intrstatus",
        "gptimer_new_timer",
        "gpio_set_level",
        "pin_mode",
        "digital_write",
    )
    for token in forbidden_in_default:
        assert token not in default_setup


def test_esp32_realtime_backend_has_bench_gated_uart_timer_path() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    assert "bench_enable_realtime_uart_" in backend
    assert "setup_esp32_realtime_uart_timer_" in backend
    assert "esp32_realtime_phase_e_uart_ready" in backend
    assert "esp32c6_hp_realtime_uart_ready" in backend
    assert "ulp_lp_core_stop()" in backend

    required = (
        "uart_param_config",
        "uart_set_pin",
        "esp_intr_alloc_intrstatus",
        "gptimer_new_timer",
        "gptimer_set_alarm_action",
        "uart_ll_write_txfifo",
        "UART_INTR_TX_DONE",
        "gpio_set_level",
    )
    for token in required:
        assert token in backend


def test_esp32_realtime_tx_done_completes_through_tail_fallback_helper() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    assert "esp32_realtime_tx_tail_due_us_" in backend
    assert "HCP2BRIDGE_REALTIME_BITS_PER_UART_BYTE" in backend
    assert "esp32_realtime_finish_tx_from_isr_" in backend
    assert "esp32_realtime_schedule_alarm_from_isr_(self, self->esp32_realtime_tx_tail_due_us_)" in backend
    assert "uart_ll_is_tx_idle(self->esp32_realtime_uart_hw_)" in backend
    assert "HCP2BRIDGE_REALTIME_RXFIFO_FULL_THR = 1u" in backend
    assert "uart_ll_set_rxfifo_full_thr(this->esp32_realtime_uart_hw_, HCP2BRIDGE_REALTIME_RXFIFO_FULL_THR)" in backend
    assert "ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3" in backend

    uart_tx_done_block = backend.split("if ((status & UART_INTR_TX_DONE) != 0u)", 1)[1].split(
        "uart_ll_clr_intsts_mask", 1
    )[0]
    assert "hcp2_engine_mark_tx_done" not in uart_tx_done_block
    assert "esp32_realtime_finish_tx_from_isr_(self, now, false)" in uart_tx_done_block


def test_esp32c6_asm_dma_probe_backend_is_hard_gated_and_high_priority() -> None:
    init_py = (ROOT / "components" / "hcp2bridge" / "__init__.py").read_text()
    common = COMMON_SOURCE.read_text()
    backend = ESP32C6_ASM_DMA_BACKEND_SOURCE.read_text()
    header = ESP32C6_ASM_DMA_HEADER.read_text()

    assert 'BACKEND_ESP32C6_HP_ASM_DMA = "esp32c6_hp_asm_dma"' in init_py
    assert 'CONF_BENCH_ENABLE_ASM_DMA_PROBE = "bench_enable_asm_dma_probe"' in init_py
    assert "requires bench_enable_asm_dma_probe: true" in init_py
    assert "-DHCP2_ESP32C6_ASM_DMA_EXPERIMENT=1" in init_py

    assert "HCP2BackendKind::ESP32C6_HP_ASM_DMA" in common
    assert "bench_enable_asm_dma_probe_" in backend
    assert "ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL4" in backend
    assert "esp_intr_alloc_intrstatus" in backend
    assert "ulp_lp_core_stop()" in backend
    assert "DR_REG_UHCI0_BASE" in backend
    assert "DR_REG_GDMA_BASE" in backend
    assert "offsetof(hcp2_c6_asm_dma_probe_state_t" in backend
    assert "__attribute__((noinline, used)) hcp2_c6_asm_dma_probe_isr" in backend
    assert "mcycle" in backend
    assert "HCP2_C6_ASM_DMA_PROBE_OFF_DRAINED_BYTES" in backend

    assert "HCP2_C6_ASM_DMA_PROBE_OFF_UART_FIFO_ADDR" in header
    assert "HCP2_C6_ASM_DMA_PROBE_OFF_GDMA_BASE_ADDR" in header
    assert "hcp2_c6_asm_dma_probe_state_t" in header


def test_esp32c6_uhci_rx_isr_only_queues_dma_buffer_metadata() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    rx_cb = backend.split("bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_uhci_rx_cb_", 1)[1].split(
        "bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_uhci_tx_cb_", 1
    )[0]
    task_loop = backend.split("void HCP2Bridge::esp32c6_realtime_uhci_task_loop_()", 1)[1].split(
        "void HCP2Bridge::esp32_realtime_maintenance_task_loop_()", 1
    )[0]

    assert "xQueueSendFromISR" in rx_cb
    assert "event.buffer_index" in rx_cb
    assert "esp32c6_realtime_uhci_rx_bufs_[i]" in rx_cb
    assert "hcp2_engine_rx_byte" not in rx_cb
    assert "event.data" not in rx_cb

    assert "hcp2_engine_rx_byte" in task_loop
    assert "esp32c6_realtime_uhci_rx_bufs_[event.buffer_index]" in task_loop
    assert "esp32c6_realtime_uhci_rx_buf_busy_[event.buffer_index] = false" in task_loop
    assert "hcp2_responder_runtime_publish_state" not in task_loop
    assert "drain_lp_protocol_event_" not in task_loop


def test_esp32c6_uhci_uses_maintenance_task_for_debug_publishing() -> None:
    header = (ROOT / "components" / "hcp2bridge" / "hcp2bridge.h").read_text()
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    assert "esp32_realtime_maintenance_task_handle_" in header
    assert "esp32_realtime_maintenance_task_loop_" in header
    assert "start_esp32_realtime_maintenance_task_" in backend
    assert '"hcp2_rt_maint"' in backend
    assert "use_c6_uhci ? (configMAX_PRIORITIES - 1) : 5" in backend
    assert "HCP2BRIDGE_REALTIME_MAINT_TASK_STACK_BYTES" in backend

    maintenance_loop = backend.split("void HCP2Bridge::esp32_realtime_maintenance_task_loop_()", 1)[1].split(
        "void IRAM_ATTR HCP2Bridge::esp32_realtime_uart_isr_", 1
    )[0]
    assert "hcp2_responder_runtime_publish_state" in maintenance_loop
    assert "hcp2_responder_runtime_publish_counters" in maintenance_loop
    assert "drain_lp_protocol_event_" in maintenance_loop
    assert "drain_lp_trace_" in maintenance_loop


def test_esp32c6_uhci_tx_completion_wakes_realtime_task() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    tx_cb = backend.split("bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_uhci_tx_cb_", 1)[1].split(
        "void HCP2Bridge::esp32c6_realtime_uhci_wait_until_", 1
    )[0]
    send_response = backend.split("void HCP2Bridge::esp32c6_realtime_uhci_send_response_", 1)[1].split(
        "void HCP2Bridge::esp32c6_realtime_uhci_service_pending_tx_", 1
    )[0]

    assert "vTaskNotifyGiveFromISR" in tx_cb
    assert "esp32_realtime_task_handle_" in tx_cb
    assert "ulTaskNotifyTake(pdTRUE, 0)" in send_response
    assert "ulTaskNotifyTake(pdTRUE, 1)" in send_response


def test_esp32c6_uhci_response_wait_uses_gptimer_not_tick_delay() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    assert "HCP2BRIDGE_C6_UHCI_RX_IDLE_BITS = 16u" in backend
    assert "HCP2BRIDGE_C6_UHCI_TX_TIMER_GUARD_US" in backend
    assert "esp32c6_realtime_due_timer_alarm_" in backend
    assert "xSemaphoreGiveFromISR" in backend
    assert "xSemaphoreTake(this->esp32c6_realtime_due_sem_" in backend

    wait_until = backend.split("void HCP2Bridge::esp32c6_realtime_uhci_wait_until_", 1)[1].split(
        "void HCP2Bridge::esp32c6_realtime_uhci_send_response_", 1
    )[0]
    assert "gptimer_set_alarm_action" in wait_until
    assert "vTaskDelay(1)" not in wait_until


def test_esp32c6_uhci_de_brackets_re_disable() -> None:
    backend = ESP32_REALTIME_BACKEND_SOURCE.read_text()

    send_response = backend.split("void HCP2Bridge::esp32c6_realtime_uhci_send_response_", 1)[1].split(
        "void HCP2Bridge::esp32c6_realtime_uhci_service_pending_tx_", 1
    )[0]
    set_de_from_isr = backend.split("void IRAM_ATTR HCP2Bridge::esp32_realtime_set_de_from_isr_", 1)[1].split(
        "bool IRAM_ATTR HCP2Bridge::esp32_realtime_schedule_alarm_from_isr_", 1
    )[0]

    assert send_response.find("gpio_set_level(this->esp32_realtime_de_gpio_, 1)") < send_response.find(
        "gpio_set_level(this->esp32_realtime_re_gpio_, 1)"
    )
    assert send_response.find("gpio_set_level(this->esp32_realtime_re_gpio_, 0)") < send_response.find(
        "gpio_set_level(this->esp32_realtime_de_gpio_, 0)"
    )
    assert set_de_from_isr.find("gpio_set_level(self->esp32_realtime_de_gpio_, 1)") < set_de_from_isr.find(
        "gpio_set_level(self->esp32_realtime_re_gpio_, 1)"
    )
    assert set_de_from_isr.find("gpio_set_level(self->esp32_realtime_re_gpio_, 0)") < set_de_from_isr.find(
        "gpio_set_level(self->esp32_realtime_de_gpio_, 0)"
    )
