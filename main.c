#include "hardware/adc.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

// Pio is needed for real time stuff, all Pio has a FIFO queue, and we just write to it. Pio can do bit banging async from processor. Pio has 2 blocks with 4 state machines each
#include "i2s_rx.pio.h"
#include "i2s_tx.pio.h"

#define BCK_PIN 0 // Bit Clock that comes from 1808
#define LRC_PIN 1 // Left Right Clock 1808
#define DAC_DIN_PIN 2 // Data output to 5102, i2s_tx sends through here

#define ADC_DOUT_PIN 3 // Digital output from 1808, receives 24 bit words
#define ADC_SCK_PIN 4 // System clock input for the 1808, hi speed, 1808 derives clocks from it

#define FOOTSWITCH_PIN 5    // bypass toggle, active low
#define EFFECT_BUTTON_PIN 6 // effect select, active low

// Physical pots are wired right-to-left, read_adc_input() flips the ADC value.
// Two ids cause sdk needs it
#define POT_1_PIN 28
#define POT_2_PIN 27
#define POT_3_PIN 26
#define POT_1_ADC_INPUT 2
#define POT_2_ADC_INPUT 1
#define POT_3_ADC_INPUT 0

// LED stuff
#define LED_RED_PIN 13
#define LED_GREEN_PIN 12
#define LED_BLUE_PIN 11
#define LED_PWM_WRAP 255
#define LED_RED_LEVEL 8
#define LED_GREEN_LEVEL 10
#define LED_BLUE_LEVEL 13

// 2 PIO blocks, 4 state machines each. Audio uses one TX sm and two RX sm.
#define DAC_TX_PIO pio0 // block 0 to send
#define DAC_TX_SM 0 // Transmitter uses sm0 from block 0
#define ADC_RX_PIO pio1 // block 1 to receive
#define ADC_LEFT_RX_SM 0 // left uses sm0
#define ADC_RIGHT_RX_SM 1 // right uses sm1


// pico SC (system clock) is 125MHz, we use PWM to generate 1808's. PWM counts from 0 to WRAP, 125MHz/10 = 12.5MHz
// eg 1808 derives audio sample rate from that 12.5Mhz / 256 = 48.828 kHz
#define SCK_PWM_WRAP 9
// How much of the count is high, 5 is 50% hi 50% lo square wave
#define SCK_PWM_LEVEL 5

// Control stuff
#define CONTROL_POLL_MS 2
#define BUTTON_DEBOUNCE_MS 20
#define BUTTON_DEBOUNCE_TICKS (BUTTON_DEBOUNCE_MS / CONTROL_POLL_MS)

#define ADC_MAX 4095
// pOt has a weird deadzone on low values
#define POT_DEADZONE 20
#define SAMPLE_24_MAX 0x007fffff
#define SAMPLE_24_MIN (-0x00800000)
// Equivalent for 1.0 for gain and stuff
#define Q8_UNITY 256
#define Q7_UNITY 128

#define SAMPLE_RATE_HZ 48828
#define DELAY_MAX_MS 500
//How many samples the delay buffer needs for 500 ms
#define DELAY_BUFFER_SAMPLES ((SAMPLE_RATE_HZ * DELAY_MAX_MS) / 1000)
// delay pot quantization
#define DELAY_PRESET_COUNT 16

#define BYPASS_GAIN_MAX_Q8 (2 * Q8_UNITY)
#define TONE_DARKEST_Q8 8
#define TONE_BRIGHTEST_Q8 Q8_UNITY
#define DISTORTION_DRIVE_MAX_Q8 (16 * Q8_UNITY)
#define OVERDRIVE_DRIVE_MAX_Q8 (12 * Q8_UNITY)
#define DISTORTION_CLIP_LEVEL (SAMPLE_24_MAX / 3)
#define OVERDRIVE_CLIP_LEVEL (SAMPLE_24_MAX / 2)
#define DELAY_FEEDBACK_MAX_Q7 (Q7_UNITY / 2)
#define DELAY_LEVEL_MAX_Q7 83

// Pot roles:
// drive effects: Drive / Tone / Level
// delay: Time / Feedback / Level
// bypass: pot 3 is gain

typedef enum {
  EFFECT_DISTORTION = 0,
  EFFECT_OVERDRIVE,
  EFFECT_DELAY,
  EFFECT_COUNT
} effect_mode_t;

typedef struct {
  uint pin;
  bool last_raw_pressed;
  bool debounced_pressed;
  uint stable_ticks;
  void (*on_press)(void);
} button_t;

static void next_effect(void);
static void toggle_bypass(void);

static volatile effect_mode_t current_effect = EFFECT_DISTORTION;
static volatile bool bypass_enabled = true;

static volatile uint16_t pot_1 = 0;
static volatile uint16_t pot_2 = 0;
static volatile uint16_t pot_3 = 0;

static volatile uint32_t delay_samples_setting =
    ((uint32_t)250 * SAMPLE_RATE_HZ) / 1000;
static volatile int32_t delay_feedback_q7 = 0;
static volatile int32_t delay_level_q7 = 0;

static int32_t delay_buffer[DELAY_BUFFER_SAMPLES];
static uint32_t delay_write_index = 0;
static const uint16_t delay_preset_ms[DELAY_PRESET_COUNT] = {
    50,  80,  110, 140, 170, 200, 230, 260,
    290, 320, 350, 380, 410, 440, 470, 500,
};
static int32_t distortion_tone_state = 0;
static int32_t overdrive_tone_state = 0;

static button_t footswitch = {
    .pin = FOOTSWITCH_PIN,
    .on_press = toggle_bypass,
};

static button_t effect_button = {
    .pin = EFFECT_BUTTON_PIN,
    .on_press = next_effect,
};

static void reset_delay(void) {
  for (uint32_t i = 0; i < DELAY_BUFFER_SAMPLES; i++) {
    delay_buffer[i] = 0;
  }

  delay_write_index = 0;
}

static void next_effect(void) {
  effect_mode_t next = current_effect + 1;

  if (next >= EFFECT_COUNT) {
    next = EFFECT_DISTORTION;
  }

  if (next == EFFECT_DELAY) {
    reset_delay();
  }

  current_effect = next;
}

static void toggle_bypass(void) {
  bypass_enabled = !bypass_enabled;

  if (!bypass_enabled && current_effect == EFFECT_DELAY) {
    reset_delay();
  }
}

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

static int32_t clamp_24(int32_t sample) {
  if (sample > SAMPLE_24_MAX) {
    return SAMPLE_24_MAX;
  }

  if (sample < SAMPLE_24_MIN) {
    return SAMPLE_24_MIN;
  }

  return sample;
}

static int32_t mul_q8(int32_t sample, int32_t gain_q8) {
  return clamp_24((int32_t)(((int64_t)sample * gain_q8) >> 8));
}

static int32_t scale_pot_q8(uint16_t pot, int32_t min_q8, int32_t max_q8) {
  return min_q8 + ((int32_t)pot * (max_q8 - min_q8)) / ADC_MAX;
}

static int32_t scale_pot_q7(uint16_t pot, int32_t max_q7) {
  return ((int32_t)pot * max_q7) / ADC_MAX;
}

static uint16_t pot_value(volatile uint16_t *pot) {
  uint16_t value = *pot;

  if (value <= POT_DEADZONE) {
    return 0;
  }

  return value;
}

static int32_t bypass_gain(int32_t sample) {
  uint16_t gain_pot = pot_value(&pot_3);

  // pot 3: 0% to 200% bypass gain.
  int32_t gain_q8 = scale_pot_q8(gain_pot, 0, BYPASS_GAIN_MAX_Q8);

  return mul_q8(sample, gain_q8);
}

static int32_t tone_filter(int32_t sample, uint16_t tone_pot,
                           int32_t *tone_state) {
  int32_t tone_q8 =
      scale_pot_q8(tone_pot, TONE_DARKEST_Q8, TONE_BRIGHTEST_Q8);
  *tone_state += (int32_t)((((int64_t)sample - *tone_state) * tone_q8) >> 8);

  return clamp_24(*tone_state);
}

static int32_t distortion(int32_t sample) {
  uint16_t drive_pot = pot_value(&pot_1);
  uint16_t tone_pot = pot_value(&pot_2);
  uint16_t level_pot = pot_value(&pot_3);

  // pot 1: 1x to 16x input gain.
  int32_t drive_q8 =
      scale_pot_q8(drive_pot, Q8_UNITY, DISTORTION_DRIVE_MAX_Q8);

  // pot 3: 0% to 100% output level.
  int32_t level_q8 = scale_pot_q8(level_pot, 0, Q8_UNITY);

  int32_t driven = mul_q8(sample, drive_q8);
  if (driven > DISTORTION_CLIP_LEVEL) {
    driven = DISTORTION_CLIP_LEVEL;
  } else if (driven < -DISTORTION_CLIP_LEVEL) {
    driven = -DISTORTION_CLIP_LEVEL;
  }

  int32_t toned = tone_filter(driven, tone_pot, &distortion_tone_state);

  return mul_q8(toned, level_q8);
}

static int32_t soft_clip(int32_t sample, int32_t clip) {
  int64_t value = sample;
  int64_t magnitude = value < 0 ? -value : value;
  int64_t clipped = (value * clip) / (magnitude + clip);

  return clamp_24((int32_t)clipped);
}

static int32_t overdrive(int32_t sample) {
  uint16_t drive_pot = pot_value(&pot_1);
  uint16_t tone_pot = pot_value(&pot_2);
  uint16_t level_pot = pot_value(&pot_3);

  // pot 1: 1x to 12x input gain.
  int32_t drive_q8 =
      scale_pot_q8(drive_pot, Q8_UNITY, OVERDRIVE_DRIVE_MAX_Q8);

  // pot 3: 0% to 100% output level.
  int32_t level_q8 = scale_pot_q8(level_pot, 0, Q8_UNITY);

  int32_t driven = mul_q8(sample, drive_q8);
  int32_t clipped = soft_clip(driven, OVERDRIVE_CLIP_LEVEL);
  int32_t toned = tone_filter(clipped, tone_pot, &overdrive_tone_state);

  return mul_q8(toned, level_q8);
}

static int32_t delay(int32_t sample) {
  uint32_t delay_samples = delay_samples_setting;

  if (delay_samples >= DELAY_BUFFER_SAMPLES) {
    delay_samples = DELAY_BUFFER_SAMPLES - 1;
  }

  uint32_t read_index = delay_write_index >= delay_samples
                            ? delay_write_index - delay_samples
                            : delay_write_index + DELAY_BUFFER_SAMPLES -
                                  delay_samples;

  int32_t wet = delay_buffer[read_index];

  int32_t feedback_q7 = delay_feedback_q7;
  int32_t write_sample = sample + ((wet * feedback_q7) >> 7);
  delay_buffer[delay_write_index] = clamp_24(write_sample);

  delay_write_index++;
  if (delay_write_index >= DELAY_BUFFER_SAMPLES) {
    delay_write_index = 0;
  }

  int32_t level_q7 = delay_level_q7;

  return clamp_24(sample + ((wet * level_q7) >> 7));
}

static int32_t apply_effect(int32_t sample) {
  if (bypass_enabled) {
    return bypass_gain(sample);
  }

  effect_mode_t effect = current_effect;

  switch (effect) {
  case EFFECT_DISTORTION:
    return distortion(sample);
  case EFFECT_OVERDRIVE:
    return overdrive(sample);
  case EFFECT_DELAY:
    return delay(sample);
  default:
    return sample;
  }
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

static void init_controls(void) {
  adc_init();
  adc_gpio_init(POT_1_PIN);
  adc_gpio_init(POT_2_PIN);
  adc_gpio_init(POT_3_PIN);

  gpio_init(FOOTSWITCH_PIN);
  gpio_set_dir(FOOTSWITCH_PIN, GPIO_IN);
  gpio_pull_up(FOOTSWITCH_PIN);

  gpio_init(EFFECT_BUTTON_PIN);
  gpio_set_dir(EFFECT_BUTTON_PIN, GPIO_IN);
  gpio_pull_up(EFFECT_BUTTON_PIN);
}

static void init_led_pin(uint pin) {
  gpio_set_function(pin, GPIO_FUNC_PWM);
  uint slice = pwm_gpio_to_slice_num(pin);
  pwm_set_wrap(slice, LED_PWM_WRAP);
  pwm_set_gpio_level(pin, 0);
  pwm_set_enabled(slice, true);
}

static void init_leds(void) {
  init_led_pin(LED_RED_PIN);
  init_led_pin(LED_GREEN_PIN);
  init_led_pin(LED_BLUE_PIN);
}

static void set_leds(uint8_t red, uint8_t green, uint8_t blue) {
  pwm_set_gpio_level(LED_RED_PIN, red);
  pwm_set_gpio_level(LED_GREEN_PIN, green);
  pwm_set_gpio_level(LED_BLUE_PIN, blue);
}

static void update_leds(void) {
  effect_mode_t effect = current_effect;

  if (bypass_enabled) {
    set_leds(0, 0, 0);
    return;
  }

  switch (effect) {
  case EFFECT_DISTORTION:
    set_leds(LED_RED_LEVEL, 0, 0);
    break;
  case EFFECT_OVERDRIVE:
    set_leds(0, LED_GREEN_LEVEL, 0);
    break;
  case EFFECT_DELAY:
    set_leds(0, 0, LED_BLUE_LEVEL);
    break;
  default:
    set_leds(0, 0, 0);
    break;
  }
}

static uint16_t read_adc_input(uint input) {
  adc_select_input(input);
  // Flip so left is min and right is max.
  return ADC_MAX - adc_read();
}

static void read_pots(void) {
  pot_1 = read_adc_input(POT_1_ADC_INPUT);
  pot_2 = read_adc_input(POT_2_ADC_INPUT);
  pot_3 = read_adc_input(POT_3_ADC_INPUT);
}

static void update_delay_controls(void) {
  uint16_t time_pot = pot_value(&pot_1);
  uint16_t feedback_pot = pot_value(&pot_2);
  uint16_t level_pot = pot_value(&pot_3);

  uint32_t preset_index =
      ((uint32_t)time_pot * DELAY_PRESET_COUNT) / (ADC_MAX + 1);
  if (preset_index >= DELAY_PRESET_COUNT) {
    preset_index = DELAY_PRESET_COUNT - 1;
  }

  delay_samples_setting =
      ((uint32_t)delay_preset_ms[preset_index] * SAMPLE_RATE_HZ) / 1000;

  // pot 2: feedback, capped below unity so repeats decay.
  delay_feedback_q7 = scale_pot_q7(feedback_pot, DELAY_FEEDBACK_MAX_Q7);

  // pot 3: echo level, dry stays full.
  delay_level_q7 = scale_pot_q7(level_pot, DELAY_LEVEL_MAX_Q7);
}

static void read_button(button_t *button) {
  bool raw_pressed = !gpio_get(button->pin);

  if (raw_pressed == button->last_raw_pressed) {
    if (button->stable_ticks < BUTTON_DEBOUNCE_TICKS) {
      button->stable_ticks++;
    }
  } else {
    button->stable_ticks = 0;
    button->last_raw_pressed = raw_pressed;
  }

  if (button->stable_ticks == BUTTON_DEBOUNCE_TICKS &&
      raw_pressed != button->debounced_pressed) {
    button->debounced_pressed = raw_pressed;

    if (button->debounced_pressed) {
      button->on_press();
    }
  }
}

static void control_core(void) {
  while (true) {
    read_pots();
    // some mapping thats better here then in audio
    update_delay_controls();
    read_button(&footswitch);
    read_button(&effect_button);
    update_leds();

    sleep_ms(CONTROL_POLL_MS);
  }
}

static void audio_core(void) {
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
    int32_t processed = apply_effect(sample);
    uint32_t dac_word = sample_to_dac_word(processed);

    // we write twice because its mono, one for left and one for right
    pio_sm_put_blocking(DAC_TX_PIO, DAC_TX_SM, dac_word);
    pio_sm_put_blocking(DAC_TX_PIO, DAC_TX_SM, dac_word);
  }
}

int main(void) {
  init_controls();
  init_leds();

  multicore_launch_core1(control_core);
  audio_core();
}
