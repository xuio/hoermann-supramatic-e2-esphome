import subprocess
import sys
import textwrap
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = ROOT / "components"


def run_esphome_config(tmp_path: Path, body: str) -> subprocess.CompletedProcess[str]:
    config = tmp_path / "test.yaml"
    config.write_text(textwrap.dedent(body).format(components=COMPONENTS), encoding="utf-8")
    return subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(config)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def assert_config_valid(result: subprocess.CompletedProcess[str]) -> None:
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Configuration is valid" in result.stdout + result.stderr


def assert_config_invalid(result: subprocess.CompletedProcess[str], needle: str) -> None:
    assert result.returncode != 0, result.stdout + result.stderr
    assert needle in result.stdout + result.stderr


def esp32_realtime_base(hcp2: str) -> str:
    return f"""
esphome:
  name: hcp2-esp32-test

esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source:
      type: local
      path: {{components}}

logger:
  level: INFO

hcp2bridge:
{textwrap.indent(textwrap.dedent(hcp2).strip(), "  ")}
"""


def esp32c6_base(hcp2: str) -> str:
    return f"""
esphome:
  name: hcp2-c6-test

esp32:
  board: esp32-c6-devkitc-1
  variant: ESP32C6
  framework:
    type: esp-idf

external_components:
  - source:
      type: local
      path: {{components}}

logger:
  level: INFO
  hardware_uart: USB_SERIAL_JTAG

hcp2bridge:
{textwrap.indent(textwrap.dedent(hcp2).strip(), "  ")}
"""


def test_esp32_realtime_minimal_config_defaults_pins(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            """
        ),
    )

    assert_config_valid(result)
    output = result.stdout + result.stderr
    assert "backend: esp32_realtime" in output
    assert "esp32_realtime_board_profile: esp32_wroom_no_psram" in output
    assert "number: 16" in output
    assert "number: 17" in output
    assert "number: 18" in output
    assert "number: 19" in output


def test_esp32_realtime_requires_explicit_board_profile(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            """
        ),
    )

    assert_config_invalid(result, "requires esp32_realtime_board_profile")


def test_classic_esp32_requires_realtime_backend(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            rx_pin: GPIO16
            tx_pin: GPIO17
            de_pin: GPIO18
            re_pin: GPIO19
            """
        ),
    )

    assert_config_invalid(result, "backend: esp32c6_lp requires variant: ESP32C6")


def test_esp32_realtime_rejects_ota_by_default(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            """
        )
        + """
wifi:
  ssid: test-network
  password: test-password
  reboot_timeout: 0s

ota:
  - platform: esphome
""",
    )

    assert_config_invalid(result, "backend: esp32_realtime disables OTA by default")


def test_esp32_realtime_rejects_reboot_sources_by_default(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            """
        )
        + """
wifi:
  ssid: test-network
  password: test-password
  reboot_timeout: 15min
""",
    )

    assert_config_invalid(result, "backend: esp32_realtime requires wifi.reboot_timeout: 0s")


def test_esp32_realtime_rejects_uart0_by_default(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            uart_num: 0
            """
        ),
    )

    assert_config_invalid(result, "must not use UART0")


def test_esp32_realtime_rejects_unicore_build(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        """
        esphome:
          name: hcp2-esp32-unicore-test

        esp32:
          board: esp32dev
          framework:
            type: esp-idf
            sdkconfig_options:
              CONFIG_FREERTOS_UNICORE: y

        external_components:
          - source:
              type: local
              path: {components}

        logger:
          level: INFO

        hcp2bridge:
          id: hcp2_test
          backend: esp32_realtime
          esp32_realtime_board_profile: esp32_wroom_no_psram
        """,
    )

    assert_config_invalid(result, "CONFIG_FREERTOS_UNICORE is unsupported")


def test_esp32_realtime_allows_auto_direction_without_de_re_pins(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            rs485_mode: auto_direction
            """
        ),
    )

    assert_config_valid(result)
    output = result.stdout + result.stderr
    assert "rs485_mode: auto_direction" in output
    assert "de_pin:" not in output
    assert "re_pin:" not in output


def test_esp32_realtime_accepts_bench_uart_gate(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32_realtime_base(
            """
            id: hcp2_test
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
            bench_enable_realtime_uart: true
            """
        ),
    )

    assert_config_valid(result)
    assert "bench_enable_realtime_uart: true" in result.stdout + result.stderr


def test_c6_lp_rejects_auto_direction(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            rx_pin: GPIO4
            tx_pin: GPIO5
            de_pin: GPIO0
            re_pin: GPIO1
            rs485_mode: auto_direction
            """
        ),
    )

    assert_config_invalid(result, "esp32c6_lp requires rs485_mode: de_re")


def test_c6_lp_rejects_bench_realtime_uart_gate(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            rx_pin: GPIO4
            tx_pin: GPIO5
            de_pin: GPIO0
            re_pin: GPIO1
            bench_enable_realtime_uart: true
            """
        ),
    )

    assert_config_invalid(result, "bench_enable_realtime_uart is only valid with realtime HCP2 backends")


def test_c6_hp_realtime_defaults_to_lp_wiring(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_realtime
            bench_enable_realtime_uart: true
            """
        ),
    )

    assert_config_valid(result)
    output = result.stdout + result.stderr
    assert "backend: esp32c6_hp_realtime" in output
    assert "number: 4" in output
    assert "number: 5" in output
    assert "number: 0" in output
    assert "number: 1" in output


def test_c6_hp_realtime_requires_bench_uart_gate(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_realtime
            """
        ),
    )

    assert_config_invalid(result, "backend: esp32c6_hp_realtime requires bench_enable_realtime_uart: true")


def test_c6_hp_realtime_rejects_lp_uart(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_realtime
            bench_enable_realtime_uart: true
            uart_num: 2
            """
        ),
    )

    assert_config_invalid(result, "UART2 is the 16-byte LP-UART")


def test_c6_hp_realtime_rejects_ota_by_default(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_realtime
            bench_enable_realtime_uart: true
            """
        )
        + """
wifi:
  ssid: test-network
  password: test-password
  reboot_timeout: 0s

ota:
  - platform: esphome
""",
    )

    assert_config_invalid(result, "backend: esp32c6_hp_realtime disables OTA by default")


def test_c6_hp_asm_dma_probe_defaults_to_lp_wiring(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_asm_dma
            bench_enable_asm_dma_probe: true
            """
        ),
    )

    assert_config_valid(result)
    output = result.stdout + result.stderr
    assert "backend: esp32c6_hp_asm_dma" in output
    assert "bench_enable_asm_dma_probe: true" in output
    assert "number: 4" in output
    assert "number: 5" in output
    assert "number: 0" in output
    assert "number: 1" in output


def test_c6_hp_asm_dma_probe_requires_gate(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_asm_dma
            """
        ),
    )

    assert_config_invalid(result, "backend: esp32c6_hp_asm_dma requires bench_enable_asm_dma_probe: true")


def test_c6_hp_asm_dma_probe_rejects_realtime_uart_gate(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_asm_dma
            bench_enable_asm_dma_probe: true
            bench_enable_realtime_uart: true
            """
        ),
    )

    assert_config_invalid(result, "bench_enable_realtime_uart is only valid with realtime HCP2 backends")


def test_c6_hp_asm_dma_probe_rejects_lp_uart(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_asm_dma
            bench_enable_asm_dma_probe: true
            uart_num: 2
            """
        ),
    )

    assert_config_invalid(result, "UART2 is the 16-byte LP-UART")


def test_c6_hp_asm_dma_probe_rejects_ota_by_default(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_hp_asm_dma
            bench_enable_asm_dma_probe: true
            """
        )
        + """
wifi:
  ssid: test-network
  password: test-password
  reboot_timeout: 0s

ota:
  - platform: esphome
""",
    )

    assert_config_invalid(result, "backend: esp32c6_hp_asm_dma disables OTA by default")


def test_rejects_multiple_hcp2bridge_instances(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        """
        esphome:
          name: hcp2-multi-test

        esp32:
          board: esp32dev
          framework:
            type: esp-idf

        external_components:
          - source:
              type: local
              path: {components}

        logger:
          level: INFO

        hcp2bridge:
          - id: hcp2_a
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
          - id: hcp2_b
            backend: esp32_realtime
            esp32_realtime_board_profile: esp32_wroom_no_psram
        """,
    )

    assert_config_invalid(result, "hcp2bridge currently supports only one instance")


def test_hp_fallback_conflict_is_rejected(tmp_path: Path) -> None:
    result = run_esphome_config(
        tmp_path,
        esp32c6_base(
            """
            id: hcp2_test
            backend: esp32c6_lp
            hp_fallback: true
            rx_pin: GPIO4
            tx_pin: GPIO5
            de_pin: GPIO0
            re_pin: GPIO1
            """
        ),
    )

    assert_config_invalid(result, "hp_fallback: true conflicts with backend")
