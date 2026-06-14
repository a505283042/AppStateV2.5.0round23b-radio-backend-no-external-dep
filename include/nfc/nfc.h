#ifndef NFC_H
#define NFC_H

#include <Arduino.h>
#include <stdint.h>

void nfc_init(void);
void nfc_poll(void);
bool nfc_take_last_uid(String& out_uid);
bool nfc_is_uid_present(const String& uid);
void nfc_ignore_uid_once(const String& uid, uint32_t ms);
// 读取最近一次刷卡事件，同时带出卡类型描述；会消费这次事件。
bool nfc_take_last_card_info(String& out_uid, String& out_card_type);

#endif