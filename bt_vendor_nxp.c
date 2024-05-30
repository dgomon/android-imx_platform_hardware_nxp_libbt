/******************************************************************************
 *  Copyright 2012 The Android Open Source Project
 *  Portions copyright (C) 2009-2012 Broadcom Corporation
 *  Portions copyright 2012-2013, 2015, 2018-2023 NXP
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  Filename:      bt_vendor_nxp.c
 *
 *  Description:   NXP vendor specific library exported functions with
 *                 configuration file processing function
 *
 ******************************************************************************/

#define LOG_TAG "bt-vnd-nxp"

/*============================== Include Files ===============================*/

#include <ctype.h>
#include <cutils/properties.h>
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef FW_LOADER_V2
#include "fw_loader_uart_v2.h"
#else
#include "fw_loader_uart.h"
#endif
#include <limits.h>
#include <linux/gpio.h>

#include "bt_vendor_log.h"
#include "bt_vendor_nxp.h"
#include "fw_loader_io.h"
/*================================== Macros ==================================*/
/*[NK] @NXP - Driver FIX
  ioctl command to release the read thread before driver close */

#define MBTCHAR_IOCTL_RELEASE _IO('M', 1)

/*
 * Defines for wait for Bluetooth firmware ready Specify durations
 * between polls and max wait time
 */
#define POLL_DRIVER_DURATION_US (100000U)
#define POLL_DRIVER_MAX_TIME_MS (20000U)
#define POLL_CONFIG_UART_MS (10U)
#define POLL_RETRY_TIMEOUT_MS (1U)
#define POLL_MAX_TIMEOUT_MS (1000U)

#define CONF_COMMENT '#'
#define CONF_DELIMITERS " =\n\r\t"
#define CONF_VALUES_DELIMITERS "=\n\r\t"
#define CONF_MAX_LINE_LEN 1024
#define UNUSED(x) (void)(x)
#define BD_ADDR_LEN 6
#define BD_STR_ADDR_LEN 17

#define MCHAR_PORT_PROP "ro.boot.bluetooth.mchar_port"
/*================================== Typedefs ================================*/

typedef int(conf_action_t)(char* p_conf_name, char* p_conf_value,
                           void* p_conf_var, int param);

typedef struct {
  const char* conf_entry;
  conf_action_t* p_action;
  void* p_conf_var;
  int param;
} conf_entry_t;

/*============================ Function Prototypes ===========================*/
static int8_t send_hci_reset(void);

/*================================ Variables =================================*/
int mchar_fd = 0;
uint32_t vhal_trace_level = BT_TRACE_LEVEL_INFO;
static struct termios ti;

unsigned char* bdaddr = NULL;
const bt_vendor_callbacks_t* vnd_cb = NULL;
/* for NXP USB/SD interface */
static char mbt_port[MAX_PATH_LEN] = "/dev/mbtchar0";
/* for NXP Uart interface */
static char mchar_port[MAX_PATH_LEN] = "/dev/ttyUSB0";
static int is_uart_port = 0;
static uint32_t baudrate_fw_init = 115200;
static uint32_t baudrate_bt = 3000000;
int write_bdaddrss = 0;
int8_t ble_1m_power = 0;
int8_t ble_2m_power = 0;
uint8_t set_1m_2m_power = 0;
uint8_t bt_max_power_sel = 0;
uint8_t bt_set_max_power = 0;
uint8_t independent_reset_gpio_pin = 0xFF;
bool enable_sco_config = true;
bool enable_pdn_recovery = false;
bool use_controller_addr = true;
static uint8_t ir_host_gpio_pin = 14;
static char chrdev_name[32] = "/dev/gpiochip5";
/* 0:disable IR(default); 1:OOB IR(FW Config); 2:Inband IR;*/
uint8_t independent_reset_mode = IR_MODE_NONE;
/* 0:disable OOB IR Trigger; 1:RFKILL Trigger; 2:GPIO Trigger;*/
static uint8_t send_oob_ir_trigger = IR_TRIGGER_NONE;
bool enable_heartbeat_config = false;
bool wakeup_enable_uart_low_config = false;
char pFilename_fw_init_config_bin[MAX_PATH_LEN];
uint8_t write_bd_address[WRITE_BD_ADDRESS_SIZE] = {
    0xFE, /* Parameter ID */
    0x06, /* bd_addr length */
    0x00, /* 6th byte of bd_addr */
    0x00, /* 5th */
    0x00, /* 4th */
    0x00, /* 3rd */
    0x00, /* 2nd */
    0x00  /* 1st */
};
/*Low power mode LPM enables controller to go in sleep mode, this command
 * enables host to controller sleep(H2C)*/
static bool enable_lpm = false;
bool lpm_configured = false;
static uint32_t lpm_timeout_ms = 1000;
#ifdef UART_DOWNLOAD_FW
static bool enable_download_fw = false;
static uint32_t uart_sleep_after_dl = 0;
static int download_helper = 0;
static bool auto_select_fw_name = true;
static uint32_t baudrate_dl_helper = 115200;
static uint32_t baudrate_dl_image = 3000000;
static char pFileName_helper[MAX_PATH_LEN] =
    "/vendor/firmware/helper_uart_3000000.bin";
static char pFileName_image[MAX_PATH_LEN] =
    "/vendor/firmware/uart8997_bt_v4.bin";
static uint32_t iSecondBaudrate = 0;
uint8_t enable_poke_controller = 0;
static bool send_boot_sleep_trigger = false;
#endif
char pFilename_cal_data[MAX_PATH_LEN];
static int rfkill_id = -1;
static char* rfkill_state_path = NULL;
static uint32_t last_baudrate = 0;
wakeup_gpio_config_t wakeup_gpio_config[wakeup_key_num] = {
    {.gpio_pin = 13, .high_duration = 2, .low_duration = 2},
    {.gpio_pin = 13, .high_duration = 4, .low_duration = 4}};
wakeup_adv_pattern_config_t wakeup_adv_config = {.length = 0};
wakeup_scan_param_config_t wakeup_scan_param_config = {.le_scan_type = 0,
                                                       .interval = 128,
                                                       .window = 96,
                                                       .own_addr_type = 0,
                                                       .scan_filter_policy = 0};
wakeup_local_param_config_t wakeup_local_param_config = {
    .heartbeat_timer_value = 8};

/*============================== Coded Procedures ============================*/

static uint32_t str_to_uint32_t(const char* str) {
  unsigned long temp = strtoul(str, NULL, 10);
  uint32_t ret = 0;
  if (temp <= UINT_MAX) {
    ret = (uint32_t)temp;
  } else {
    ALOGE("Error while processing conf parameter");
  }
  return ret;
}

static uint8_t str_to_uint8_t(const char* str) {
  unsigned long temp = strtoul(str, NULL, 10);
  uint8_t ret = 0;
  if (temp <= UCHAR_MAX) {
    ret = (uint8_t)temp;
  } else {
    ALOGE("Error while processing conf parameter");
  }
  return ret;
}

static int8_t str_to_int8_t(const char* str) {
  long temp = strtol(str, NULL, 10);
  int8_t ret = 0;
  if (temp <= CHAR_MAX) {
    ret = (int8_t)temp;
  } else {
    ALOGE("Error while processing conf parameter");
  }
  return ret;
}

static uint16_t str_to_uint16_t(const char* str) {
  unsigned long temp = strtoul(str, NULL, 10);
  uint16_t ret = 0;
  if (temp <= USHRT_MAX) {
    ret = (uint16_t)temp;
  } else {
    ALOGE("Error while processing conf parameter");
  }
  return ret;
}

static int set_param_bool(char* p_conf_name, char* p_conf_value,
                          void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  *((bool*)p_conf_var) = (str_to_uint32_t(p_conf_value) == 0) ? false : true;
  return 0;
}

static int set_param_int8(char* p_conf_name, char* p_conf_value,
                          void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  *((int8_t*)p_conf_var) = str_to_int8_t(p_conf_value);
  return 0;
}

static int set_param_uint8(char* p_conf_name, char* p_conf_value,
                           void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  *((uint8_t*)p_conf_var) = str_to_uint8_t(p_conf_value);
  return 0;
}

static int set_param_uint16(char* p_conf_name, char* p_conf_value,
                            void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  *((uint16_t*)p_conf_var) = str_to_uint16_t(p_conf_value);
  return 0;
}

static int set_param_uint32(char* p_conf_name, char* p_conf_value,
                            void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  *((uint32_t*)p_conf_var) = str_to_uint32_t(p_conf_value);
  return 0;
}

static int set_ble_1m_power(char* p_conf_name, char* p_conf_value,
                            void* p_conf_var, int param) {
  set_param_int8(p_conf_name, p_conf_value, p_conf_var, param);
  set_1m_2m_power |= BLE_SET_1M_POWER;
  return 0;
}

static int set_ble_2m_power(char* p_conf_name, char* p_conf_value,
                            void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  set_param_int8(p_conf_name, p_conf_value, p_conf_var, param);
  set_1m_2m_power |= BLE_SET_2M_POWER;
  return 0;
}

static int set_bt_tx_power(char* p_conf_name, char* p_conf_value,
                           void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  set_param_uint8(p_conf_name, p_conf_value, p_conf_var, param);
  bt_set_max_power = 1;
  return 0;
}

static int set_param_string(char* p_conf_name, char* p_conf_value,
                            void* p_conf_var, int param) {
  size_t len_p_conf_value = 0;
  UNUSED(p_conf_name);
  UNUSED(param);

  len_p_conf_value = strnlen(p_conf_value, MAX_PATH_LEN);
  if (len_p_conf_value < MAX_PATH_LEN) {
    (void)strlcpy((char*)p_conf_var, p_conf_value, len_p_conf_value + 1U);
  } else {
    VND_LOGE("String length too long, unable to process");
    VND_LOGE("Source length: %zu and destination size: %lu", len_p_conf_value,
             MAX_PATH_LEN - 1U);
  }

  return 0;
}

static int set_mchar_port(char* p_conf_name, char* p_conf_value,
                          void* p_conf_var, int param) {
  set_param_string(p_conf_name, p_conf_value, p_conf_var, param);
  property_get(MCHAR_PORT_PROP, mchar_port, p_conf_value);
  VND_LOGI("the mchar_port is %s", mchar_port);
  is_uart_port = 1;
  return 0;
}

static int set_bd_address_buf(char* p_conf_name, char* p_conf_value,
                              void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  size_t i = 0;
  int j = 7;
  size_t len = 0;
  if (p_conf_value == NULL) {
    return 0;
  }
  len = strnlen(p_conf_value, BD_STR_ADDR_LEN + 1U);
  if (len != BD_STR_ADDR_LEN) {
    VND_LOGE("Invalid string length, unable to process");
    VND_LOGE("Source length: %zu and expected length: %u", len,
             BD_STR_ADDR_LEN);
    return 0;
  }
  for (i = 0; i < BD_STR_ADDR_LEN; i++) {
    if (((i + 1) % 3) == 0 && p_conf_value[i] != ':') {
      return 0;
    }
    if (((i + 1) % 3) != 0 && !isxdigit(p_conf_value[i])) {
      return 0;
    }
    char tmp = p_conf_value[i];
    if (isupper((int)(p_conf_value[i]))) {
      p_conf_value[i] = (char)(p_conf_value[i] - 'A' + 10);
    } else if (islower((int)(p_conf_value[i]))) {
      p_conf_value[i] = (char)(p_conf_value[i] - 'a' + 10);
    } else if (isdigit((int)(p_conf_value[i]))) {
      p_conf_value[i] = (char)(p_conf_value[i] - '0');
    } else if (p_conf_value[i] == ':') {
      p_conf_value[i] = tmp;
    } else {
      return 0;
    }
  }
  for (i = 0; i < BD_STR_ADDR_LEN; i = i + 3) {
    ((uint8_t*)p_conf_var)[j--] =
        (uint8_t)((p_conf_value[i] << 4) | p_conf_value[i + 1]);
  }
  write_bdaddrss = 1;
  return 0;
}

#ifdef UART_DOWNLOAD_FW

static int set_pFileName_image(char* p_conf_name, char* p_conf_value,
                               void* p_conf_var, int param) {
  set_param_string(p_conf_name, p_conf_value, p_conf_var, param);
  auto_select_fw_name = false;
  return 0;
}

static int set_pFileName_helper(char* p_conf_name, char* p_conf_value,
                                void* p_conf_var, int param) {
  set_param_string(p_conf_name, p_conf_value, p_conf_var, param);
  download_helper = 1;
  return 0;
}

#endif

static int set_wakeup_adv_pattern(char* p_conf_name, char* p_conf_value,
                                  void* p_conf_var, int param) {
  UNUSED(p_conf_name);
  UNUSED(param);
  UNUSED(p_conf_var);
  if (wakeup_adv_config.length >= NXP_WAKEUP_ADV_PATTERN_LENGTH) {
    VND_LOGE("%s, invalid length:%d max limit %d", __func__,
             (int32)wakeup_adv_config.length,
             NXP_WAKEUP_ADV_PATTERN_LENGTH - 1);
    return -1;
  }
  wakeup_adv_config.adv_pattern[wakeup_adv_config.length] =
      str_to_uint8_t(p_conf_value);
  wakeup_adv_config.length += 1;
  return 0;
}

/*
 * Current supported entries and corresponding action functions
 */

static const conf_entry_t conf_table[] = {
    {"mchar_port", set_mchar_port, &mchar_port, 0},
    {"mbt_port", set_param_string, &mbt_port, 0},
    {"baudrate_bt", set_param_uint32, &baudrate_bt, 0},
    {"baudrate_fw_init", set_param_uint32, &baudrate_fw_init, 0},
    {"bd_address", set_bd_address_buf, &write_bd_address, 0},
    {"pFilename_fw_init_config_bin", set_param_string,
     &pFilename_fw_init_config_bin, 0},
    {"ble_1m_power", set_ble_1m_power, &set_1m_2m_power, 0},
    {"ble_2m_power", set_ble_2m_power, &ble_2m_power, 0},
    {"bt_max_power_sel", set_bt_tx_power, &bt_max_power_sel, 0},
    {"independent_reset_gpio_pin", set_param_uint8, &independent_reset_gpio_pin,
     0},
    {"oob_ir_host_gpio_pin", set_param_uint8, &ir_host_gpio_pin, 0},
    {"chardev_name", set_param_string, &chrdev_name, 0},
    {"independent_reset_mode", set_param_uint8, &independent_reset_mode, 0},
    {"send_oob_ir_trigger", set_param_uint8, &send_oob_ir_trigger, 0},
    {"enable_lpm", set_param_bool, &enable_lpm, 0},
    {"lpm_timeout", set_param_bool, &lpm_timeout_ms, 0},

#ifdef UART_DOWNLOAD_FW
    {"enable_download_fw", set_param_bool, &enable_download_fw, 0},
    {"pFileName_image", set_pFileName_image, &pFileName_image, 0},
    {"pFileName_helper", set_pFileName_helper, &pFileName_helper, 0},
    {"baudrate_dl_helper", set_param_uint32, &baudrate_dl_helper, 0},
    {"baudrate_dl_image", set_param_uint32, &baudrate_dl_image, 0},
    {"iSecondBaudrate", set_param_uint32, &iSecondBaudrate, 0},
    {"uart_sleep_after_dl", set_param_uint32, &uart_sleep_after_dl, 0},
    {"enable_poke_controller", set_param_uint8, &enable_poke_controller, 0},
    {"send_boot_sleep_trigger", set_param_bool, &send_boot_sleep_trigger, 0},
#endif
    {"enable_pdn_recovery", set_param_bool, &enable_pdn_recovery, 0},
    {"pFilename_cal_data", set_param_string, &pFilename_cal_data, 0},
    {"vhal_trace_level", set_param_uint32, &vhal_trace_level, 0},
    {"enable_sco_config", set_param_bool, &enable_sco_config, 0},
    {"use_controller_addr", set_param_bool, &use_controller_addr, 0},
    {"enable_heartbeat_config", set_param_bool, &enable_heartbeat_config, 0},
    {"wakeup_power_gpio_pin", set_param_uint8,
     &wakeup_gpio_config[wakeup_power_key].gpio_pin, 0},
    {"wakeup_enable_uart_low_config", set_param_bool,
     &wakeup_enable_uart_low_config, 0},
    {"wakeup_power_gpio_high_duration", set_param_uint8,
     &wakeup_gpio_config[wakeup_power_key].high_duration, 0},
    {"wakeup_power_gpio_low_duration", set_param_uint8,
     &wakeup_gpio_config[wakeup_power_key].low_duration, 0},
    {"wakeup_netflix_gpio_pin", set_param_uint8,
     &wakeup_gpio_config[wakeup_netflix_key].gpio_pin, 0},
    {"wakeup_netflix_gpio_high_duration", set_param_uint8,
     &wakeup_gpio_config[wakeup_netflix_key].high_duration, 0},
    {"wakeup_netflix_gpio_low_duration", set_param_uint8,
     &wakeup_gpio_config[wakeup_netflix_key].low_duration, 0},
    {"wakeup_adv_pattern", set_wakeup_adv_pattern, NULL, 0},
    {"wakeup_scan_type", set_param_uint8,
     &wakeup_scan_param_config.le_scan_type, 0},
    {"wakeup_scan_interval", set_param_uint16,
     &wakeup_scan_param_config.interval, 0},
    {"wakeup_scan_window", set_param_uint16, &wakeup_scan_param_config.window,
     0},
    {"wakeup_own_addr_type", set_param_uint8,
     &wakeup_scan_param_config.own_addr_type, 0},
    {"wakeup_scan_filter_policy", set_param_uint8,
     &wakeup_scan_param_config.scan_filter_policy, 0},
    {"wakeup_local_heartbeat_timer_value", set_param_uint8,
     &wakeup_local_param_config.heartbeat_timer_value, 0},
    {(const char*)NULL, NULL, NULL, 0}};

/*******************************************************************************
**
** Function        vnd_load_conf
**
** Description     Read conf file from mentioned p_path at run time and read
**                 one by one entry and call the corresponding config function
**
** Returns         None
**
*******************************************************************************/
static void vnd_load_conf(const char* p_path) {
  FILE* p_file = NULL;
  char* p_name;
  char* p_value;
  const conf_entry_t* p_entry;
  char line[CONF_MAX_LINE_LEN + 1]; /* add 1 for \0 char */

  VND_LOGD("Attempt to load conf from %s", p_path);
  p_file = fopen(p_path, "r");
  if (p_file != NULL) {
    /* read line by line */
    while (fgets(line, CONF_MAX_LINE_LEN + 1, p_file) != NULL) {
      if (line[0] == CONF_COMMENT) continue;

      p_name = strtok(line, CONF_DELIMITERS);

      if (NULL == p_name) {
        continue;
      }

      p_value = strtok(NULL, CONF_DELIMITERS);

      if (NULL == p_value) {
        ALOGW("vnd_load_conf: missing value for name: %s", p_name);
        continue;
      }

      p_entry = (conf_entry_t*)conf_table;

      while (p_entry->conf_entry != NULL) {
        if (strncmp(p_entry->conf_entry, (const char*)p_name,
                    MAX_CONF_PARA_LEN) == 0) {
          p_entry->p_action(p_name, p_value, p_entry->p_conf_var,
                            p_entry->param);
          break;
        }

        p_entry++;
      }
    }

    fclose(p_file);
  } else {
    VND_LOGE("File open error at %s", p_path);
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
  }
}

/******************************************************************************
 **
 ** Function:        read_hci_event
 **
 ** Description:     Reads hci event.
 **
 ** Return Value:    0 is successful, -1 otherwise
 **
 *
 *****************************************************************************/

static int read_hci_event(hci_event* evt_pkt, uint64_t retry_delay_ms,
                          uint32_t max_duration_ms) {
  ssize_t r;
  uint8_t count, remain;
  uint32_t total_duration = 0;

  /* The first byte identifies the packet type. For HCI event packets, it
   * should be 0x04, so we read until we get to the 0x04. */
  VND_LOGV("start read hci event 0x4");
  count = 0;
  while (fw_upload_GetBufferSize(mchar_fd) <
         HCI_EVENT_HEADER_SIZE + HCI_PACKET_TYPE_SIZE) {
    usleep((useconds_t)(retry_delay_ms * 1000));
    total_duration += (uint32_t)retry_delay_ms;
    if (total_duration >= max_duration_ms) {
      VND_LOGE("Read hci complete event failed timed out. Total_duration = %u",
               total_duration);
      return -1;
    }
  };
  r = read(mchar_fd, evt_pkt->raw_data,
           HCI_EVENT_HEADER_SIZE + HCI_PACKET_TYPE_SIZE);
  if (r <= 0) {
    VND_LOGV("read hci event 0x04 failed");
    VND_LOGV("Error %s (%d)", strerror(errno), errno);
  }
  if (evt_pkt->info.packet_type != HCI_PACKET_EVENT) {
    VND_LOGE("Invalid packet type(%02X) received", evt_pkt->info.packet_type);
    return -1;
  }
  /* Now we read the parameters. */
  VND_LOGV("start read hci event para");
  if (evt_pkt->info.para_len < HCI_EVENT_PAYLOAD_SIZE) {
    remain = evt_pkt->info.para_len;
  } else {
    remain = HCI_EVENT_PAYLOAD_SIZE;
    VND_LOGE("Payload size(%d) greater than capacity", evt_pkt->info.para_len);
  }
  while (fw_upload_GetBufferSize(mchar_fd) < (uint32_t)remain)
    ;
  while ((count) < remain) {
    r = read(mchar_fd, evt_pkt->info.payload + count, remain - (count));
    if (r <= 0) {
      VND_LOGE("read hci event para failed");
      VND_LOGE("Error: %s (%d)", strerror(errno), errno);
      return -1;
    }
    count += r;
  }
  return 0;
}

/******************************************************************************
 **
 ** Function:        check_hci_event_status
 **
 ** Description:     Parse evt_pkt for opcode.
 **
 ** Return Value:    0 is successful, -1 otherwise
 **
 *
 *****************************************************************************/
static int8_t check_hci_event_status(hci_event* evt_pkt, uint16_t opcode) {
  int8_t ret = -1;
  uint8_t* ptr;
  uint16_t pkt_opcode;
  switch (evt_pkt->info.event_type) {
    case HCI_EVENT_COMMAND_COMPLETE:
      if (evt_pkt->info.para_len > HCI_EVT_PYLD_STATUS_IDX) {
        ptr = &evt_pkt->info.payload[HCI_EVT_PYLD_OPCODE_IDX];
        STREAM_TO_UINT16(pkt_opcode, ptr);
        VND_LOGD("Reply received for command 0x%04hX (%s) status 0x%02x",
                 pkt_opcode, hw_bt_cmd_to_str(pkt_opcode),
                 evt_pkt->info.payload[HCI_EVT_PYLD_STATUS_IDX]);
        if (evt_pkt->info.payload[HCI_EVT_PYLD_STATUS_IDX] != 0) {
          VND_LOGE(
              "Error status received for command 0x%04hX (%s) status 0x%02x",
              pkt_opcode, hw_bt_cmd_to_str(pkt_opcode),
              evt_pkt->info.payload[HCI_EVT_PYLD_STATUS_IDX]);
        }
        if (pkt_opcode == opcode) ret = 0;
      } else {
        VND_LOGE("Unexpected packet length received. Event type:%02x Len:%02x",
                 evt_pkt->info.event_type, evt_pkt->info.para_len);
      }
      break;
    case HCI_EVENT_HARDWARE_ERROR:
      VND_LOGE("Hardware error event(%02x) received ",
               evt_pkt->info.event_type);
      VND_LOGE("Payload length received %02x", evt_pkt->info.para_len);
      if (evt_pkt->info.para_len > 0) {
        VND_LOGE("Hardware error code: %02x", evt_pkt->info.payload[0]);
      }
      break;
    default:
      VND_LOGE("Invalid Event type %02x received ", evt_pkt->info.event_type);
      break;
  }
  VND_LOGV("Packet dump");
  for (uint8_t i = 0; i < evt_pkt->info.para_len +
                              (HCI_PACKET_TYPE_SIZE + HCI_EVENT_HEADER_SIZE);
       i++) {
    VND_LOGV("Packet[%d]= %02x", i, evt_pkt->raw_data[i]);
  }
  if (evt_pkt->info.event_type == HCI_EVENT_HARDWARE_ERROR) {
    /*BLUETOOTH CORE SPECIFICATION Version 5.4 | Vol 4, Part A Point 4*/
    ret = send_hci_reset();
  }
  return ret;
}

/******************************************************************************
 **
 ** Function:        read_hci_event_status
 **
 ** Description:     Polls HCI_EVENT with opcode till max_duration_ms and checks
 *                   its status.
 **
 ** Return Value:    0 is successful, -1 otherwise
 **
 *
 *****************************************************************************/
static int8_t read_hci_event_status(uint16_t opcode, uint64_t retry_delay_ms,
                                    uint64_t max_duration_ms) {
  int8_t ret = -1;
  hci_event evt_pkt;
  memset(&evt_pkt, 0x00, sizeof(evt_pkt));
  uint64_t start_ms = fw_upload_GetTime();
  uint64_t cost_ms = 0;
  uint64_t remaining_time_ms = max_duration_ms;
  int read_hci_flag;
  VND_LOGD("Reading %s event", hw_bt_cmd_to_str(opcode));
  read_hci_flag =
      read_hci_event(&evt_pkt, retry_delay_ms, (uint32_t)remaining_time_ms);
  while ((cost_ms < max_duration_ms) && (read_hci_flag == 0)) {
    ret = check_hci_event_status(&evt_pkt, opcode);
    if (ret == 0) {
      break;
    }
    remaining_time_ms = max_duration_ms - (fw_upload_GetTime() - start_ms);
    read_hci_flag =
        read_hci_event(&evt_pkt, retry_delay_ms, (uint32_t)remaining_time_ms);
    cost_ms = fw_upload_GetTime() - start_ms;
  }
  if (cost_ms >= max_duration_ms) {
    VND_LOGE("Read hci complete event failed, timed out at: %u",
             (uint32_t)cost_ms);
  }
  return ret;
}

/*******************************************************************************
**
** Function        send_hci_reset
**
** Description     Send HCI reset command over raw UART.
**
** Returns         0 on success, -1 on failure
**
*******************************************************************************/
static int8_t send_hci_reset(void) {
  int8_t ret = -1;
  if (hw_bt_send_hci_cmd_raw(HCI_CMD_NXP_RESET) != 0) {
    VND_LOGE("Failed to write reset command");
  } else if ((read_hci_event_status(HCI_CMD_NXP_RESET, POLL_CONFIG_UART_MS,
                                    POLL_MAX_TIMEOUT_MS) != 0)) {
    VND_LOGE("Failed to read HCI RESET CMD response!");
  } else {
    VND_LOGD("HCI reset completed successfully");
    ret = 0;
  }
  return ret;
}

void set_prop_int32(const char* name, int value) {
  char init_value[PROPERTY_VALUE_MAX];
  int ret;

  sprintf(init_value, "%d", value);
  ret = property_set(name, init_value);
  if (ret < 0) {
    VND_LOGE("set_prop_int32 failed: %d", ret);
  } else {
    VND_LOGD("set_prop_int32: %s = %d", name, value);
  }
  return;
}

int get_prop_int32(const char* name) {
  int ret;

  ret = property_get_int32(name, -1);
  VND_LOGD("get_prop_int32: %s = %d", name, ret);
  if (ret < 0) {
    return 0;
  }
  return ret;
}

/******************************************************************************
 **
 ** Function:        uart_speed
 **
 ** Description:     Return the baud rate corresponding to the frequency.
 **
 ** Return Value:    Baudrate
 *
 *****************************************************************************/

static uint32 uart_speed(uint32 s) {
  uint32 ret = 0;
  switch (s) {
    case 9600U:
      ret = B9600;
      break;
    case 19200U:
      ret = B19200;
      break;
    case 38400U:
      ret = B38400;
      break;
    case 57600U:
      ret = B57600;
      break;
    case 115200U:
      ret = B115200;
      break;
    case 230400U:
      ret = B230400;
      break;
    case 460800U:
      ret = B460800;
      break;
    case 500000U:
      ret = B500000;
      break;
    case 576000U:
      ret = B576000;
      break;
    case 921600U:
      ret = B921600;
      break;
    case 1000000U:
      ret = B1000000;
      break;
    case 1152000U:
      ret = B1152000;
      break;
    case 1500000U:
      ret = B1500000;
      break;
    case 3000000U:
      ret = B3000000;
      break;
    default:
      ret = B0;
      break;
  }
  return ret;
}
/******************************************************************************
 **
 ** Function:        uart_speed_translate
 **
 ** Description:     Return the frequency to corresponding baud rate.
 **
 ** Return Value:    Baudrate
 *
 *****************************************************************************/

static uint32 uart_speed_translate(uint32 s) {
  uint32 ret = 0;
  switch (s) {
    case B9600:
      ret = 9600U;
      break;
    case B19200:
      ret = 19200U;
      break;
    case B38400:
      ret = 38400U;
      break;
    case B57600:
      ret = 57600U;
      break;
    case B115200:
      ret = 115200U;
      break;
    case B230400:
      ret = 230400U;
      break;
    case B460800:
      ret = 460800U;
      break;
    case B500000:
      ret = 500000U;
      break;
    case B576000:
      ret = 576000U;
      break;
    case B921600:
      ret = 921600U;
      break;
    case B1000000:
      ret = 1000000U;
      break;
    case B1152000:
      ret = 1152000U;
      break;
    case B1500000:
      ret = 1500000U;
      break;
    case B3000000:
      ret = 3000000U;
      break;
    default:
      ret = 0;
      break;
  }
  return ret;
}

/******************************************************************************
 *
 ** Function            uart_get_speed
 **
 ** Description         Get the last baud rate speed.

 ** Return Value:       Value of last baud rate
 **
 *****************************************************************************/

static uint32_t uart_get_speed(struct termios* ter) {
  uint32 speed = 0;
  speed = cfgetospeed(ter);
  return uart_speed_translate(speed);
}
/******************************************************************************
 *
 ** Function            uart_set_speed
 **
 ** Description         Set the baud rate speed.

 ** Return Value:       0 On success else -1

 **
 *****************************************************************************/

static int32 uart_set_speed(int32 fd, struct termios* ter, uint32 speed) {
  if (cfsetospeed(ter, uart_speed(speed)) < 0) {
    VND_LOGE("Set O speed failed!");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    return -1;
  }

  if (cfsetispeed(ter, uart_speed(speed)) < 0) {
    VND_LOGE("Set I speed failed!");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    return -1;
  }

  if (tcsetattr(fd, TCSANOW, ter) < 0) {
    VND_LOGE("Set Attr speed failed!");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    return -1;
  }
  VND_LOGD("Host baudrate set to %d", speed);
  return 0;
}

/******************************************************************************
 **
 ** Name:               init_uart
 **
 ** Description         Initialize UART.
 **
 ** Return Value        Valid fd on success
 **
 *****************************************************************************/

int32 init_uart(int8* dev, uint32 dwBaudRate, uint8 ucFlowCtrl) {
  int32 fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    VND_LOGE("Can't open serial port");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    return -1;
  }

  tcflush(fd, TCIOFLUSH);

  if (tcgetattr(fd, &ti) < 0) {
    VND_LOGE("Can't get port settings");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    close(fd);
    return -1;
  }

  cfmakeraw(&ti);
#ifdef FW_LOADER_V2
  ti.c_cflag |= CLOCAL | CREAD;
#else
  ti.c_cflag |= CLOCAL;
#endif

  /* Set 1 stop bit & no parity (8-bit data already handled by cfmakeraw) */
  ti.c_cflag &= ~(CSTOPB | PARENB);

  if (ucFlowCtrl) {
    ti.c_cflag |= CRTSCTS;
  } else {
    ti.c_cflag &= ~CRTSCTS;
  }

  /*FOR READS: set timeout time w/ no minimum characters needed
                         (since we read only 1 at at time...)          */
  ti.c_cc[VMIN] = 0;
  ti.c_cc[VTIME] = TIMEOUT_SEC * 10;

  if (tcsetattr(fd, TCSANOW, &ti) < 0) {
    VND_LOGE("Can't set port settings");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);
  if (independent_reset_mode == IR_MODE_INBAND_VSC) {
    last_baudrate = uart_get_speed(&ti);
    VND_LOGD("Last baud rate = %d", last_baudrate);
  }
  /* Set actual baudrate */
  if (uart_set_speed(fd, &ti, dwBaudRate) < 0) {
    VND_LOGE("Can't set baud rate");
    VND_LOGE("Error: %s (%d)", strerror(errno), errno);
    close(fd);
    return -1;
  }

  return fd;
}

/*******************************************************************************
**
** Function        uart_init_open
**
** Description     Open the serial port with the given configuration
**
** Returns         device fd
**
*******************************************************************************/

static int uart_init_open(int8* dev, int32 dwBaudRate, uint8 ucFlowCtrl) {
  int fd = 0, num = 0;
  do {
    fd = init_uart(dev, dwBaudRate, ucFlowCtrl);
    if (fd < 0) {
      num++;
      if (num >= 8) {
        VND_LOGE("exceed max retry count, return error");
        return -1;
      } else {
        VND_LOGW("open uart port %s failed fd: %d, retrying", dev, fd);
        VND_LOGW("Error: %s (%d)", strerror(errno), errno);
        usleep(50 * 1000);
        continue;
      }
    }
  } while (fd < 0);

  return fd;
}
#ifdef UART_DOWNLOAD_FW
/*******************************************************************************
**
** Function        detect_and_download_fw
**
** Description     Start firmware download process if fw is not already download
**
** Returns         0 : FW is ready
**                 1 : FW not ready
**
*******************************************************************************/

static uint32 detect_and_download_fw() {
  uint32 download_ret = 1;
#ifndef FW_LOADER_V2
  init_crc8();
#endif
/* force download only when header is received */
#ifdef FW_LOADER_V2
  if (bt_vnd_mrvl_check_fw_status_v2()) {
#else
  if (bt_vnd_mrvl_check_fw_status()) {
#endif
#ifdef UART_DOWNLOAD_FW
    if (send_boot_sleep_trigger == true) {
      if (get_prop_int32(PROP_BLUETOOTH_BOOT_SLEEP_TRIGGER) == 0) {
        VND_LOGD("setting PROP_BLUETOOTH_BOOT_SLEEP_TRIGGER to 1");
        set_prop_int32(PROP_BLUETOOTH_BOOT_SLEEP_TRIGGER, 1);
      }
    }
#endif
    if (download_helper) {
#ifdef FW_LOADER_V2
      download_ret = bt_vnd_mrvl_download_fw_v2(mchar_port, baudrate_dl_helper,
                                                pFileName_helper);
#else
      download_ret = bt_vnd_mrvl_download_fw(mchar_port, baudrate_dl_helper,
                                             pFileName_helper, iSecondBaudrate);
#endif
      if (download_ret != 0) {
        VND_LOGE("helper download failed");
        goto done;
      }

      usleep(50000);
      /* flush additional A5 header if any */
      tcflush(mchar_fd, TCIFLUSH);

      /* close and open the port and set baud rate to baudrate_dl_image */
      close(mchar_fd);
      mchar_fd = uart_init_open(mchar_port, 3000000U, 1);
      if (mchar_fd < 0) {
        download_ret = 1;
        goto done;
      }
      usleep(20000);
      tcflush(mchar_fd, TCIOFLUSH);
    }

    /* download fw image */
    if (auto_select_fw_name == true) {
      fw_loader_get_default_fw_name(pFileName_image, sizeof(pFileName_image));
    }
#ifdef FW_LOADER_V2
    download_ret = bt_vnd_mrvl_download_fw_v2(mchar_port, baudrate_dl_image,
                                              pFileName_image);
#else
    download_ret = bt_vnd_mrvl_download_fw(mchar_port, baudrate_dl_image,
                                           pFileName_image, iSecondBaudrate);
#endif
    if (download_ret != 0) {
      VND_LOGE("fw download failed");
      goto done;
    }

    tcflush(mchar_fd, TCIFLUSH);
    if (uart_sleep_after_dl > 0) {
      usleep((useconds_t)(uart_sleep_after_dl * 1000));
    }
    if (enable_pdn_recovery) {
      ALOGI("%s:%d\n", PROP_VENDOR_TRIGGER_PDN,
            get_prop_int32(PROP_VENDOR_TRIGGER_PDN));
    }
  }
done:
  return download_ret;
}
#endif

/*******************************************************************************
**
** Function        config_uart
**
** Description     Configure uart w.r.t different fw_init_baudrate
**                 and bt_baudrate and send relevant HCI command to confirm
                   uart configuration
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/

static int config_uart() {
  if (baudrate_fw_init != baudrate_bt) {
    /* set baud rate to baudrate_fw_init */
    if (uart_set_speed(mchar_fd, &ti, baudrate_fw_init) < 0) {
      VND_LOGE("Can't set baud rate");
      return -1;
    }
    if (send_hci_reset() != 0) {
      return -1;
    }
    /* Set bt chip Baud rate CMD */
    if ((baudrate_bt == 3000000) || (baudrate_bt == 115200)) {
      VND_LOGD("set fw baudrate as %d", baudrate_bt);
      if (hw_send_change_baudrate_raw(baudrate_bt)) {
        VND_LOGE("Failed to write set baud rate command");
        return -1;
      }
      VND_LOGV("start read hci event");
      if (read_hci_event_status(HCI_CMD_NXP_CHANGE_BAUDRATE,
                                POLL_CONFIG_UART_MS,
                                POLL_MAX_TIMEOUT_MS) != 0) {
        VND_LOGE("Failed to read set baud rate command response! ");
        return -1;
      }
      VND_LOGD("Controller Baudrate changed successfully to %d", baudrate_bt);
    } else {
      VND_LOGD("Unsupported baudrate_bt %d", baudrate_bt);
    }

    usleep(60000); /* Sleep to allow baud rate setting to happen in FW */

    tcflush(mchar_fd, TCIOFLUSH);
    if (uart_set_speed(mchar_fd, &ti, baudrate_bt) != 0) {
      VND_LOGE("Failed to  set baud rate ");
      return -1;
    }
    ti.c_cflag |= CRTSCTS;
    if (tcsetattr(mchar_fd, TCSANOW, &ti) < 0) {
      VND_LOGE("Set Flow Control failed!");
      VND_LOGE("Error: %s (%d)", strerror(errno), errno);
      return -1;
    }
    tcflush(mchar_fd, TCIOFLUSH);
  } else {
    /* set host uart speed according to baudrate_bt */
    VND_LOGD("Set host baud rate as %d", baudrate_bt);
    tcflush(mchar_fd, TCIOFLUSH);

    /* Close and open the port as setting baudrate to baudrate_bt */
    close(mchar_fd);
    mchar_fd = uart_init_open(mchar_port, baudrate_bt, 1);
    if (mchar_fd < 0) {
      return -1;
    }
    usleep(20000);
    tcflush(mchar_fd, TCIOFLUSH);
  }

  usleep(20 * 1000);
  return 0;
}
static bool bt_vnd_is_rfkill_disabled(void) {
  char value[PROPERTY_VALUE_MAX] = {'\0'};
  bool ret = false;
  property_get("ro.rfkilldisabled", value, "0");
  VND_LOGD("ro.rfkilldisabled %c", value[0]);
  if (value[0] == (char)('1')) {
    ret = true;
  }
  return ret;
}

static int bt_vnd_init_rfkill() {
  char path[64];
  char buf[16];
  int fd = -1, id = 0, ret = -1;
  ssize_t sz = -1;  // initial
  if (bt_vnd_is_rfkill_disabled() == true) {
    ret = -1;
  } else {
    do {
      snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", id);
      fd = open(path, O_RDONLY);
      if (fd < 0) {
        VND_LOGE("open(%s) failed: %s (%d)", path, strerror(errno), errno);
        break;
      }
      sz = read(fd, &buf, sizeof(buf));
      if (sz < 0) {
        VND_LOGE("read failed: %s (%d)", strerror(errno), errno);
      }
      close(fd);
      if (sz >= 9 && (memcmp(buf, "bluetooth", 9U) == 0)) {
        rfkill_id = id;
        break;
      }
      id++;
    } while (fd >= 0);

    if (rfkill_id == -1) {
      VND_LOGE("Bluetooth rfkill not found");
    } else {
      asprintf(&rfkill_state_path, "/sys/class/rfkill/rfkill%d/state",
               rfkill_id);
      VND_LOGD("rfkill_state_path set to %s ", rfkill_state_path);
      ret = 0;
    }
  }
  return ret;
}

static int bt_vnd_set_bluetooth_power(bool bt_turn_on) {
  ssize_t sz;
  int fd = -1;
  int ret = -1;
  char buffer = bt_turn_on ? '1' : '0';
  /* check if we have rfkill interface */
  if (bt_vnd_is_rfkill_disabled() == true) {
    VND_LOGD("rfkill disabled, ignoring bluetooth power %s",
             (bt_turn_on == true) ? "ON" : "OFF");
    ret = 0;
    goto done;
  }

  if (rfkill_id == -1) {
    if (bt_vnd_init_rfkill() != 0) {
      goto done;
    }
  }

  fd = open(rfkill_state_path, O_WRONLY);
  if (fd < 0) {
    VND_LOGE("open(%s) for write failed: %s (%d)", rfkill_state_path,
             strerror(errno), errno);
    goto done;
  }
  sz = write(fd, &buffer, 1U);
  if (sz < 0) {
    VND_LOGE("write(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
  }
  ret = 0;

done:
  if (fd >= 0) {
    close(fd);
  }
  return ret;
}

static void bt_vnd_gpio_configuration(int value) {
  struct gpiohandle_request req;
  struct gpiohandle_data data;
  int fd = 0, ret = 0;

  /* Open device: gpiochip0 for GPIO bank A */
  fd = open(chrdev_name, 0);
  if (fd == -1) {
    VND_LOGW("Failed to open %s %s(%d)", chrdev_name, strerror(errno), errno);
    return;
  }
  /* Request GPIO Direction line as out */
  req.lineoffsets[0] = ir_host_gpio_pin;
  req.flags = GPIOHANDLE_REQUEST_OUTPUT;
  req.lines = 1;
  ret = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);

  if (ret == -1) {
    VND_LOGE("%s Failed to issue GET LINEHANDLE IOCTL(%d)", strerror(errno),
             errno);
    close(fd);
    return;
  }
  if (close(fd) == -1) {
    VND_LOGE("Failed to close GPIO character device file");
    return;
  }
  /* Get the value of line */
  memset(&data, 0, sizeof(data));
  ret = ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
  if (ret == -1) {
    close(req.fd);
    VND_LOGE("%s failed to issue GET LINEHANDLE(%d)", strerror(errno), errno);
    return;
  }
  VND_LOGD("Current Value of line=: %d", data.values[0]);

  /* Set the requested value to the line*/
  data.values[0] = value;
  ret = ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
  if (ret == -1) {
    close(req.fd);
    VND_LOGE("%s failed to issue SET LINEHANDLE", strerror(errno));
    return;
  }
  ret = ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
  if (ret < 0) {
    VND_LOGE("ioctl error: %s (%d)", strerror(errno), errno);
  }
  VND_LOGD("Updated Value of Line:= %d", data.values[0]);

  /*  release line */
  ret = close(req.fd);
  if (ret == -1) {
    VND_LOGW("%s Failed to close GPIO LINEHANDLE device file", strerror(errno));
    return;
  }
}

/*******************************************************************************
**
** Function        bt_vnd_send_inband_ir
**
** Description     Send Inband Independent Reset to Controller.
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
static int bt_vnd_send_inband_ir(uint32_t baudrate) {
  uint32_t _last_baudrate = last_baudrate;

  if (get_prop_int32(PROP_BLUETOOTH_INBAND_CONFIGURED) == 1) {
    close(mchar_fd);
    mchar_fd = uart_init_open(mchar_port, _last_baudrate, 1);
    if (mchar_fd <= 0) {
      VND_LOGE("Can't set last baud rate %d", _last_baudrate);
      return -1;
    } else {
      VND_LOGD("Baud rate changed from %u to %u with flow control enabled",
               baudrate, _last_baudrate);
    }
    tcflush(mchar_fd, TCIOFLUSH);
    if (hw_bt_send_hci_cmd_raw(HCI_CMD_INBAND_RESET)) {
      VND_LOGE("Failed to write in-band reset command ");
      VND_LOGE("Error: %s (%d)", strerror(errno), errno);
      return -1;
    } else {
      VND_LOGV("start read hci event");
      if (read_hci_event_status(HCI_CMD_INBAND_RESET, POLL_RETRY_TIMEOUT_MS,
                                POLL_MAX_TIMEOUT_MS) != 0) {
        VND_LOGE("Failed to read Inband reset response");
        return -1;
      }
      VND_LOGD("=========Inband IR trigger sent successfully=======");
    }
    close(mchar_fd);
    mchar_fd = uart_init_open(mchar_port, baudrate, 0);
    if (mchar_fd <= 0) {
      VND_LOGE("Can't set last baud rate %u", _last_baudrate);
      return -1;
    } else {
      VND_LOGD("Baud rate changed from %u to %u with flow control disabled",
               _last_baudrate, baudrate);
    }
  }
  return 0;
}

/*******************************************************************************
**
** Function        send_exit_heartbeat_mode
**
** Description     Exit heartbeat mode during bluetooth disabling procedure
**
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
static void send_exit_heartbeat_mode(void) {
  hci_event evt_pkt;
  bool heartbeatFlag = false;
  VND_LOGD("Start to send exit heartbeat cmd ...\n");
  memset(&evt_pkt, 0x00, sizeof(evt_pkt));

  if (hw_bt_send_wakeup_disable_raw()) {
    VND_LOGD("Failed to write exit heartbeat command \n");
    return;
  }
  if (read_hci_event(&evt_pkt, POLL_RETRY_TIMEOUT_MS, POLL_CONFIG_UART_MS) ==
      0) {
    if (check_hci_event_status(&evt_pkt, HCI_CMD_NXP_BLE_WAKEUP) == 0) {
      if ((evt_pkt.info.para_len > HCI_EVT_PYLD_SUBCODE_IDX) &&
          (evt_pkt.info.payload[HCI_EVT_PYLD_SUBCODE_IDX] ==
           HCI_CMD_OTT_SUB_WAKEUP_EXIT_HEARTBEATS)) {
        VND_LOGD("Exit heartbeat mode cmd sent successfully\n");
        heartbeatFlag = true;
      }
    }
  }

  if (heartbeatFlag == false) {
    VND_LOGD("Failed to read exit heartbeat cmd response! \n");
  }
  return;
}

/*****************************************************************************
**
**   BLUETOOTH VENDOR INTERFACE LIBRARY FUNCTIONS
**
*****************************************************************************/

static int bt_vnd_init(const bt_vendor_callbacks_t* p_cb,
                       unsigned char* local_bdaddr) {
  vnd_cb = p_cb;
  if (vnd_cb == NULL) {
    VND_LOGE("vnd_cb is NULL");
  }
  ALOGI("bt_vnd_init --- BT Vendor HAL Ver: %s ---", BT_HAL_VERSION);
  vnd_load_conf(VENDOR_LIB_CONF_FILE);
  VND_LOGI("Max supported Log Level: %d", VHAL_LOG_LEVEL);
  VND_LOGI("Selected Log Level:%d", vhal_trace_level);
  ALOGI(
      "BT_VHAL Log Level:%d",
      (vhal_trace_level <= VHAL_LOG_LEVEL ? vhal_trace_level : VHAL_LOG_LEVEL));
  if (local_bdaddr) {
    bdaddr = (unsigned char*)malloc(BD_ADDR_LEN);
    if (bdaddr != NULL) {
      memcpy(bdaddr, local_bdaddr, 6U);
      VND_LOGD(
          "BD address received from stack "
          "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
          bdaddr[0], bdaddr[1], bdaddr[2], bdaddr[3], bdaddr[4], bdaddr[5]);
    }
  }
  return 0;
}

/** Requested operations */
static int bt_vnd_op(bt_vendor_opcode_t opcode, void* param) {
  int ret = 0;
  int local_st = 0;
  static bt_vendor_power_state_t adapterState;
  VND_LOGD("opcode = %d", opcode);
  switch (opcode) {
    case BT_VND_OP_POWER_CTRL: {
      int* state = (int*)param;

      if (*state == BT_VND_PWR_OFF) {
        VND_LOGD("power off --------------------------------------*");
        if (enable_heartbeat_config == true) {
          wakeup_kill_heartbeat_thread();
        }
        if (adapterState == BT_VND_PWR_ON) {
          VND_LOGD("BT adapter switches from ON to OFF .. ");
          adapterState = BT_VND_PWR_OFF;
        }
      } else if (*state == BT_VND_PWR_ON) {
        VND_LOGD("power on --------------------------------------");
        if (independent_reset_mode == IR_MODE_INBAND_VSC) {
          VND_LOGD("Reset the download status for Inband IR ");
          set_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED, 0);
        }
        adapterState = BT_VND_PWR_ON;
        if (send_oob_ir_trigger == IR_TRIGGER_RFKILL) {
          bt_vnd_set_bluetooth_power(false);
          usleep(5000);
          bt_vnd_set_bluetooth_power(true);
          set_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED, 0);
        }
        if (send_oob_ir_trigger == IR_TRIGGER_GPIO) {
          set_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED, 0);

          VND_LOGD("-------------- Setting GPIO LOW -----------------");
          bt_vnd_gpio_configuration(0);  // false -> 0
          usleep(1000);
          VND_LOGD("---------------- Setting GPIO HIGH ----------------");
          bt_vnd_gpio_configuration(1);  // true -> 1
        }
      }
    } break;
    case BT_VND_OP_FW_CFG:
      hw_config_start();
      break;

    case BT_VND_OP_SCO_CFG:
      if (vnd_cb) {
        vnd_cb->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
      }
      break;
    case BT_VND_OP_USERIAL_OPEN: {
      VND_LOGD("open serial port --------------------------------------");
      int(*fd_array)[CH_MAX] = (int(*)[CH_MAX])param;
      int idx;
      int bluetooth_opened;
      int num = 0;
      uint32_t baudrate = 0;
      if (is_uart_port) {
        VND_LOGD("baudrate_bt %d", baudrate_bt);
        VND_LOGD("baudrate_fw_init %d", baudrate_fw_init);
#ifdef UART_DOWNLOAD_FW
        if (enable_download_fw) {
          VND_LOGD("download_helper %d", download_helper);
          VND_LOGD("baudrate_dl_helper %d", baudrate_dl_helper);
          VND_LOGD("baudrate_dl_image %d", baudrate_dl_image);
          VND_LOGD("pFileName_helper %s", pFileName_helper);
          VND_LOGD("pFileName_image %s", pFileName_image);
          VND_LOGD("iSecondBaudrate %d", iSecondBaudrate);
          VND_LOGD("enable_download_fw %d", enable_download_fw);
          VND_LOGD("uart_sleep_after_dl %d", uart_sleep_after_dl);
          VND_LOGD("independent_reset_mode %d", independent_reset_mode);
          VND_LOGD("send_oob_ir_trigger %d", send_oob_ir_trigger);
          VND_LOGD("independent_reset_gpio_pin %d", independent_reset_gpio_pin);
          VND_LOGD("ir_host_gpio_pin %d", ir_host_gpio_pin);
          VND_LOGD("chrdev_name %s", chrdev_name);
          VND_LOGD("send_boot_sleep_trigger %d", send_boot_sleep_trigger);
          VND_LOGD("enable_pdn_recovery %d", enable_pdn_recovery);
          VND_LOGD("enable_lpm %d", enable_lpm);
          VND_LOGD("use_controller_addr %d", use_controller_addr);
          VND_LOGD("bt_max_power_sel %d", bt_max_power_sel);
        }
#endif
      }

      if (is_uart_port) {
        /* ensure libbt can talk to the driver, only need open port once */
        if (get_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED) == true)
          mchar_fd = uart_init_open(mchar_port, baudrate_bt, 1);
        else {
#ifdef UART_DOWNLOAD_FW
          if (enable_download_fw) {
            /* if define micro UART_DOWNLOAD_FW, then open uart must with
               baudrate 115200,
               since libbt can only communicate with bootloader with baudrate
               115200*/
            /* for 9098 helper is not need, so baudrate_dl_image is 115200, and
               iSecondBaudrate is true
               to set baudrate to 3000000 before download FW*/
            baudrate =
                (download_helper == 1) ? baudrate_dl_helper : baudrate_dl_image;
          } else {
            baudrate = baudrate_fw_init;
          }
#else
          baudrate = baudrate_fw_init;
#endif
#ifdef UART_DOWNLOAD_FW
          if (send_boot_sleep_trigger) {
            if (get_prop_int32(PROP_BLUETOOTH_BOOT_SLEEP_TRIGGER) == 0) {
              VND_LOGD("boot sleep trigger is enabled and its first boot");
              mchar_fd = uart_init_open(mchar_port, baudrate, 1);
              close(mchar_fd);
            }
          }
#endif
          mchar_fd = uart_init_open(mchar_port, baudrate, 0);
          if ((independent_reset_mode == IR_MODE_INBAND_VSC) &&
              (mchar_fd > 0)) {
            if (bt_vnd_send_inband_ir(baudrate) != 0) {
              return -1;
            }
          }
        }
        if (mchar_fd > 0) {
          VND_LOGI("open uart port successfully, fd=%d, mchar_port=%s",
                   mchar_fd, mchar_port);
        } else {
          VND_LOGE("open UART bt port %s failed fd: %d", mchar_port, mchar_fd);
          return -1;
        }
        bluetooth_opened = get_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED);
#ifdef UART_DOWNLOAD_FW
        if ((enable_download_fw == true) &&
            !get_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED)) {
          if (detect_and_download_fw() != 0) {
            VND_LOGE("detect_and_download_fw failed");
            set_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED, 0);
            if (enable_pdn_recovery == true) {
              int init_attempted =
                  get_prop_int32(PROP_BLUETOOTH_INIT_ATTEMPTED);
              init_attempted = (init_attempted == -1) ? 0 : init_attempted;
              if (++init_attempted >= PDN_RECOVERY_THRESHOLD) {
                VND_LOGE("%s: %s(%d) > %d, Triggering PDn recovery.\n",
                         __FUNCTION__, PROP_BLUETOOTH_INIT_ATTEMPTED,
                         init_attempted, PDN_RECOVERY_THRESHOLD);
                set_prop_int32(PROP_VENDOR_TRIGGER_PDN, 1);
                set_prop_int32(PROP_BLUETOOTH_INIT_ATTEMPTED, 0);
              } else {
                set_prop_int32(PROP_BLUETOOTH_INIT_ATTEMPTED, init_attempted);
                ALOGI("%s:%d\n", PROP_VENDOR_TRIGGER_PDN, init_attempted);
              }
            }
            return -1;
          }
        } else {
          ti.c_cflag |= CRTSCTS;
          if (tcsetattr(mchar_fd, TCSANOW, &ti) < 0) {
            VND_LOGE("Set Flow Control failed!");
            VND_LOGE("Error: %s (%d)", strerror(errno), errno);
            return -1;
          }
          tcflush(mchar_fd, TCIOFLUSH);
        }
#else
        ti.c_cflag |= CRTSCTS;
        if (tcsetattr(mchar_fd, TCSANOW, &ti) < 0) {
          VND_LOGE("Set Flow Control failed!");
          VND_LOGE("Error: %s (%d)", strerror(errno), errno);
          return -1;
        }
        tcflush(mchar_fd, TCIOFLUSH);
#endif
        if (!bluetooth_opened) {
#ifdef UART_DOWNLOAD_FW
          if (!enable_download_fw)
#endif
          {
            /*NXP Bluetooth use combo firmware which is loaded at wifi driver
            probe.
            This function will wait to make sure basic client netdev is created
            */
            int count = (int)((POLL_DRIVER_MAX_TIME_MS * 1000) /
                              POLL_DRIVER_DURATION_US);
            FILE* fd = NULL;

            while (count-- > 0) {
              fd = fopen("/sys/class/net/wlan0", "r");
              if (fd != NULL) {
                VND_LOGD("Error: %s (%d)", strerror(errno), errno);
                fclose(fd);
                break;
              }
              usleep(POLL_DRIVER_DURATION_US);
            }
          }

          if (config_uart()) {
            VND_LOGE("config_uart failed");
            set_prop_int32(PROP_BLUETOOTH_FW_DOWNLOADED, 0);
            return -1;
          }
        }
      } else {
        do {
          mchar_fd = open(mbt_port, O_RDWR | O_NOCTTY);
          if (mchar_fd < 0) {
            num++;
            if (num >= 8) {
              VND_LOGE("exceed max retry count, return error");
              return -1;
            } else {
              VND_LOGW("open USB/SD port %s failed fd: %d, retrying", mbt_port,
                       mchar_fd);
              VND_LOGW("Error: %s (%d)", strerror(errno), errno);
              sleep(1);
              continue;
            }
          } else {
            VND_LOGI("open USB or SD port successfully, fd=%d, mbt_port=%s",
                     mchar_fd, mbt_port);
          }
        } while (mchar_fd < 0);
      }

      for (idx = 0; idx < ((int)CH_MAX); idx++) {
        (*fd_array)[idx] = mchar_fd;
        ret = 1;
      }
      if (enable_pdn_recovery == true) {
        // Reset PROP_BLUETOOTH_INIT_ATTEMPTED as init is successful
        set_prop_int32(PROP_BLUETOOTH_INIT_ATTEMPTED, 0);
      }
      VND_LOGD("open serial port over --------------------------------------");
    } break;
    case BT_VND_OP_USERIAL_CLOSE:

      if (enable_heartbeat_config == true) {
        send_exit_heartbeat_mode();
      }
      send_hci_reset();
      /* mBtChar port is blocked on read. Release the port before we close it */
      if (is_uart_port) {
        if (mchar_fd) {
          tcflush(mchar_fd, TCIOFLUSH);
          close(mchar_fd);
          mchar_fd = 0;
        }
      } else {
        if (ioctl(mchar_fd, MBTCHAR_IOCTL_RELEASE, &local_st) < 0) {
          VND_LOGE("ioctl error: %s (%d)", strerror(errno), errno);
        }
        /* Give it sometime before we close the mbtchar */
        usleep(1000);
        if (mchar_fd) {
          if (close(mchar_fd) < 0) {
            VND_LOGE("close serial port failed!");
            VND_LOGE("Error: %s (%d)", strerror(errno), errno);
            ret = -1;
          }
        }
      }
      break;
    case BT_VND_OP_GET_LPM_IDLE_TIMEOUT: {
      uint32_t* timeout_ms = (uint32_t*)param;
      *timeout_ms = (enable_lpm == true) ? lpm_timeout_ms : 0;
      VND_LOGI("LPM timeout = %d", *timeout_ms);
    } break;
    case BT_VND_OP_LPM_SET_MODE:
      if (enable_lpm == true) {
        bt_vendor_lpm_mode_t* lpm_mode = (bt_vendor_lpm_mode_t*)param;
        if (*lpm_mode == BT_VND_LPM_ENABLE) {
          VND_LOGI("Enable LPM mode");
          ret = hw_bt_configure_lpm(BT_SET_SLEEP_MODE);
        } else {
          VND_LOGI("Disable LPM mode");
          ret = hw_bt_configure_lpm(BT_SET_FULL_POWER_MODE);
        }
      }
      if (vnd_cb) {
        if (ret == 0) {
          vnd_cb->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
        } else {
          vnd_cb->lpm_cb(BT_VND_OP_RESULT_FAIL);
        }
      }
      break;
    case BT_VND_OP_LPM_WAKE_SET_STATE:
      if (lpm_configured == true) {
        int status;
        bt_vendor_lpm_wake_state_t* wake_state =
            (bt_vendor_lpm_wake_state_t*)param;
        if (*wake_state == BT_VND_LPM_WAKE_ASSERT) {
          VND_LOGI("LPM: Wakeup BT Device");
          status = ioctl(mchar_fd, TIOCCBRK);
          VND_LOGI("Assert Status:%d\n", status);
        } else {
          VND_LOGI("LPM: Allow BT Device to sleep");
          status = ioctl(mchar_fd, TIOCSBRK);
          VND_LOGI("Deassert Status:%d\n", status);
        }
        if (status < 0) {
          VND_LOGE("LPM toggle Error: %s (%d)", strerror(errno), errno);
        }
      }
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}

/** Closes the interface */
static void bt_vnd_cleanup(void) {
  VND_LOGD("cleanup ...");
  vnd_cb = NULL;
  if (bdaddr) {
    free(bdaddr);
    bdaddr = NULL;
  }
}

/** Entry point of DLib */
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    bt_vnd_init,
    bt_vnd_op,
    bt_vnd_cleanup,
};
