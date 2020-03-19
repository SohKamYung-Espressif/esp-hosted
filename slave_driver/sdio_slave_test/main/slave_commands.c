// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_wifi_types.h>
#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include "esp_system.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "slave_commands.h"
#include "slave_config.pb-c.h"

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define SUCCESS "success"
#define FAILURE "failure"

static const char* TAG = "slave_commands";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

typedef struct {
	uint8_t ssid[32];
	uint8_t pwd[64];
	uint8_t bssid[19];
	uint8_t chnl;
	uint8_t max_conn;
	int8_t rssi;
	bool ssid_hidden;
	wifi_auth_mode_t ecn;
	uint8_t bw;
	uint16_t count;
} credentials_t;

typedef struct {
	bool is_ap_connected;
	bool is_softap_started;
} flags;

static flags hosted_flags;

static credentials_t credentials;

/* Bits for wifi connect event */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY			5

// sattion got IP event is remain
static int s_retry_num = 0;
static bool scan_done = false;

static void ap_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
		ESP_LOGI(TAG,"wifi connected");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
			ESP_LOGI(TAG,"sta disconncted, set group bit");
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void softap_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static void ap_scan_list_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        scan_done = true;
    }
}

static void ap_event_register(void)
{
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ap_event_handler, NULL));
	ESP_LOGI(TAG,"AP Event group registered");
}

static void ap_event_unregister(void)
{
	ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler));
	ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ap_event_handler));
	ESP_LOGI(TAG, "AP Event group unregistered");
}

static void softap_event_register(void)
{
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &softap_event_handler, NULL));
	ESP_LOGI(TAG,"SoftAP Event group registered");
}

static void ap_scan_list_event_register(void)
{
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_scan_list_event_handler, NULL));
}

typedef struct slave_config_cmd {
    int cmd_num;
    esp_err_t (*command_handler)(SlaveConfigPayload *req,
                                 SlaveConfigPayload *resp, void *priv_data);
} slave_config_cmd_t;

static esp_err_t cmd_get_mac_address_handler(SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	esp_err_t ret;
	uint8_t mac[6];
	char *mac_str = (char *)calloc(1,19);
	if (mac_str == NULL) {
		ESP_LOGE(TAG, "Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	if (strcmp(req->cmd_get_mac_address->cmd, "1") == 0) {
		ret = esp_wifi_get_mac(ESP_IF_WIFI_STA , mac);
		ESP_LOGI(TAG,"get station mac address");
		if (ret != ESP_OK) {
			ESP_LOGE(TAG,"Error in getting MAC of ESP Station %d", ret);
			return ESP_FAIL;
		}
	} else if (strcmp(req->cmd_get_mac_address->cmd,"2") == 0) {
		ret = esp_wifi_get_mac(ESP_IF_WIFI_AP, mac);
		ESP_LOGI(TAG,"get AP mac address");
		if (ret != ESP_OK) {
			ESP_LOGE(TAG,"Error in getting MAC of ESP AP %d", ret);
			return ESP_FAIL;
		}
	} else {
		ESP_LOGI(TAG,"Invalid msg type");
		return ESP_FAIL;
	}

	sprintf(mac_str,MACSTR,MAC2STR(mac));
	RespGetStatus *resp_payload = (RespGetStatus *)calloc(1,sizeof(RespGetStatus));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_get_status__init(resp_payload);
	ESP_LOGI(TAG,"mac [%s] ", mac_str);
	resp_payload->resp = mac_str;
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_GET_MAC_ADDRESS ;
	resp->resp_get_mac_address = resp_payload;
	ESP_LOGI(TAG,"mac address %s ", resp->resp_get_mac_address->resp);
	return ESP_OK;
}

static esp_err_t cmd_get_wifi_mode_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	ESP_LOGI(TAG,"inside get wifi mode");
	esp_err_t ret;
	wifi_mode_t mode;
	ret = esp_wifi_get_mode(&mode);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get wifi mode %d", ret);
		return ESP_FAIL;
	}
	RespGetStatus *resp_payload = (RespGetStatus *)calloc(1,sizeof(RespGetStatus));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_get_status__init(resp_payload);
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_GET_WIFI_MODE ;
	resp_payload->has_mode = 1;
	resp_payload->mode = mode;
	resp->resp_get_wifi_mode = resp_payload;
	return ESP_OK;
}

static esp_err_t cmd_set_wifi_mode_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	esp_err_t ret;
	wifi_mode_t num = req->cmd_set_wifi_mode->mode;
	ESP_LOGI(TAG,"num %d ", num);
	ret = esp_wifi_set_mode(num);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to set mode");
		return ESP_FAIL;
	}
	/* ESP_LOGI(TAG,"set mode done");
	esp_wifi_get_mode(&num);
	ESP_LOGI(TAG,"current set mode is %d", num); */
	RespGetStatus *resp_payload = (RespGetStatus *)calloc(1,sizeof(RespGetStatus));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_get_status__init(resp_payload);
	resp_payload->has_mode = 1;
	resp_payload->mode = num;
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_SET_WIFI_MODE ;
	resp->resp_set_wifi_mode = resp_payload;
	return ESP_OK;
}

static esp_err_t cmd_set_ap_config_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	ESP_LOGI(TAG,"connect to AP function");
	esp_err_t ret;
	s_wifi_event_group = xEventGroupCreate();
	ap_event_register();
	ret = esp_wifi_set_mode(WIFI_MODE_STA);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"failed to set mode");
		return ESP_FAIL;
	}
	wifi_config_t* wifi_cfg = (wifi_config_t *)calloc(1,sizeof(wifi_config_t));
	if (wifi_cfg == NULL) {
		ESP_LOGE(TAG,"failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	memcpy(wifi_cfg->sta.ssid,req->cmd_set_ap_config->ssid,strlen(req->cmd_set_ap_config->ssid));
	memcpy(wifi_cfg->sta.password,req->cmd_set_ap_config->pwd,strlen(req->cmd_set_ap_config->pwd));
	if (strlen(req->cmd_set_ap_config->bssid)) {
		wifi_cfg->sta.bssid_set = true;
		memcpy(wifi_cfg->sta.bssid,req->cmd_set_ap_config->bssid,6);
	}

	ESP_LOGI(TAG,"final AP name %s ", wifi_cfg->sta.ssid);
	ESP_LOGI(TAG,"final AP password %s", wifi_cfg->sta.password);
 	ret = esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to set wifi softAP mode");
		return ESP_FAIL;
	}
	ret = esp_wifi_start();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to start wifi");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"wifi start is called");
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", req->cmd_set_ap_config->ssid , req->cmd_set_ap_config->pwd );
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", req->cmd_set_ap_config->ssid , req->cmd_set_ap_config->pwd );
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}
	/* To check ESP station is connected to AP */
	/* wifi_ap_record_t ap_info;
	ret = esp_wifi_sta_get_ap_info(&ap_info));
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to AP info to which ESP32 is connected");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"ssid %s", (char*)ap_info.ssid); */
	hosted_flags.is_ap_connected = true;
	RespConfig *resp_payload = (RespConfig *)calloc(1,sizeof(RespConfig));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_config__init (resp_payload);
	resp_payload->status = SUCCESS;
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_SET_AP_CONFIG ;
	resp->resp_set_ap_config = resp_payload;
	ap_event_unregister();

	ESP_LOGI(TAG,"connected to AP ssid");
	vEventGroupDelete(s_wifi_event_group);
	free(wifi_cfg);
	return ESP_OK;
}

static esp_err_t cmd_get_ap_config_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	if (hosted_flags.is_ap_connected == false) {
		ESP_LOGI(TAG,"ESP32 station is not connected with AP, can't get AP configuration");
		return ESP_FAIL;
	}
	esp_err_t ret;
	ESP_LOGI(TAG,"get ap config");
	wifi_ap_record_t *ap_info = (wifi_ap_record_t *)calloc(1,sizeof(wifi_ap_record_t));
	if (ap_info == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}

	ret = esp_wifi_sta_get_ap_info(ap_info);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to get AP config %d \n", ret);
		free(ap_info);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG,"AP ssid %s AP bssid %s",ap_info->ssid,ap_info->bssid);
	ESP_LOGI(TAG,"sizeof rssi %d channel %d ", sizeof(ap_info->rssi), sizeof(ap_info->primary));
	sprintf((char *)credentials.bssid,MACSTR,MAC2STR(ap_info->bssid));
	memcpy(credentials.ssid,ap_info->ssid,strlen((char *)ap_info->ssid));
	credentials.rssi = ap_info->rssi;
	credentials.chnl = ap_info->primary;
	ESP_LOGI(TAG,"afetr memcpy ssid %s bssid %s",credentials.ssid, credentials.bssid);
	ESP_LOGI(TAG,"data present in rssi %d and channel field %d ", credentials.rssi, credentials.chnl);
	RespConfig *resp_payload = (RespConfig *)calloc(1,sizeof(RespConfig));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_config__init (resp_payload);
	resp_payload->ssid = (char *)credentials.ssid;
	resp_payload->bssid = (char *)credentials.bssid;
	resp_payload->has_rssi = 1;
	resp_payload->rssi = credentials.rssi;
	resp_payload->has_chnl = 1;
	resp_payload->chnl = credentials.chnl;
	resp_payload->status = SUCCESS;
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_GET_AP_CONFIG ;
	resp->resp_get_ap_config = resp_payload;
	free(ap_info);
	return ESP_OK;
}

static esp_err_t cmd_disconnect_ap_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	if (hosted_flags.is_ap_connected == false) {
		ESP_LOGI(TAG,"ESP32 station is not connected with AP, can't disconnect from AP");
		return ESP_FAIL;
	}
	esp_err_t ret;
	ret = esp_wifi_disconnect();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to disconnect");
		return ESP_FAIL;
	}
	RespGetStatus *resp_payload = (RespGetStatus *)calloc(1,sizeof(RespGetStatus));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_get_status__init(resp_payload);
	ESP_LOGI(TAG,"resp_payload address %p and resp also %p", resp_payload, resp_payload->resp);
	resp_payload->resp = SUCCESS;
	ESP_LOGI(TAG,"response success string %s",resp_payload->resp);
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_DISCONNECT_AP;
	resp->resp_disconnect_ap = resp_payload;
	ESP_LOGI(TAG,"disconnect AP here");

	return ESP_OK;
}

static esp_err_t cmd_get_softap_config_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	if (!hosted_flags.is_softap_started) {
		ESP_LOGI(TAG,"ESP32 SoftAP mode aren't set, So can't get config");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"get soft AP handler");
	esp_err_t ret;
	wifi_config_t *get_conf = (wifi_config_t *)calloc(1,sizeof(wifi_config_t));
	if (get_conf == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	wifi_bandwidth_t *get_bw = (wifi_bandwidth_t *)calloc(1,sizeof(wifi_bandwidth_t));
	if (get_bw == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	ret = esp_wifi_get_config(ESP_IF_WIFI_AP, get_conf);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to get SoftAP config");
		return ESP_FAIL;
	}
	RespConfig *resp_payload = (RespConfig *)calloc(1,sizeof(RespConfig));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_config__init (resp_payload);
	ESP_LOGI(TAG,"ssid name %s",get_conf->ap.ssid);
	ESP_LOGI(TAG,"pwd %s",get_conf->ap.password);
	ESP_LOGI(TAG,"chnl %d",get_conf->ap.channel);
	ESP_LOGI(TAG,"ecn %d",get_conf->ap.authmode);
	ESP_LOGI(TAG,"max conn %d",get_conf->ap.max_connection);
	ESP_LOGI(TAG,"ssid hidden %d",get_conf->ap.ssid_hidden);
	ESP_LOGI(TAG,"len of ssid %d", strlen((char *)get_conf->ap.ssid));
	ESP_LOGI(TAG,"len of pwd %d",strlen((char *)get_conf->ap.password));
	memcpy(credentials.ssid,get_conf->ap.ssid,strlen((char *)get_conf->ap.ssid));
	if (strlen((char*)get_conf->ap.password)) {
		memcpy(credentials.pwd,get_conf->ap.password,strlen((char *)get_conf->ap.password));
	}
	credentials.chnl = get_conf->ap.channel;
	credentials.max_conn = get_conf->ap.max_connection;
	credentials.ecn = get_conf->ap.authmode;
	credentials.ssid_hidden = get_conf->ap.ssid_hidden;
	ESP_LOGI(TAG,"ssid %s pwd %s chnl %d ecn %d max_conn %d ssid_hidden %d",credentials.ssid, credentials.pwd, credentials.chnl, credentials.ecn, credentials.max_conn, credentials.ssid_hidden );
	resp_payload->ssid = (char *)credentials.ssid;
	resp_payload->pwd = (char *)credentials.pwd;
	resp_payload->has_chnl = 1;
	resp_payload->chnl = credentials.chnl;
	resp_payload->has_ecn = 1;
	resp_payload->ecn = credentials.ecn;
	resp_payload->has_max_conn = 1;
	resp_payload->max_conn = credentials.max_conn;
	resp_payload->ssid_hidden = credentials.ssid_hidden;
	ret = esp_wifi_get_bandwidth(ESP_IF_WIFI_AP,get_bw);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to get bandwidth");
	}
	ESP_LOGI(TAG,"got bandwidth now %d", *get_bw);
	resp_payload->bw = *(int *)get_bw;
	resp_payload->status = SUCCESS;
	ESP_LOGI(TAG,"response success string %s",resp_payload->status);
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_GET_SOFTAP_CONFIG  ;
	resp->resp_get_softap_config = resp_payload;
	free(get_conf);
	free(get_bw);
	return ESP_OK;
}

static esp_err_t cmd_set_softap_config_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	ESP_LOGI(TAG,"set soft AP handler");
	esp_err_t ret;
	softap_event_register();
	ESP_LOGI(TAG,"event handler registered in set softap config ");
	ret = esp_wifi_set_mode(WIFI_MODE_AP);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to set mode");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"set mode as softAP");
	wifi_config_t *wifi_config = (wifi_config_t *)calloc(1,sizeof(wifi_config_t));
	if (wifi_config == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	uint8_t ssid_length = strlen(req->cmd_set_softap_config->ssid);
	wifi_config->ap.authmode = req->cmd_set_softap_config->ecn;
	uint8_t pwd_length = strlen(req->cmd_set_softap_config->pwd);

	if (wifi_config->ap.authmode != WIFI_AUTH_OPEN)	{
		memcpy(wifi_config->ap.password,req->cmd_set_softap_config->pwd,pwd_length);
	}

	memcpy(wifi_config->ap.ssid,req->cmd_set_softap_config->ssid,ssid_length);
	ESP_LOGI(TAG,"ssid len %d and password len %d",ssid_length, pwd_length);
	wifi_config->ap.ssid_len = ssid_length;
	wifi_config->ap.channel = req->cmd_set_softap_config->chnl;

	wifi_config->ap.max_connection = req->cmd_set_softap_config-> max_conn;
	wifi_config->ap.ssid_hidden = req->cmd_set_softap_config->ssid_hidden;
	uint8_t mac[6];
	ret = esp_wifi_get_mac(WIFI_IF_AP, mac);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to get MAC address");
		return ESP_FAIL;
	}
	ret = esp_wifi_set_bandwidth(ESP_IF_WIFI_AP,req->cmd_set_softap_config->bw);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to set MAC address");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, MACSTR, MAC2STR(mac));
	ret = esp_wifi_set_config(ESP_IF_WIFI_AP, wifi_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to set AP config");
		return ESP_FAIL;
	}
	ret = esp_wifi_start();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG,"Failed to start WiFi");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"ssid %d %s pwd %d %s authmode %d ssid_hidden %d max_conn %d channel %d",ssid_length, wifi_config->ap.ssid, pwd_length, wifi_config->ap.password, wifi_config->ap.authmode,wifi_config->ap.ssid_hidden,wifi_config->ap.max_connection,wifi_config->ap.channel);

	RespConfig *resp_payload = (RespConfig *)calloc(1,sizeof(RespConfig));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_config__init (resp_payload);
	ESP_LOGI(TAG,"resp_payload address %p and status also %p", resp_payload, resp_payload->status);
	resp_payload->status = SUCCESS;
	ESP_LOGI(TAG,"response success string %s",resp_payload->status);
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_SET_SOFTAP_CONFIG ;
	resp->resp_set_softap_config = resp_payload;
	ESP_LOGI(TAG,"ESp32 SoftAP is avaliable ");

	hosted_flags.is_softap_started = true;
	free(wifi_config);
	return ESP_OK;
}

static esp_err_t cmd_get_ap_scan_list_handler (SlaveConfigPayload *req,
                                        SlaveConfigPayload *resp, void *priv_data)
{
	ESP_LOGI(TAG,"Inside get AP Scan list handler");
	ESP_LOGI(TAG,"scan entry %d",req->cmd_scan_ap_list->count);
	ap_scan_list_event_register();
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));

	uint16_t scan_count = req->cmd_scan_ap_list->count;
	uint16_t ap_count = 0;

	wifi_ap_record_t *ap_info = (wifi_ap_record_t *)calloc(scan_count,sizeof(wifi_ap_record_t));
	if (ap_info == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&scan_count,ap_info));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
	ESP_LOGI(TAG,"Total APs scanned = %u",ap_count);
	credentials.count = (scan_count <= ap_count ) ? scan_count: ap_count;
	RespScanResult * resp_payload = (RespScanResult *)calloc(1,sizeof(RespScanResult));
	if (resp_payload == NULL) {
		ESP_LOGE(TAG,"Failed To allocate memory");
		return ESP_ERR_NO_MEM;
	}
	resp_scan_result__init(resp_payload);
	resp->payload_case = SLAVE_CONFIG_PAYLOAD__PAYLOAD_RESP_SCAN_AP_LIST ;
	resp_payload->has_count = 1;
	resp_payload->count = credentials.count;
	resp_payload->n_entries = scan_count;
	ESP_LOGI(TAG,"n_entries %d",resp_payload->n_entries);
	ScanResult **results = (ScanResult **) calloc(scan_count,sizeof(ScanResult));
	if (results == NULL) {
		ESP_LOGE(TAG,"Failed To allocate memory");
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG,"address of results %p",results);
	resp_payload->entries = results;
	for (int i = 0; i < credentials.count; i++ ) {
		ESP_LOGI(TAG,"SSID \t\t%s", ap_info[i].ssid);
		ESP_LOGI(TAG,"RSSI \t\t%d", ap_info[i].rssi);
		ESP_LOGI(TAG,"Channel \t\t%d\n", ap_info[i].primary);
		results[i] = (ScanResult *)calloc(1,sizeof(ScanResult));
		if (results[i] == NULL) {
			ESP_LOGE(TAG,"Failed to allocate memory");
			return ESP_ERR_NO_MEM;
		}
		scan_result__init(results[i]);
		ESP_LOGI(TAG,"scan init on %dth location done ",i);
		results[i]->has_ssid = 1;
		results[i]->ssid.len = strnlen((char *)ap_info[i].ssid, 32);
		results[i]->ssid.data = (uint8_t *)strndup((char *)ap_info[i].ssid,32);
		if (!results[i]->ssid.data) {
			ESP_LOGE(TAG,"Failed to allocate memory for scan result entry SSID");
			return ESP_ERR_NO_MEM;
		}
		ESP_LOGI(TAG,"ssid scan %s %d", results[i]->ssid.data,results[i]->ssid.len );
		results[i]->has_chnl = 1;
		credentials.chnl = ap_info[i].primary;
		results[i]->chnl = credentials.chnl;
		ESP_LOGI(TAG,"chnl scan %d %d",results[i]->chnl,resp_payload->entries[i]->chnl );
		results[i]->has_rssi = 1;
		credentials.rssi = ap_info[i].rssi;
		results[i]->rssi = credentials.rssi;
		ESP_LOGI(TAG,"chnl scan %d",resp_payload->entries[i]->rssi );
		results[i]->has_bssid = 1;
		sprintf((char *)credentials.bssid,MACSTR,MAC2STR(ap_info[i].bssid));
		results[i]->bssid.len = strnlen((char *)credentials.bssid,19);
		results[i]->bssid.data = (uint8_t *)strndup((char *)credentials.bssid,19);
		if (!results[i]->bssid.data) {
			ESP_LOGE(TAG, "Failed to allocate memory for scan result entry BSSID");
			return ESP_ERR_NO_MEM;
		}
		ESP_LOGI(TAG,"bssid %s", results[i]->bssid.data);
		results[i]->has_ecn = 1;
		credentials.ecn = ap_info[i].authmode;
		results[i]->ecn = credentials.ecn;
		ESP_LOGI(TAG,"auth mode %d", results[i]->ecn);
	}

	resp->resp_scan_ap_list = resp_payload;
	return ESP_OK;
}
static slave_config_cmd_t cmd_table[] = {
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdGetMACAddress ,
		.command_handler = cmd_get_mac_address_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdGetWiFiMode,
		.command_handler = cmd_get_wifi_mode_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdSetWiFiMode,
		.command_handler = cmd_set_wifi_mode_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdGetAPConfig ,
		.command_handler = cmd_get_ap_config_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdSetAPConfig ,
		.command_handler = cmd_set_ap_config_handler
	},
	{
		.cmd_num =  SLAVE_CONFIG_MSG_TYPE__TypeCmdGetSoftAPConfig ,
		.command_handler = cmd_get_softap_config_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdSetSoftAPConfig ,
		.command_handler = cmd_set_softap_config_handler
	},
	{
		.cmd_num =  SLAVE_CONFIG_MSG_TYPE__TypeCmdDisconnectAP ,
		.command_handler = cmd_disconnect_ap_handler
	},
	{
		.cmd_num = SLAVE_CONFIG_MSG_TYPE__TypeCmdGetAPScanList ,
		.command_handler = cmd_get_ap_scan_list_handler
	},
};


static int lookup_cmd_handler(int cmd_id)
{
    for (int i = 0; i < sizeof(cmd_table)/sizeof(slave_config_cmd_t); i++) {
        if (cmd_table[i].cmd_num == cmd_id) {
            return i;
        }
    }
    return -1;
}

static esp_err_t slave_config_command_dispatcher(SlaveConfigPayload *req, SlaveConfigPayload *resp, void *priv_data)
{
	esp_err_t ret;
	ESP_LOGI(TAG, "Inside command Dispatcher");
	int cmd_index = lookup_cmd_handler(req->msg);
	if (cmd_index < 0) {
		ESP_LOGE(TAG, "Invalid command handler lookup");
		return ESP_FAIL;
	}

	ret = cmd_table[cmd_index].command_handler(req, resp, priv_data);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Error executing command handler");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static void slave_config_cleanup(SlaveConfigPayload *resp)
{
	if (!resp) {
		return;
	}
	switch (resp->msg) {
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespGetMACAddress ) : {
			if (resp->resp_get_mac_address) {
				free(resp->resp_get_mac_address->resp);
				free(resp->resp_get_mac_address);
				ESP_LOGI(TAG,"resp get mac address freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespGetWiFiMode) : {
			if (resp->resp_get_wifi_mode) {
				free(resp->resp_get_wifi_mode);
				ESP_LOGI(TAG,"resp get wifi mode freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespSetWiFiMode ) : {
			if (resp->resp_set_wifi_mode) {
				free(resp->resp_set_wifi_mode);
				ESP_LOGI(TAG,"resp set wifi mode freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespGetAPConfig ) : {
			if (resp->resp_get_ap_config) {
				free(resp->resp_get_ap_config);
				memset(&credentials,0,sizeof(credentials_t));
				ESP_LOGI(TAG,"resp get ap config freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespSetAPConfig ) : {
			if (resp->resp_set_ap_config) {
				free(resp->resp_set_ap_config);
				ESP_LOGI(TAG,"resp set ap config");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespGetSoftAPConfig ) : {
			if (resp->resp_get_softap_config) {
				free(resp->resp_get_softap_config);
				memset(&credentials,0,sizeof(credentials_t));
				ESP_LOGI(TAG,"resp get sta config freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespSetSoftAPConfig ) : {
			if (resp->resp_set_softap_config) {
				free(resp->resp_set_softap_config);
				ESP_LOGI(TAG,"resp set softap config freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespDisconnectAP ) : {
			if (resp->resp_disconnect_ap) {
				free(resp->resp_disconnect_ap);
				ESP_LOGI(TAG, "resp disconnect ap freed");
			}
		}
		break;
		case (SLAVE_CONFIG_MSG_TYPE__TypeRespGetAPScanList) : {
			if (resp->resp_scan_ap_list) {
				if (resp->resp_scan_ap_list->entries) {
					for (int i=0 ; i<resp->resp_scan_ap_list->n_entries; i++) {
						if (resp->resp_scan_ap_list->entries[i]) {
							free(resp->resp_scan_ap_list->entries[i]->ssid.data);
							free(resp->resp_scan_ap_list->entries[i]->bssid.data);
							free(resp->resp_scan_ap_list->entries[i]);
						}
					}
					free(resp->resp_scan_ap_list->entries);
				}
				free(resp->resp_scan_ap_list);
				memset(&credentials,0,sizeof(credentials_t));
				ESP_LOGI(TAG, "resp get AP Scan List freed");
			}
		}
		break;
		default:
			ESP_LOGE(TAG, "Unsupported response type");
			break;
		}
	return;
}

esp_err_t data_transfer_handler(uint32_t session_id,const uint8_t *inbuf, ssize_t inlen,uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
	SlaveConfigPayload *req;
	SlaveConfigPayload resp;

	esp_err_t ret = ESP_OK;
	ESP_LOGI(TAG,"Hello inside transfer handler");
	if (inbuf == NULL || outbuf == NULL || outlen == NULL) {
		ESP_LOGE(TAG,"buffers are NULL");
		return ESP_FAIL;
	}

	req = slave_config_payload__unpack(NULL, inlen, inbuf);
	if (!req) {
		ESP_LOGE(TAG, "unable to unpack config data");
		return ESP_FAIL;
	}

	slave_config_payload__init (&resp);
	ret = slave_config_command_dispatcher(req,&resp,NULL);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "command dispatching no happening");
		return ESP_FAIL;
	}
	slave_config_payload__free_unpacked(req, NULL);
	resp.has_msg = 1;
	resp.msg = req->msg + 1;
	*outlen = slave_config_payload__get_packed_size (&resp);
	ESP_LOGI(TAG,"outlen %d ",*outlen);
	if (*outlen <= 0) {
		ESP_LOGE(TAG, "Invalid encoding for response");
		return ESP_FAIL;
	}
	*outbuf = (uint8_t *)calloc(1,*outlen);
	if (!*outbuf) {
		ESP_LOGE(TAG, "No memory allocated for outbuf");
		return ESP_ERR_NO_MEM;
	}
	slave_config_payload__pack (&resp, *outbuf);
	slave_config_cleanup(&resp);
	return ESP_OK;
}
