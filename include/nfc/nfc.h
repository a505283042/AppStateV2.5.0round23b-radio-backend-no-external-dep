#ifndef NFC_H
#define NFC_H

#include <Arduino.h>
#include <stdint.h>

void nfc_init(void);
void nfc_poll(void);
bool nfc_take_last_uid(String& out_uid);
bool nfc_is_uid_present(const String& uid);
void nfc_ignore_uid_once(const String& uid, uint32_t ms);

#endif