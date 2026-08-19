#include "rssi_apis.h"
#include "config_gpio.h"
#include "i2c.h"          // RGB LED is driven over I2C (PM5 only; armsrc is on the include path)
#include "ticks_apis.h"   // GetTickCount() for throttling LED updates

uint16_t g_adc_vref_value;

// ----------------------------------------------------------------------------
// PM5 antenna RGB "field meter"
//
// When enabled (off by default), every field-strength measurement made through
// AdcRssiAvgToMilliVolt() is mapped to a colour on the I2C RGB LED controller,
// turning the antenna LED into a live coupling meter ("getting warmer" = closer
// to the tag). This lives entirely in the AT32 HAL, so no other platform is
// affected. Kept off by default so normal operation never touches the I2C bus.
//
// The RGB controller (I2C 7-bit addr 0x48) register map, per the QC/bring-up code:
//   0x02 = index (which LED)   0x01 = count (# LEDs, must be set)
//   0x03 = data (3 bytes RGB888 per LED)
// ----------------------------------------------------------------------------
#define RGB_METER_I2C_ADDR   (0x48 << 1) // 8-bit form expected by the I2C_* API
#define RGB_METER_REG_COUNT  0x01
#define RGB_METER_REG_INDEX  0x02
#define RGB_METER_REG_DATA   0x03
#define RGB_METER_PERIOD_MS  60          // min interval between LED updates (throttle)

static bool g_rgb_meter_on = false;
static bool g_rgb_meter_i2c_ready = false;
static uint32_t g_rgb_meter_last_ms = 0;

static void rgb_meter_write(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t rgb[3] = { r, g, b };
    I2C_WriteByte(0, RGB_METER_REG_INDEX, RGB_METER_I2C_ADDR); // address LED index 0
    I2C_WriteByte(1, RGB_METER_REG_COUNT, RGB_METER_I2C_ADDR); // 1 LED on the string
    I2C_BufferWrite(rgb, sizeof(rgb), RGB_METER_REG_DATA, RGB_METER_I2C_ADDR);
}

// Map a field voltage (mV) to a blue -> green -> red "getting warmer" gradient.
// NOTE: the full-scale reference is a rough starting point; tune per hardware.
static void rgb_meter_map(uint32_t mv, adc_rssi_ch_t ch, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint32_t max_mv = (ch == ADC_RSSI_CH_HF) ? 20000 : 90000;
    if (mv > max_mv) {
        mv = max_mv;
    }
    uint32_t t = (mv * 511) / (max_mv ? max_mv : 1); // 0..511
    if (t < 256) {              // low field: blue -> green
        *r = 0;
        *g = (uint8_t)t;
        *b = (uint8_t)(255 - t);
    } else {                    // high field: green -> red
        t -= 256;
        *r = (uint8_t)t;
        *g = (uint8_t)(255 - t);
        *b = 0;
    }
}

void RgbFieldMeterEnable(bool on) {
    g_rgb_meter_on = on;
    if (on) {
        if (!g_rgb_meter_i2c_ready) {
            I2C_init(true);
            g_rgb_meter_i2c_ready = true;
        }
        g_rgb_meter_last_ms = 0; // update on the next measurement
    } else {
        rgb_meter_write(0, 0, 0); // turn the LED off when disabling
    }
}

void RgbFieldMeterUpdate(uint32_t mv, adc_rssi_ch_t ch) {
    if (g_rgb_meter_on == false) {
        return;
    }
    // Throttle: a single tune sweep calls the RSSI read hundreds of times in a
    // burst; we only need to refresh the LED a handful of times per second.
    uint32_t now = GetTickCount();
    if ((now - g_rgb_meter_last_ms) < RGB_METER_PERIOD_MS) {
        return;
    }
    g_rgb_meter_last_ms = now;

    uint8_t r, g, b;
    rgb_meter_map(mv, ch, &r, &g, &b);
    rgb_meter_write(r, g, b);
}

/**
  * @brief  gpio configuration.
  * Note: view the 'Datasheet' not 'Reference Manual' for pin maping get.
  */
static void gpio_config(void) {
    gpio_init_type gpio_initstructure;
    gpio_default_para_init(&gpio_initstructure);
    crm_periph_clock_enable(AT32_GPIO_ADC_RSSI_CLK, TRUE);
    // config adc pin as analog input mode
    gpio_initstructure.gpio_mode = GPIO_MODE_ANALOG;
    gpio_initstructure.gpio_pins = AT32_GPIO_ADC_RSSI_LF_PIN | AT32_GPIO_ADC_RSSI_HF_PIN;
    gpio_init(AT32_GPIO_ADC_RSSI, &gpio_initstructure);
}

void AdcSetupRssiChannel(adc_rssi_ch_t ch) {
    adc_common_config_type adc_common_struct;
    adc_base_config_type adc_base_struct;

    adc_common_default_para_init(&adc_common_struct);
    crm_periph_clock_enable(AT32_RSSI_ADC_PERIPH_CLK, TRUE);
    gpio_config();
    adc_reset();

    adc_common_struct.combine_mode = ADC_INDEPENDENT_MODE; // config combine mode
    adc_common_struct.div = ADC_HCLK_DIV_10; // config division,adcclk is division by hclk
    adc_common_struct.common_dma_mode = ADC_COMMON_DMAMODE_DISABLE; // config common dma mode,it's not useful in independent mode
    adc_common_struct.common_dma_request_repeat_state = FALSE; // config common dma request repeat
    adc_common_struct.sampling_interval = ADC_SAMPLING_INTERVAL_5CYCLES; // config adjacent adc sampling interval,it's useful for ordinary shifting mode
    adc_common_struct.tempervintrv_state = TRUE; // config inner temperature sensor and vintrv, connect to ADC1_IN16 & ADC1_IN17, we need to detect vref

    /* config voltage battery */
    adc_common_struct.vbat_state = FALSE;
    adc_common_config(&adc_common_struct);

    adc_base_default_para_init(&adc_base_struct);
    adc_base_struct.sequence_mode = FALSE; // Disable sequence mode, acquire a channel once.
    adc_base_struct.repeat_mode = FALSE;
    adc_base_struct.data_align = ADC_RIGHT_ALIGNMENT;
    adc_base_struct.ordinary_channel_length = 1;
    adc_base_config(AT32_RSSI_ADC, &adc_base_struct);
    adc_resolution_set(AT32_RSSI_ADC, ADC_RESOLUTION_12B);

    // adc_ordinary_conversion_trigger_set(AT32_RSSI_RSSI_ADC, ADC_ORDINARY_TRIG_TMR1CH1, ADC_ORDINARY_TRIG_EDGE_NONE); // config ordinary trigger source and trigger edge
    adc_dma_mode_enable(AT32_RSSI_ADC, FALSE); // config dma mode,it's not useful when common dma mode is use
    adc_dma_request_repeat_enable(AT32_RSSI_ADC, FALSE); // config dma request repeat,it's not useful when common dma mode is use
    adc_occe_each_conversion_enable(AT32_RSSI_ADC, TRUE); // each ordinary channel conversion set occe flag
    adc_interrupt_enable(AT32_RSSI_ADC, ADC_OCCO_INT, FALSE); // disable adc overflow interrupt

    // adc enable and wait ready
    adc_enable(AT32_RSSI_ADC, TRUE);
    while (adc_flag_get(AT32_RSSI_ADC, ADC_RDY_FLAG) == RESET);

    // adc calibration and wait finish
    adc_calibration_init(AT32_RSSI_ADC);
    while (adc_calibration_init_status_get(AT32_RSSI_ADC));
    adc_calibration_start(AT32_RSSI_ADC);
    while (adc_calibration_status_get(AT32_RSSI_ADC));

    // get vref value, ADC_CHANNEL_17 is fixed, don't change!!!
    adc_ordinary_channel_set(AT32_RSSI_ADC, ADC_CHANNEL_17, 1, ADC_SAMPLETIME_640_5);
    AdcRssiConversionStart();
    while(adc_flag_get(AT32_RSSI_ADC, ADC_OCCE_FLAG) == RESET); // Waiting for adc conversion done.
    // printf("vref_value = %f V\r\n", ((double)1.2 * 4095) / adc1_ordinary_value);
    g_adc_vref_value = adc_ordinary_conversion_data_get(AT32_RSSI_ADC);

    // config ordinary channel and start first time conversion.
    if (ch == ADC_RSSI_CH_HF) {
        adc_ordinary_channel_set(AT32_RSSI_ADC, AT32_RSSI_ADC_HF_CHANNEL, 1, ADC_SAMPLETIME_640_5);
    } else {
        adc_ordinary_channel_set(AT32_RSSI_ADC, AT32_RSSI_ADC_LF_CHANNEL, 1, ADC_SAMPLETIME_640_5);
    }
    AdcRssiConversionStart();
}
