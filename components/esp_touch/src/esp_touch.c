#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_smartconfig.h"
#include "esp_netif.h"

#include "../include/esp_touch.h"

#define SMARTCONFIG_TYPE	CONFIG_ESP_SMARTCONFIG_TYPE

static EventGroupHandle_t	esp_touch_event_group;

static const int CONNECTED_BIT		= BIT0;
static const int ESPTOUCH_DONE_BIT	= BIT1;

static const char *TAG = "esp_touch";

static void esp_touch_task(void *param)
{
	EventBits_t			uxBits;

	ESP_ERROR_CHECK(esp_smartconfig_set_type(SMARTCONFIG_TYPE));

	smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

	while (true) {
		uxBits = xEventGroupWaitBits(esp_touch_event_group,
			CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false,
			portMAX_DELAY);

		if (uxBits & CONNECTED_BIT) {
			ESP_LOGI(TAG, "WiFi connected");
		}

		if (uxBits & ESPTOUCH_DONE_BIT) {
			ESP_LOGI(TAG, "Esp Touch configuration over");
			esp_smartconfig_stop();
			vTaskDelete(NULL);
		}
	}
}

static void run_esp_touch_task(void)
{
	xTaskCreate(esp_touch_task, "esp_touch_task", 4096, NULL,
		3, NULL);
}

static void esp_touch_wifi_event_handler(void *arg, int32_t event_id,
		void *event_data)
{
	switch (event_id) {
	case WIFI_EVENT_STA_START: ;
		wifi_config_t	wifi_config;
		esp_err_t	err;

		err = esp_wifi_get_config(ESP_IF_WIFI_STA,
				&wifi_config);
		if (err != ESP_OK)
			return run_esp_touch_task();

		err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
		if (err != ESP_OK)
			return run_esp_touch_task();

		err = esp_wifi_connect();
		if (err != ESP_OK)
			return run_esp_touch_task();

		break;
	case WIFI_EVENT_STA_DISCONNECTED:
		/*
		 * Try reconnecting.
		 */

		ESP_ERROR_CHECK(esp_wifi_connect());
		xEventGroupClearBits(esp_touch_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
}

static void esp_touch_ip_event_handler(void *arg, int32_t event_id,
		void *event_data)
{
	switch (event_id) {
	case IP_EVENT_STA_GOT_IP:
		/*
		 * IP Allocated.
		 */

		xEventGroupSetBits(esp_touch_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
}

static void esp_touch_smartconfig_event_handler(void *arg, int32_t event_id,
		void *event_data)
{
	switch (event_id) {
	case SC_EVENT_SCAN_DONE:
		break;
	case SC_EVENT_FOUND_CHANNEL:
		break;
	case SC_EVENT_GOT_SSID_PSWD: ;
		/*
		 * Got Wifi Credentails. Let's try connecting.
		 */
		wifi_config_t				wifi_config;
		smartconfig_event_got_ssid_pswd_t*	event =
			(smartconfig_event_got_ssid_pswd_t*)event_data;

		bzero(&wifi_config, sizeof(wifi_config_t));
		memcpy(wifi_config.sta.ssid, event->ssid,
		       sizeof(wifi_config.sta.ssid));
		memcpy(wifi_config.sta.password, event->password,
		       sizeof(wifi_config.sta.password));
		wifi_config.sta.bssid_set = event->bssid_set;

		if (wifi_config.sta.bssid_set)
			memcpy(wifi_config.sta.bssid, event->bssid,
			       sizeof(wifi_config.sta.bssid));

		ESP_ERROR_CHECK(esp_wifi_disconnect());
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA,
				&wifi_config));
		ESP_ERROR_CHECK(esp_wifi_connect());

		break;
	case SC_EVENT_SEND_ACK_DONE:
		xEventGroupSetBits(esp_touch_event_group, ESPTOUCH_DONE_BIT);
		break;
	default:
		break;
	}
}

static void esp_touch_event_handler(void *arg, esp_event_base_t event_base,
			int32_t event_id, void *event_data)
{

	if (event_base == WIFI_EVENT)
		esp_touch_wifi_event_handler(arg, event_id, event_data);
	else if (event_base == IP_EVENT)
		esp_touch_ip_event_handler(arg, event_id, event_data);
	else if (event_base == SC_EVENT)
		esp_touch_smartconfig_event_handler(arg, event_id, event_data);
}

void initialize_wifi()
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_netif_t *sta_netif;

	esp_touch_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
			ESP_EVENT_ANY_ID,
			&esp_touch_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
			IP_EVENT_STA_GOT_IP,
			&esp_touch_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
			&esp_touch_event_handler, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
}
