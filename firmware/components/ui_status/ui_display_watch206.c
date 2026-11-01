#include "ui_display_watch206.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_board.h"

/*
 * Das AMOLED der Waveshare-Uhr: CO5300 ueber QSPI, 410x502.
 *
 * Der CO5300 versteht den Befehlssatz des SH8601, deshalb der Treiber
 * espressif/esp_lcd_sh8601. Die Startsequenz unten ist die aus dem
 * Board-Paket von Waveshare (waveshare/esp32_s3_touch_amoled_2_06 2.0.0),
 * unveraendert: Spaltenfenster 0x16..0x1AF ist genau die 410 Pixel breite
 * sichtbare Flaeche, daher auch der Versatz von 0x16 in set_gap.
 *
 * Helligkeit gibt es hier nicht als Hintergrundbeleuchtung, sondern als
 * Befehl 0x51 an den Controller -- ein AMOLED leuchtet pixelweise.
 */

static const char *TAG = "ui_display";

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 410
#define LCD_V_RES 502
#define LCD_X_GAP 0x16

/*
 * Zehn Zeilen wie beim StickS3, aus demselben Grund: Die beiden Puffer liegen
 * im DMA-faehigen internen RAM, aus dem sich auch der Aufnahmepfad bedient.
 * 410 * 10 * 2 Byte * 2 = 16.400 Byte. Gerade muss die Zahl sein, weil der
 * CO5300 nur Flaechen mit gerader Kante annimmt (siehe rounder_cb).
 */
#define LVGL_DRAW_BUF_LINES 10

static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    (void)display;
}

/* Der CO5300 schreibt nur Fenster, die auf geraden Koordinaten beginnen und
   auf ungeraden enden; LVGL wird das vor jedem Neuzeichnen gesagt. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

esp_err_t ui_display_watch206_create(lv_display_t **out)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        VOICE_BOARD_PIN_LCD_PCLK, VOICE_BOARD_PIN_LCD_DATA0, VOICE_BOARD_PIN_LCD_DATA1,
        VOICE_BOARD_PIN_LCD_DATA2, VOICE_BOARD_PIN_LCD_DATA3,
        LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "qspi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(VOICE_BOARD_PIN_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg,
                                                 &s_io),
                        TAG, "panel io");

    sh8601_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = VOICE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "lvgl display");

    const size_t buf_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, buf_size, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, buf_size, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");

    lv_display_set_buffers(display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_add_event_cb(display, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_io, &callbacks, display),
                        TAG, "io callbacks");
    *out = display;
    return ESP_OK;
}

esp_err_t ui_display_watch206_set_brightness(uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(s_io, ESP_ERR_INVALID_STATE, TAG, "panel not ready");
    /* QSPI-Befehlsform des Controllers: 0x02 im obersten Byte, Befehl darunter. */
    const int cmd = (0x02 << 24) | (0x51 << 8);
    return esp_lcd_panel_io_tx_param(s_io, cmd, &brightness, 1);
}

void ui_display_watch206_sleep(void)
{
    if (s_panel) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, false));
        /*
         * Sleep-In (0x10): der Controller zieht danach kaum noch Strom. Der
         * SH8601-Treiber bietet dafuer keine Funktion, also direkt -- in
         * derselben QSPI-Befehlsform wie die Helligkeit.
         */
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x10 << 8),
                                                                NULL, 0));
    }
}

void ui_display_watch206_wake(void)
{
    if (s_panel) {
        /*
         * Sleep-Out (0x11) und danach die 120 ms, die der Controller laut
         * SH8601-Befehlssatz braucht, bevor er wieder Bilder annimmt; dann
         * Display an. Der Bildspeicher hat den Schlaf ueberstanden, der alte
         * Inhalt steht also sofort -- LVGL zeichnet den frischen gleich nach.
         */
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x11 << 8),
                                                                NULL, 0));
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, true));
    }
}
