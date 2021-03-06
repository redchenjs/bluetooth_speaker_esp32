/*
 * audio_render.c
 *
 *  Created on: 2018-04-05 16:41
 *      Author: Jack Chen <redchenjs@live.com>
 */

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "core/os.h"
#include "chip/i2s.h"

#include "user/fft.h"
#include "user/bt_av.h"

#define TAG "audio_render"

RingbufHandle_t audio_buff = NULL;

static uint8_t buff_data[10 * 1024] = {0};
static StaticRingbuffer_t buff_struct = {0};

static void audio_render_task(void *pvParameter)
{
    uint8_t delay = 0;
    bool clear = false;
    bool start = false;

    ESP_LOGI(TAG, "started.");

    while (1) {
        uint8_t *data = NULL;
        uint32_t size = 0;

        taskYIELD();

        if (start) {
            uint32_t remain = sizeof(buff_data) - xRingbufferGetCurFreeSize(audio_buff);

            if (remain >= FFT_BLOCK_SIZE) {
                delay = 0;

                data = xRingbufferReceiveUpTo(audio_buff, &size, portMAX_DELAY, FFT_BLOCK_SIZE);
            } else if (remain > 0) {
                delay = 0;

                data = xRingbufferReceiveUpTo(audio_buff, &size, portMAX_DELAY, remain);
            } else {
                if (++delay <= 2) {
                    vTaskDelay(256000 / a2d_sample_rate / portTICK_RATE_MS);
                } else {
                    delay = 0;

                    clear = false;
                    start = false;
                }

                continue;
            }
        } else {
            if (!clear) {
#ifdef CONFIG_ENABLE_VFX
                EventBits_t uxBits = xEventGroupGetBits(user_event_group);
                if (!(uxBits & AUDIO_INPUT_RUN_BIT) && (uxBits & VFX_FFT_MODE_BIT)) {
                    xEventGroupWaitBits(
                        user_event_group,
                        VFX_FFT_IDLE_BIT | VFX_RLD_MODE_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY
                    );

                    fft_init();

                    xEventGroupClearBits(user_event_group, VFX_FFT_IDLE_BIT);
                }
#endif
                data = xRingbufferReceive(audio_buff, &size, portMAX_DELAY);
                if (data != NULL) {
                    vRingbufferReturnItem(audio_buff, data);
                }

                clear = true;
            } else {
                if (xRingbufferGetCurFreeSize(audio_buff) >= FFT_BLOCK_SIZE) {
                    vTaskDelay(1 / portTICK_RATE_MS);
                } else {
                    start = true;
                }
            }

            continue;
        }

        if (xEventGroupGetBits(user_event_group) &
            (AUDIO_PLAYER_RUN_BIT | BT_A2DP_IDLE_BIT | OS_PWR_RESET_BIT | OS_PWR_SLEEP_BIT)) {
            vRingbufferReturnItem(audio_buff, data);
            continue;
        }

        i2s_output_set_sample_rate(a2d_sample_rate);

        size_t bytes_written = 0;
        i2s_write(CONFIG_AUDIO_OUTPUT_I2S_NUM, data, size, &bytes_written, portMAX_DELAY);

#ifndef CONFIG_AUDIO_INPUT_NONE
        if (xEventGroupGetBits(user_event_group) & AUDIO_INPUT_RUN_BIT) {
            vRingbufferReturnItem(audio_buff, data);
            continue;
        }
#endif

#ifdef CONFIG_ENABLE_VFX
        if (size < FFT_BLOCK_SIZE || !(xEventGroupGetBits(user_event_group) & VFX_FFT_IDLE_BIT)) {
            vRingbufferReturnItem(audio_buff, data);
            continue;
        }

    #ifdef CONFIG_BT_AUDIO_FFT_ONLY_LEFT
        fft_load_data(data, FFT_CHANNEL_L);
    #elif defined(CONFIG_BT_AUDIO_FFT_ONLY_RIGHT)
        fft_load_data(data, FFT_CHANNEL_R);
    #else
        fft_load_data(data, FFT_CHANNEL_LR);
    #endif

        xEventGroupClearBits(user_event_group, VFX_FFT_IDLE_BIT);
#endif

        vRingbufferReturnItem(audio_buff, data);
    }
}

void audio_render_init(void)
{
    audio_buff = xRingbufferCreateStatic(sizeof(buff_data), RINGBUF_TYPE_BYTEBUF, buff_data, &buff_struct);

    xTaskCreatePinnedToCore(audio_render_task, "audioRenderT", 1920, NULL, configMAX_PRIORITIES - 3, NULL, 0);
}
