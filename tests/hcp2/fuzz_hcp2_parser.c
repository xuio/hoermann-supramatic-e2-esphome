#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcp2_crc.h"
#include "hcp2_engine.h"

typedef struct {
  uint32_t now_us;
  uint32_t tx_count;
} fuzz_port_t;

static uint32_t fuzz_now_us(void *user) {
  return ((fuzz_port_t *) user)->now_us;
}

static void fuzz_tx(void *user, const uint8_t *data, uint8_t len) {
  (void) data;
  (void) len;
  ((fuzz_port_t *) user)->tx_count++;
}

static uint32_t rng_next(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static uint8_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint8_t) (c - '0');
  }
  if (c >= 'A' && c <= 'F') {
    return (uint8_t) (10 + c - 'A');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint8_t) (10 + c - 'a');
  }
  assert(0 && "invalid hex digit");
  return 0;
}

static uint8_t parse_hex(const char *hex, uint8_t *out) {
  uint8_t len = 0;
  while (*hex != '\0') {
    out[len++] = (uint8_t) ((hex_value(hex[0]) << 4) | hex_value(hex[1]));
    hex += 2;
  }
  return len;
}

static void feed(hcp2_engine_t *engine, const uint8_t *data, uint8_t len) {
  uint8_t i;
  for (i = 0; i < len; i++) {
    hcp2_engine_rx_byte(engine, data[i], HCP2_RX_OK);
  }
}

int main(int argc, char **argv) {
  static const char *valid_hex[] = {
      "02179CB900059C41000306000200000102F835",
      "02179CB900089C410002043E030000EBCC",
      "02179CB900029C4100020402041700798D",
      "00109D310009123000C8C8206000000000000000140001000055A1",
  };
  const uint32_t iterations = argc > 1 ? (uint32_t) strtoul(argv[1], NULL, 10) : 10000u;
  const char *report_path = argc > 2 ? argv[2] : NULL;
  uint32_t seed = 0xC0DEC0DEu;
  uint32_t bad_crc_cases = 0u;
  uint32_t bad_crc_responses = 0u;
  uint32_t i;

  for (i = 0; i < iterations; i++) {
    fuzz_port_t fuzz_port;
    hcp2_port_t port;
    hcp2_engine_config_t config;
    hcp2_engine_t engine;
    uint8_t frame[HCP2_MAX_FRAME_LEN];
    uint8_t len;
    uint8_t mutations;
    uint8_t m;
    uint32_t tx_before;
    uint8_t crc_ok;

    memset(&fuzz_port, 0, sizeof(fuzz_port));
    memset(&port, 0, sizeof(port));
    port.user = &fuzz_port;
    port.now_us = fuzz_now_us;
    port.tx = fuzz_tx;
    hcp2_engine_config_default(&config);
    config.response_delay_us = 0;
    hcp2_engine_init(&engine, &port, &config);

    len = parse_hex(valid_hex[rng_next(&seed) % (uint32_t) (sizeof(valid_hex) / sizeof(valid_hex[0]))], frame);
    mutations = (uint8_t) (1u + (rng_next(&seed) % 4u));
    for (m = 0; m < mutations; m++) {
      const uint8_t index = (uint8_t) (rng_next(&seed) % len);
      frame[index] ^= (uint8_t) (1u << (rng_next(&seed) % 8u));
    }

    if ((rng_next(&seed) & 0x07u) == 0u && len > 3u) {
      len = (uint8_t) (len - (1u + (rng_next(&seed) % 3u)));
    }
    if ((rng_next(&seed) & 0x07u) == 0u) {
      hcp2_engine_rx_byte(&engine, (uint8_t) rng_next(&seed), HCP2_RX_OK);
    }

    crc_ok = hcp2_crc16_check(frame, len);
    tx_before = fuzz_port.tx_count;
    feed(&engine, frame, len);
    hcp2_engine_poll(&engine);
    if (!crc_ok) {
      bad_crc_cases++;
      if (fuzz_port.tx_count != tx_before) {
        bad_crc_responses++;
      }
    }
  }

  if (report_path != NULL) {
    FILE *report = fopen(report_path, "w");
    assert(report != NULL);
    fprintf(report,
            "{\n"
            "  \"iterations\": %lu,\n"
            "  \"bad_crc_cases\": %lu,\n"
            "  \"bad_crc_responses\": %lu,\n"
            "  \"seed\": \"0xC0DEC0DE\"\n"
            "}\n",
            (unsigned long) iterations, (unsigned long) bad_crc_cases, (unsigned long) bad_crc_responses);
    fclose(report);
  }

  assert(bad_crc_responses == 0u);
  printf("hcp2 parser fuzz ok iterations=%lu bad_crc_cases=%lu\n", (unsigned long) iterations,
         (unsigned long) bad_crc_cases);
  return 0;
}
