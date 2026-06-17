#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

// Pio is needed for real time stuff, all Pio has a FIFO queue, and we just write to it. Pio can do bit banging async from processor. Pio has 2 blocks with 4 state machines each
#include "i2s_rx.pio.h"
#include "i2s_tx.pio.h"

#define BCK_PIN 0 // Bit Clock that comes from 1808
#define LRC_PIN 1 // Left Right Clock 1808
#define DAC_DIN_PIN 2 // Data output to 5102, i2s_tx sends through here

#define ADC_DOUT_PIN 3 // Digital output from 1808, receives 24 bit words
#define ADC_SCK_PIN 4 // System clock input for the 1808, hi speed, 1808 derives clocks from it

// 2 blocks, 4 state machines (sm), each sm is a tiny executor
#define DAC_TX_PIO pio0 // block 0 to send
#define DAC_TX_SM 0 // Transmitter uses sm0 from block 0
//
#define ADC_RX_PIO pio1 // block 1 to receive
#define ADC_LEFT_RX_SM 0 // left uses sm0
#define ADC_RIGHT_RX_SM 1 // right uses sm1
// pretty simple, waits for phase change in LRC, each waits for a specific phase


// pico SC (system clock) is 125MHz, we use PWM to generate 1808's. PWM counts from 0 to WRAP, 125MHz/10 = 12.5MHz
// eg 1808 derives audio sample rate from that 12.5Mhz / 256 = 48.828 kHz
#define SCK_PWM_WRAP 9
// How much of the count is high, 5 is 50% hi 50% lo square wave
#define SCK_PWM_LEVEL 5

//fix sign, 24 bit has 24th bit as sign bit, 1XXXX... is negative, 0XXX... is 0 or positive. we extend the sign if 24 is negative
static int32_t sign_extend_24(uint32_t value) {
  if (value & 0x00800000u) {
    value |= 0xff000000u;
  }

  return (int32_t)value;
}

// PIO pads the word [24 bits][8 padding], fixes padding
static int32_t adc_word_to_sample(uint32_t word) {
  return sign_extend_24(word >> 8);
}

// does opposite of adc_word.
static uint32_t sample_to_dac_word(int32_t sample) {
  return ((uint32_t)sample) << 8;
}

// INIT FUNCTIONS
// Configure ADC_SCK_PIN to output a clock
static void init_pcm1808_sck(void) {
  // mode to pwm
  gpio_set_function(ADC_SCK_PIN, GPIO_FUNC_PWM);

  // what PWM hadware slice controls ADC_SCK_PIN?
  uint slice = pwm_gpio_to_slice_num(ADC_SCK_PIN);

  // Divider is 1.0, full clock speed (125Mhz)
  pwm_set_clkdiv(slice, 1.0f);

  // Sets wrap, high ratio, and starts
  pwm_set_wrap(slice, SCK_PWM_WRAP);
  pwm_set_gpio_level(ADC_SCK_PIN, SCK_PWM_LEVEL);
  pwm_set_enabled(slice, true);
}

//Configures one PIO sm, offset is if its left or right (where it was loaded in memory)
static void init_i2s_rx_sm(PIO pio, uint sm, uint offset) {

  // Creates default config for a sm
  pio_sm_config config = i2s_rx_left_program_get_default_config(offset);
  sm_config_set_in_pins(&config, ADC_DOUT_PIN);

  //Shifts Left, once 32 shifted in, PIO autopushes the complete word to the RX FIFO
  //config, shift_right, autopush, push_thresh
  sm_config_set_in_shift(&config, false, true, 32);
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);

  pio_gpio_init(pio, ADC_DOUT_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm, ADC_DOUT_PIN, 1, false);

  pio_sm_init(pio, sm, offset, &config);
  pio_sm_set_enabled(pio, sm, true);
  //set out pins : says to read that pin
  //consecutive pindirs : set pin as input
}

// setup adc receive
static void init_i2s_rx(void) {
  //adds memory to the 2 pios
  uint left_offset = pio_add_program(ADC_RX_PIO, &i2s_rx_left_program);
  uint right_offset = pio_add_program(ADC_RX_PIO, &i2s_rx_right_program);

  //INIt bit clock, set as input, since they come from 1808
  gpio_init(BCK_PIN);
  gpio_set_dir(BCK_PIN, GPIO_IN);
  gpio_init(LRC_PIN);
  gpio_set_dir(LRC_PIN, GPIO_IN);

  // init the machines
  init_i2s_rx_sm(ADC_RX_PIO, ADC_LEFT_RX_SM, left_offset);
  init_i2s_rx_sm(ADC_RX_PIO, ADC_RIGHT_RX_SM, right_offset);
}


static void init_i2s_tx(void) {
  // add program to memory for the pio DAC_TX_PIO
  uint offset = pio_add_program(DAC_TX_PIO, &i2s_tx_stereo_program);
  pio_sm_config config = i2s_tx_stereo_program_get_default_config(offset);

  // what pin to write
  sm_config_set_out_pins(&config, DAC_DIN_PIN, 1);
  // shift left (most significant first), pio must run pull block
  // to get new 32 word.
  sm_config_set_out_shift(&config, false, false, 32);
  // Since it only txes, just tx
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

  //init pios
  pio_gpio_init(DAC_TX_PIO, DAC_DIN_PIN);
  pio_sm_set_consecutive_pindirs(DAC_TX_PIO, DAC_TX_SM, DAC_DIN_PIN, 1, true);

  pio_sm_init(DAC_TX_PIO, DAC_TX_SM, offset, &config);
  pio_sm_set_enabled(DAC_TX_PIO, DAC_TX_SM, true);
}

int main(void) {
  // start SCK first so the PCM1808 can begin producing BCK/LRC,
  // then start the RX/TX state machines that wait on those clocks.
  init_pcm1808_sck();
  sleep_ms(10);
  init_i2s_rx();
  init_i2s_tx();

  while (true) {
    // Read one stereo frame from the PCM1808. Its mono, so we only read left so the right word is drained for synchronization but not sent to the DAC.
    uint32_t left = pio_sm_get_blocking(ADC_RX_PIO, ADC_LEFT_RX_SM);
    pio_sm_get_blocking(ADC_RX_PIO, ADC_RIGHT_RX_SM);

    int32_t sample = adc_word_to_sample(left);
    uint32_t dac_word = sample_to_dac_word(sample);

    // we write twice because its mono, one for left and one for right
    pio_sm_put_blocking(DAC_TX_PIO, DAC_TX_SM, dac_word);
    pio_sm_put_blocking(DAC_TX_PIO, DAC_TX_SM, dac_word);
  }
}
