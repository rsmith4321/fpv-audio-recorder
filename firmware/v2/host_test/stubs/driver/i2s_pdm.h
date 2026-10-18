#pragma once
#include <cstdint>
#include <cstddef>
typedef void *i2s_chan_handle_t;
typedef int gpio_num_t;
typedef struct { int dummy; } i2s_event_data_t;
typedef bool (*i2s_isr_callback_t)(i2s_chan_handle_t, i2s_event_data_t *, void *);
typedef struct { int dma_desc_num; int dma_frame_num; } i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(a, b) i2s_chan_config_t{}
typedef struct { int x; } i2s_pdm_clk_t;
typedef struct { int x; } i2s_pdm_slot_t;
typedef struct { gpio_num_t clk; gpio_num_t din; struct { bool clk_inv; } invert_flags; } i2s_pdm_gpio_t;
typedef struct { i2s_pdm_clk_t clk_cfg; i2s_pdm_slot_t slot_cfg; i2s_pdm_gpio_t gpio_cfg; } i2s_pdm_rx_config_t;
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(r) i2s_pdm_clk_t{}
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(a, b) i2s_pdm_slot_t{}
typedef struct { i2s_isr_callback_t on_recv; i2s_isr_callback_t on_recv_q_ovf; } i2s_event_callbacks_t;
inline int i2s_new_channel(i2s_chan_config_t *, void *, i2s_chan_handle_t *) { return 0; }
inline int i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t, const i2s_pdm_rx_config_t *) { return 0; }
inline int i2s_channel_register_event_callback(i2s_chan_handle_t, i2s_event_callbacks_t *, void *) { return 0; }
inline int i2s_channel_enable(i2s_chan_handle_t) { return 0; }
inline int i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t) { return -1; }
