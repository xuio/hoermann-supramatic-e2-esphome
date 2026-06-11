#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  HCP2_RX_OK = 0x00,
  HCP2_RX_PARITY_ERROR = 0x01,
  HCP2_RX_FRAMING_ERROR = 0x02,
} hcp2_rx_flags_t;

typedef uint32_t (*hcp2_now_us_fn)(void *user);
typedef void (*hcp2_tx_fn)(void *user, const uint8_t *data, uint8_t len);
typedef void (*hcp2_de_set_fn)(void *user, uint8_t enabled);

typedef struct {
  void *user;
  hcp2_now_us_fn now_us;
  hcp2_tx_fn tx;
  hcp2_de_set_fn de_set;
} hcp2_port_t;

#ifdef __cplusplus
}
#endif
