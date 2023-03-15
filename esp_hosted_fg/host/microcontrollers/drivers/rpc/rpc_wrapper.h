// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/** prevent recursive inclusion **/
#ifndef __RPC_WRAPPER_H__
#define __RPC_WRAPPER_H__

#ifdef __cplusplus
extern "C" {
#endif

/** Includes **/
#include "common.h"

/** constants/macros **/
typedef enum {
	MODE_NULL    = 0x0,
	MODE_STATION = 0x1,
	MODE_SOFTAP  = 0x2,
	MODE_SOFTAP_STATION = (MODE_STATION|MODE_SOFTAP),
	MODE_MAX
} operating_mode;

typedef enum control_path_events_s {
	STATION_CONNECTED,
	STATION_DISCONNECTED,
	SOFTAP_STARTED,
	SOFTAP_STOPPED
} control_path_events_e;


/** Exported variables **/


/** Inline functions **/

/** Exported Functions **/
void control_path_init(void(*control_path_evt_handler)(uint8_t));
stm_ret_t get_self_ip_station(uint32_t *self_ip);
stm_ret_t get_self_ip_softap(uint32_t *self_ip);
uint8_t *get_self_mac_station();
uint8_t *get_self_mac_softap();
stm_ret_t get_arp_dst_ip_station(uint32_t *sta_ip);
stm_ret_t get_arp_dst_ip_softap(uint32_t *soft_ip);

int test_set_wifi_mode(int mode);
int test_get_available_wifi(void);
int test_station_mode_get_mac_addr(char *mac);
int test_station_mode_connect(char *ssid, char *pwd, char *bssid,
	int is_wpa3_supported, int listen_interval);
int test_softap_mode_get_mac_addr(char *mac);
int test_softap_mode_start(char *ssid, char *pwd, int channel,
	int encryption_mode, int max_conn, int ssid_hidden, int bw);
int unregister_event_callbacks(void);
int register_event_callbacks(void);

int test_wifi_init(const wifi_init_config_t *arg);
int test_wifi_deinit(void);
int test_wifi_set_mode(int mode);
int test_wifi_get_mode(int* mode);
int test_wifi_start(void);
int test_wifi_stop(void);
int test_wifi_connect(void);
int test_wifi_disconnect(void);
int test_wifi_set_config(int interface, wifi_config_t *conf);
int test_wifi_get_config(int interface, wifi_config_t *conf);

#ifdef __cplusplus
}
#endif

#endif