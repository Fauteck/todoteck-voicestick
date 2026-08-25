#include "audio_tone.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/i2s_types.h"

#include "stick_s3_board.h"

static const char *TAG = "audio_tone";

#define TONE_SAMPLE_RATE 16000
#define TONE_OUT_VOL 78.0f
/* Eine Rampe gegen das Knacken beim Ein- und Ausschalten des Verstaerkers. */
#define TONE_FADE_MS 6

/*
 * Eigener I2S-Port: die Aufnahme haelt I2S_NUM_1. Gleichzeitig geht beides
 * nicht — dieselben Pins —, aber getrennte Ports halten die beiden Pfade im
 * Code auseinander und ersparen dem Aufnahmepfad jede Aenderung.
 */
#define TONE_I2S_PORT I2S_NUM_0

typedef struct {
    uint16_t freq_hz;   /* 0 = Pause */
    uint16_t ms;
} tone_step_t;

static const tone_step_t TONE_START_STEPS[] = {
    {880, 90},
};
static const tone_step_t TONE_DONE_STEPS[] = {
    {660, 70}, {988, 100},
};
static const tone_step_t TONE_ERROR_STEPS[] = {
    {440, 90}, {0, 40}, {330, 150},
};
static const tone_step_t TONE_CANCEL_STEPS[] = {
    {392, 70},
};

static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;

static esp_err_t open_output(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TONE_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL),
                        TAG, "create i2s tx channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = STICK_S3_PIN_ES8311_DOUT,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "init i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx");

    i2c_master_bus_handle_t i2c_bus = stick_s3_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = TONE_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2s data");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /*
         * Der Verstaerker haengt an der L3B-Schiene des M5PM1, die beim
         * Start dauerhaft eingeschaltet wird — es gibt keinen eigenen
         * Enable-Pin, den der Codec-Treiber ziehen koennte.
         */
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = TONE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec for output");
    if (esp_codec_dev_set_out_vol(s_codec, TONE_OUT_VOL) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set output volume failed");
    }
    return ESP_OK;
}

static void close_output(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
}

static esp_err_t play_step(const tone_step_t *step)
{
    const size_t samples = ((size_t)TONE_SAMPLE_RATE * step->ms) / 1000;
    if (samples == 0) {
        return ESP_OK;
    }

    int16_t *buffer = calloc(samples, sizeof(int16_t));
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "allocate tone buffer");

    if (step->freq_hz > 0) {
        const size_t fade = ((size_t)TONE_SAMPLE_RATE * TONE_FADE_MS) / 1000;
        const float step_phase = 2.0f * (float)M_PI * step->freq_hz / TONE_SAMPLE_RATE;
        for (size_t i = 0; i < samples; ++i) {
            float gain = 1.0f;
            if (fade > 0 && i < fade) {
                gain = (float)i / (float)fade;
            } else if (fade > 0 && i + fade >= samples) {
                gain = (float)(samples - i) / (float)fade;
            }
            buffer[i] = (int16_t)(9000.0f * gain * sinf(step_phase * i));
        }
    }

    const int rc = esp_codec_dev_write(s_codec, buffer, samples * sizeof(int16_t));
    free(buffer);
    return rc == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_tone_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create tone lock");
    return ESP_OK;
}

void audio_tone_play(audio_tone_t tone)
{
    const tone_step_t *steps = NULL;
    size_t count = 0;

    switch (tone) {
    case AUDIO_TONE_START:
        steps = TONE_START_STEPS;
        count = sizeof(TONE_START_STEPS) / sizeof(TONE_START_STEPS[0]);
        break;
    case AUDIO_TONE_DONE:
        steps = TONE_DONE_STEPS;
        count = sizeof(TONE_DONE_STEPS) / sizeof(TONE_DONE_STEPS[0]);
        break;
    case AUDIO_TONE_ERROR:
        steps = TONE_ERROR_STEPS;
        count = sizeof(TONE_ERROR_STEPS) / sizeof(TONE_ERROR_STEPS[0]);
        break;
    case AUDIO_TONE_CANCEL:
        steps = TONE_CANCEL_STEPS;
        count = sizeof(TONE_CANCEL_STEPS) / sizeof(TONE_CANCEL_STEPS[0]);
        break;
    }
    if (!steps || !s_lock) {
        return;
    }

    /*
     * Nicht warten: kommt ein zweiter Ton, waehrend der erste laeuft, ist er
     * ohnehin zu spaet, um noch etwas zu quittieren.
     */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Ton uebersprungen, Ausgabe belegt");
        return;
    }

    esp_err_t err = open_output();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Ausgabe nicht verfuegbar: %s", esp_err_to_name(err));
    } else {
        for (size_t i = 0; i < count; ++i) {
            err = play_step(&steps[i]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Ton abgebrochen bei Schritt %u", (unsigned)i);
                break;
            }
        }
    }
    close_output();
    xSemaphoreGive(s_lock);
}
