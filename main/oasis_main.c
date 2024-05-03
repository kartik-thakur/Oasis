/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_touch.h"
#include "oasis_timer.h"

#define TAG					"Oasis"

#define OASIS_PUMP_GPIO				GPIO_NUM_8
#define OASIS_LED_GPIO				GPIO_NUM_2

#define HOUR_IN_DAY				24ULL
#define MIN_IN_HOUR				60ULL
#define SEC_IN_MIN				60ULL
#define MSEC_IN_SEC				1000ULL
#define USEC_IN_MSEC				1000ULL

#define OASIS_BLINK_DELAY			(3000 / portTICK_PERIOD_MS)
#define OASIS_WATER_DURATION_MSEC_FOR_TIMER	((60 * MSEC_IN_SEC) / portTICK_PERIOD_MS)
#define OASIS_WATER_DURATION_MSEC_FOR_TEST	((5 * MSEC_IN_SEC) / portTICK_PERIOD_MS)

#define USEC_IN_SEC				(MSEC_IN_SEC * USEC_IN_MSEC)
#define USEC_IN_MIN				(SEC_IN_MIN * USEC_IN_SEC)
#define USEC_IN_HOUR				(MIN_IN_HOUR * USEC_IN_MIN)

#define MIN_IN_DAY				(HOUR_IN_DAY * MIN_IN_HOUR)
#define SEC_IN_DAY				(MIN_IN_DAY * SEC_IN_MIN)
#define MSEC_IN_DAY				(SEC_IN_DAY * MSEC_IN_SEC)
#define USEC_IN_DAY				(MSEC_IN_DAY * USEC_IN_MSEC)

struct oasis_alarm {
	unsigned int	hour;
	unsigned int	minute;
};

static struct oasis_alarm alarms[] = {
	{ .hour = 7, .minute = 30 },
	{ .hour = 17, .minute = 30 },
	{ /* Sentinal. */ }
};

static void oasis_configure_pump_gpio(void)
{
	gpio_set_direction(OASIS_PUMP_GPIO, GPIO_MODE_OUTPUT);
}

static void oasis_start_watering(void)
{
	gpio_num_t gpio = OASIS_PUMP_GPIO;

	gpio_set_level(gpio, 1);
}

static void oasis_stop_watering(void)
{
	gpio_num_t gpio = OASIS_PUMP_GPIO;

	gpio_set_level(gpio, 0);
}

static void oasis_led_blink_task(void *args)
{
	gpio_set_direction(OASIS_LED_GPIO, GPIO_MODE_OUTPUT);

	while (true) {
		gpio_set_level(OASIS_LED_GPIO, 1);
		vTaskDelay(OASIS_BLINK_DELAY);
		gpio_set_level(OASIS_LED_GPIO, 0);
		vTaskDelay(OASIS_BLINK_DELAY);
	}
}

static void oasis_deep_sleep_task(void *args)
{
	switch(esp_sleep_get_wakeup_cause()) {
	case ESP_SLEEP_WAKEUP_TIMER:
		ESP_LOGI(TAG, "Wakeup caused by timer");

		oasis_start_watering();
		vTaskDelay(OASIS_WATER_DURATION_MSEC_FOR_TIMER);
		oasis_stop_watering();
		break;
	default:
		ESP_LOGI(TAG, "Unknown wakeup reason");

		oasis_start_watering();
		vTaskDelay(OASIS_WATER_DURATION_MSEC_FOR_TEST);
		oasis_stop_watering();
		break;
	}

	/* Enter into deep sleep mode. */
	ESP_LOGI(TAG, "Entering deep sleep mode");

	vTaskDelay(1000 / portTICK_PERIOD_MS);
	esp_deep_sleep_start();
}

static uint64_t oasis_get_wakeup_time_us(void)
{
	time_t t_seconds = oasis_get_systemtime_sec();
	uint64_t min = 2 * USEC_IN_DAY;
	struct tm *time_info;
	unsigned int i;

	time_info = localtime(&t_seconds);

	for (i = 0 ; alarms[i].hour; i++) {
		uint64_t time_difference_usec;

		time_difference_usec = time_info->tm_hour > alarms[i].hour ?
					(24 + alarms[i].hour - time_info->tm_hour) :
					(alarms[i].hour - time_info->tm_hour);

		time_difference_usec *= USEC_IN_HOUR;

		if (time_info->tm_min > alarms[i].minute) {
			if (!time_difference_usec)
				time_difference_usec = USEC_IN_DAY;

			time_difference_usec -= USEC_IN_HOUR;
			time_difference_usec += (60 - time_info->tm_min + alarms[i].minute) * USEC_IN_MIN;
		} else {
			time_difference_usec += (alarms[i].minute - time_info->tm_min) * USEC_IN_MIN;
		}

		min = time_difference_usec < min ? time_difference_usec : min;
	}

	return min;
}

void oasis_configure_wakeup_source(void)
{
	const uint64_t wakeup_delay_usec = oasis_get_wakeup_time_us();

	ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(wakeup_delay_usec));
	ESP_LOGI(TAG, "Configured timer for %lld micro seconds", wakeup_delay_usec);
}

void app_main(void)
{
	esp_err_t err;

	/* Set Motor GPIO to low. */
	oasis_configure_pump_gpio();
	oasis_stop_watering();

	/* Start blinking LED to let user know that esp is powered up. */
	xTaskCreate(oasis_led_blink_task, "oasis_led_blink_task", 4096, NULL, 6, NULL);
	vTaskDelay(10000 / portTICK_PERIOD_MS);

	ESP_LOGI(TAG, "Project Oasis, keeping the plants watered in this extreme heat!");

	/* Configure network. */
	ESP_ERROR_CHECK(nvs_flash_init());
	initialize_wifi();

	vTaskDelay(10000 / portTICK_PERIOD_MS);

	/* Configure system time. */
	err = oasis_timer_init();
	if (err != ESP_OK)
		ESP_LOGI(TAG, "Unable to initialize system timer\n");

	oasis_configure_wakeup_source();

	xTaskCreate(oasis_deep_sleep_task, "oasis_deep_sleep_task", 4096, NULL, 6, NULL);
}
