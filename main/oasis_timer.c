#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/rtc_io.h"
#include "oasis_timer.h"

static const char *TAG = "Oasis SystemTime";

/* Thread details. */
#define oasis_sync_system_time_taskname	"oasis_sync_system_time_task"
#define oasis_sync_system_time_task_memory	4096

#define oasis_lt_updater_taskname		"oasis_timestamp_updater"
#define oasis_lt_updater_task_memory		4096

/* Configured delays for threads in msec. */
#define oasis_sync_system_time_task_initial_delay	100
#define oasis_sync_system_time_task_delay		(10 * 60 * 1000)
#define oasis_lt_updater_task_delay			100

#define NTP_SERVER		"in.pool.ntp.org"

#define USEC_IN_MSEC		1000
#define MSEC_IN_SEC		1000
#define USEC_IN_SEC		(MSEC_IN_SEC * USEC_IN_MSEC)

#define MAX_RETRIES		120
#define MIN_SEC_FOR_INIT	1000

struct oasis_system_time{
	TaskHandle_t		timesync_task_handle;
	TaskHandle_t		lt_updater_task_handle;

	struct timeval		boot_timestamp;
	struct timeval		last_timestamp;

	SemaphoreHandle_t	lt_mutex;

	/* Flags used by system time. */
	bool			initial_sync_done;
	bool			sntp_waiting_for_cb;
	bool			system_uptime_calculated;
};

static struct oasis_system_time pst;

static void oasis_set_timezone(void) {
	setenv("TZ", "IST-5:30", 1);
	tzset();
}

static void oasis_timesync_done(struct timeval *tv) {
	if (tv->tv_sec) {
		pst.initial_sync_done = true;

		/* Calculate system boot timestamp. */
		if (!pst.system_uptime_calculated) {
			long system_uptime = esp_timer_get_time();

			pst.boot_timestamp.tv_sec = tv->tv_sec - (system_uptime / USEC_IN_SEC);
			pst.boot_timestamp.tv_usec = tv->tv_usec - system_uptime;

			pst.system_uptime_calculated = true;
		}
	}

	pst.sntp_waiting_for_cb = false;
}

static void oasis_sync_system_time_task(void *unused)
{
	while (true) {
		const TickType_t delay = pst.initial_sync_done ?
			oasis_sync_system_time_task_delay / portTICK_PERIOD_MS :
			oasis_sync_system_time_task_initial_delay / portTICK_PERIOD_MS;
		wifi_ap_record_t ap_info;

		/* Check if system is connected to WiFi. */
		if (!pst.sntp_waiting_for_cb && (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)) {
			if (esp_sntp_enabled()) {
				esp_sntp_restart();
			} else {
				esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
				esp_sntp_setservername(0, NTP_SERVER);
				esp_sntp_init();
			}

			/* Register callback. */
			pst.sntp_waiting_for_cb = true;
			sntp_set_time_sync_notification_cb(oasis_timesync_done);
		}

		vTaskDelay(delay);
	}
}

static void oasis_lt_updater_task(void *unsed)
{
	while (true) {
		const TickType_t delay = oasis_lt_updater_task_delay /
						portTICK_PERIOD_MS;

		if (xSemaphoreTake(pst.lt_mutex, portMAX_DELAY) == pdTRUE) {
			struct timeval current_time;

			gettimeofday(&current_time, NULL);
			pst.last_timestamp.tv_sec = current_time.tv_sec;
			pst.last_timestamp.tv_usec = current_time.tv_usec;

			xSemaphoreGive(pst.lt_mutex);
		}

		vTaskDelay(delay);
	}
}

bool oasis_timer_initialization_required(void)
{
        struct timeval current;

        gettimeofday(&current, NULL);

	if (current.tv_sec > MIN_SEC_FOR_INIT) {
		/* Restore systemtime using RTC. */
		if (xSemaphoreTake(pst.lt_mutex, portMAX_DELAY) == pdTRUE) {
			pst.last_timestamp.tv_sec = current.tv_sec;
			pst.last_timestamp.tv_usec = current.tv_usec;
			xSemaphoreGive(pst.lt_mutex);
		}

		return false;
	}

	return true;
}

/*
 * Launch a FreeRTOS task to sync system time.
 */
esp_err_t oasis_timer_init(void)
{
	unsigned int retries = MAX_RETRIES;
	BaseType_t ret;

	/* Set local timezone. */
	oasis_set_timezone();

	pst.lt_mutex = xSemaphoreCreateMutex();

	if (!oasis_timer_initialization_required()) {
		ESP_LOGI(TAG, "initialization not required");
		return ESP_OK;
	}

	ret = xTaskCreate(oasis_sync_system_time_task,
			  oasis_sync_system_time_taskname,
			  oasis_sync_system_time_task_memory,
			  NULL,
			  tskIDLE_PRIORITY,
			  &pst.timesync_task_handle);

	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Unable to create task: %s",
			 oasis_sync_system_time_taskname);
		return ESP_FAIL;
	}

	ret = xTaskCreate(oasis_lt_updater_task,
			  oasis_lt_updater_taskname,
			  oasis_lt_updater_task_memory,
			  NULL,
			  tskIDLE_PRIORITY,
			  &pst.lt_updater_task_handle);

	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Unable to create task: %s",
			 oasis_lt_updater_taskname);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Waiting for timer to initialize");
	while ((oasis_get_systemtime_sec() < MIN_SEC_FOR_INIT) && retries--)
		vTaskDelay(1000 / portTICK_PERIOD_MS);

	ESP_LOGI(TAG, "Timer initialized: %lld", oasis_get_systemtime_sec());

	return ESP_OK;
}

/* Kills the task which periodically syncs system time. */
void oasis_timer_exit(void)
{
	vTaskDelete(pst.timesync_task_handle);
	pst.timesync_task_handle = NULL;

	vTaskDelete(pst.lt_updater_task_handle);
	pst.lt_updater_task_handle = NULL;
}

time_t oasis_get_systemtime_sec(void)
{
	time_t tv_sec = 0;

	/* This will be maintained by lt_updater_ask. */
	if (xSemaphoreTake(pst.lt_mutex, portMAX_DELAY) == pdTRUE) {
		tv_sec = pst.last_timestamp.tv_sec;
		xSemaphoreGive(pst.lt_mutex);
	}

	return tv_sec;
}

time_t oasis_get_systemtime_usec(void)
{
	time_t tv_usec = 0;
	
	/* This will be maintained by lt_updater_ask. */
	if (xSemaphoreTake(pst.lt_mutex, portMAX_DELAY) == pdTRUE) {
		tv_usec = pst.last_timestamp.tv_usec;
		xSemaphoreGive(pst.lt_mutex);
	}

	return tv_usec;
}
