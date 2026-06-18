#pragma once

#define HCP2_C6_ASM_DMA_PROBE_MAGIC 0x48324336u
#define HCP2_C6_ASM_DMA_PROBE_VERSION 1u

#define HCP2_C6_ASM_DMA_PROBE_OFF_MAGIC 0
#define HCP2_C6_ASM_DMA_PROBE_OFF_VERSION 4
#define HCP2_C6_ASM_DMA_PROBE_OFF_FLAGS 8
#define HCP2_C6_ASM_DMA_PROBE_OFF_IRQ_COUNT 12
#define HCP2_C6_ASM_DMA_PROBE_OFF_RX_IRQ_COUNT 16
#define HCP2_C6_ASM_DMA_PROBE_OFF_OTHER_IRQ_COUNT 20
#define HCP2_C6_ASM_DMA_PROBE_OFF_DROPPED_BYTES 24
#define HCP2_C6_ASM_DMA_PROBE_OFF_DRAINED_BYTES 28
#define HCP2_C6_ASM_DMA_PROBE_OFF_LAST_MCYCLE 32
#define HCP2_C6_ASM_DMA_PROBE_OFF_MAX_IRQ_GAP_CYCLES 36
#define HCP2_C6_ASM_DMA_PROBE_OFF_LAST_STATUS 40
#define HCP2_C6_ASM_DMA_PROBE_OFF_LAST_FIFO_COUNT 44
#define HCP2_C6_ASM_DMA_PROBE_OFF_MAX_FIFO_COUNT 48
#define HCP2_C6_ASM_DMA_PROBE_OFF_ERROR_STATUS_COUNT 52
#define HCP2_C6_ASM_DMA_PROBE_OFF_ZERO_STATUS_COUNT 56
#define HCP2_C6_ASM_DMA_PROBE_OFF_UART_INT_ST_ADDR 60
#define HCP2_C6_ASM_DMA_PROBE_OFF_UART_INT_CLR_ADDR 64
#define HCP2_C6_ASM_DMA_PROBE_OFF_UART_STATUS_ADDR 68
#define HCP2_C6_ASM_DMA_PROBE_OFF_UART_FIFO_ADDR 72
#define HCP2_C6_ASM_DMA_PROBE_OFF_GPIO_SET_ADDR 76
#define HCP2_C6_ASM_DMA_PROBE_OFF_GPIO_CLEAR_ADDR 80
#define HCP2_C6_ASM_DMA_PROBE_OFF_PROBE_GPIO_MASK 84
#define HCP2_C6_ASM_DMA_PROBE_OFF_RX_INTR_MASK 88
#define HCP2_C6_ASM_DMA_PROBE_OFF_CLEAR_INTR_MASK 92
#define HCP2_C6_ASM_DMA_PROBE_OFF_STATUS_FIFO_MASK 96
#define HCP2_C6_ASM_DMA_PROBE_OFF_ERROR_INTR_MASK 100
#define HCP2_C6_ASM_DMA_PROBE_OFF_UHCI_BASE_ADDR 104
#define HCP2_C6_ASM_DMA_PROBE_OFF_GDMA_BASE_ADDR 108
#define HCP2_C6_ASM_DMA_PROBE_OFF_RX_DESC_ADDR 112
#define HCP2_C6_ASM_DMA_PROBE_OFF_TX_DESC_ADDR 116
#define HCP2_C6_ASM_DMA_PROBE_SIZE 160

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct hcp2_c6_asm_dma_probe_state_s {
  volatile uint32_t magic;
  volatile uint32_t version;
  volatile uint32_t flags;
  volatile uint32_t irq_count;
  volatile uint32_t rx_irq_count;
  volatile uint32_t other_irq_count;
  volatile uint32_t dropped_bytes;
  volatile uint32_t drained_bytes;
  volatile uint32_t last_mcycle;
  volatile uint32_t max_irq_gap_cycles;
  volatile uint32_t last_status;
  volatile uint32_t last_fifo_count;
  volatile uint32_t max_fifo_count;
  volatile uint32_t error_status_count;
  volatile uint32_t zero_status_count;
  volatile uint32_t uart_int_st_addr;
  volatile uint32_t uart_int_clr_addr;
  volatile uint32_t uart_status_addr;
  volatile uint32_t uart_fifo_addr;
  volatile uint32_t gpio_set_addr;
  volatile uint32_t gpio_clear_addr;
  volatile uint32_t probe_gpio_mask;
  volatile uint32_t rx_intr_mask;
  volatile uint32_t clear_intr_mask;
  volatile uint32_t status_fifo_mask;
  volatile uint32_t error_intr_mask;
  volatile uint32_t uhci_base_addr;
  volatile uint32_t gdma_base_addr;
  volatile uint32_t rx_desc_addr;
  volatile uint32_t tx_desc_addr;
  volatile uint32_t reserved[10];
} hcp2_c6_asm_dma_probe_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void hcp2_c6_asm_dma_probe_isr(void *arg);

#ifdef __cplusplus
}
#endif
#endif
