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
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "hal/i2s_types.h"

#include "voice_board.h"

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

static esp_err_t open_output_at(uint32_t sample_rate);

static esp_err_t open_output(void)
{
    return open_output_at(TONE_SAMPLE_RATE);
}

static esp_err_t open_output_at(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TONE_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL),
                        TAG, "create i2s tx channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = VOICE_BOARD_PIN_I2S_MCLK,
            .bclk = VOICE_BOARD_PIN_I2S_BCLK,
            .ws = VOICE_BOARD_PIN_I2S_WS,
            .dout = VOICE_BOARD_PIN_I2S_DOUT,
            .din = VOICE_BOARD_PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "init i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx");

    i2c_master_bus_handle_t i2c_bus = voice_board_i2c_bus();
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
         * StickS3: Der Verstaerker haengt an der L3B-Schiene des M5PM1, die
         * beim Start dauerhaft eingeschaltet wird — es gibt keinen eigenen
         * Enable-Pin, den der Codec-Treiber ziehen koennte (-1).
         * Waveshare-Uhr: GPIO46 (PA_CTRL). Den zieht der Treiber beim Oeffnen
         * hoch und beim Schliessen wieder herunter — der Verstaerker zieht
         * also nur Strom, waehrend ein Ton laeuft.
         */
        .pa_pin = VOICE_BOARD_PIN_PA,
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
        .sample_rate = sample_rate,
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


/* ── Gesprochene Antworten (Uhr) ──────────────────────────────────────────── */
/*
 * Die Bruecke schickt die Sprachausgabe des Servers als IMA-ADPCM mit 8 kHz:
 * vier Bit je Abtastwert, rund 4 KB je Sekunde -- das passt neben allem
 * anderen durch Bluetooth, und Kodieren wie Dekodieren sind ein paar
 * Tabellenzugriffe. Opus waere kleiner, aber auf dem Handy umstaendlich zu
 * erzeugen.
 *
 * Die Daten landen in einem Puffer im PSRAM (32 s); eine eigene Task wartet
 * auf einen kleinen Vorrat, oeffnet dann den Lautsprecher und spielt ab,
 * waehrend der Rest noch hereinkommt. Toene und Mikrofon teilen sich die
 * I2S-Leitungen: Waehrend gesprochen wird, faellt ein Ton aus (er nimmt die
 * Sperre nicht), und eine Aufnahme beendet die Ausgabe vorher
 * (audio_tone_speech_stop).
 */
#define SPEECH_BUFFER_BYTES (128 * 1024)
/* 0,4 s Vorrat, bevor der Lautsprecher aufgeht -- gegen Stottern am Anfang. */
#define SPEECH_PREBUFFER_BYTES 1600
#define SPEECH_CHUNK_BYTES 256
#define SPEECH_OUT_VOL 85.0f

static StreamBufferHandle_t s_speech_buf;
static TaskHandle_t s_speech_task;
static volatile bool s_speech_ended;
static volatile bool s_speech_stop;
static volatile bool s_speech_active;
static volatile uint32_t s_speech_rate = 8000;

static const int16_t ADPCM_STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ADPCM_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    int32_t predictor;
    int32_t index;
} adpcm_state_t;

static int16_t adpcm_decode_nibble(adpcm_state_t *st, uint8_t nibble)
{
    const int32_t step = ADPCM_STEPS[st->index];
    int32_t diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    st->predictor += (nibble & 8) ? -diff : diff;
    if (st->predictor > 32767) st->predictor = 32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += ADPCM_INDEX[nibble & 0x0F];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return (int16_t)st->predictor;
}

static void speech_task(void *arg)
{
    (void)arg;
    uint8_t chunk[SPEECH_CHUNK_BYTES];
    int16_t pcm[SPEECH_CHUNK_BYTES * 2];
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Vorrat abwarten -- oder das Ende, wenn die Antwort kuerzer ist. */
        while (!s_speech_stop && !s_speech_ended &&
               xStreamBufferBytesAvailable(s_speech_buf) < SPEECH_PREBUFFER_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_speech_stop || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            xStreamBufferReset(s_speech_buf);
            s_speech_active = false;
            continue;
        }
        adpcm_state_t st = { 0, 0 };
        esp_err_t err = open_output_at(s_speech_rate);
        if (err == ESP_OK) {
            (void)esp_codec_dev_set_out_vol(s_codec, SPEECH_OUT_VOL);
            ESP_LOGI(TAG, "Sprachausgabe startet (%u Hz)", (unsigned)s_speech_rate);
            while (!s_speech_stop) {
                const size_t n = xStreamBufferReceive(s_speech_buf, chunk, sizeof(chunk),
                                                      pdMS_TO_TICKS(400));
                if (n == 0) {
                    if (s_speech_ended && xStreamBufferIsEmpty(s_speech_buf)) {
                        break;
                    }
                    continue; /* Daten verspaeten sich: weiter warten */
                }
                for (size_t i = 0; i < n; i++) {
                    pcm[2 * i] = adpcm_decode_nibble(&st, chunk[i] & 0x0F);
                    pcm[2 * i + 1] = adpcm_decode_nibble(&st, chunk[i] >> 4);
                }
                if (esp_codec_dev_write(s_codec, pcm, n * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "Sprachausgabe: Lautsprecher nicht verfuegbar: %s", esp_err_to_name(err));
        }
        close_output();
        xSemaphoreGive(s_lock);
        xStreamBufferReset(s_speech_buf);
        s_speech_active = false;
        ESP_LOGI(TAG, "Sprachausgabe beendet");
    }
}

esp_err_t audio_tone_speech_begin(uint32_t sample_rate)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_speech_buf) {
        s_speech_buf = xStreamBufferCreateWithCaps(SPEECH_BUFFER_BYTES, 1,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_speech_buf, ESP_ERR_NO_MEM, TAG, "speech buffer");
        BaseType_t ok = xTaskCreate(speech_task, "speech", 4096, NULL, 4, &s_speech_task);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "speech task");
    }
    if (s_speech_active) {
        /* Eine neue Antwort loest die alte ab. */
        audio_tone_speech_stop();
    }
    s_speech_rate = (sample_rate >= 8000 && sample_rate <= 16000) ? sample_rate : 8000;
    s_speech_stop = false;
    s_speech_ended = false;
    s_speech_active = true;
    xTaskNotifyGive(s_speech_task);
    return ESP_OK;
}

void audio_tone_speech_feed(const uint8_t *data, size_t len)
{
    if (!s_speech_buf || !s_speech_active || !data || len == 0) {
        return;
    }
    /* Nie blockieren: das ruft der Bluetooth-Strang. Ein voller Puffer verliert. */
    const size_t sent = xStreamBufferSend(s_speech_buf, data, len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "Sprachpuffer voll, %u Byte verworfen", (unsigned)(len - sent));
    }
}

void audio_tone_speech_end(void)
{
    s_speech_ended = true;
}

void audio_tone_speech_stop(void)
{
    if (!s_speech_active) {
        return;
    }
    s_speech_stop = true;
    /* Bis zu einer halben Sekunde warten, bis der Lautsprecher frei ist. */
    for (int i = 0; i < 50 && s_speech_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool audio_tone_speech_active(void)
{
    return s_speech_active;
}
