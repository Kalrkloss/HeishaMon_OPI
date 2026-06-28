/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <Arduino.h>

#include "mem.h"
#include "../../webfunctions.h"

extern settingsStruct heishamonSettings;
extern PubSubClient mqtt_client;
extern const char* mqtt_logtopic;

void _logprintln(const char *file, unsigned int line, char *msg) {
  if(heishamonSettings.logSerial1) {
#if 0
    Serial1.print(millis());
    Serial1.print(": ");
    Serial1.println(msg);
#elif defined(ESP32)
    Serial.print(millis());
    Serial.print(": ");
    Serial.println(msg);
#endif	  
  }
  websocket_write_all(msg, strlen(msg));
}

void _logprintf(const char *file, unsigned int line, char *fmt, ...) {
  char *str = NULL;

  va_list ap;
  va_start(ap, fmt);

  int bytes = vsnprintf(NULL, 0, fmt, ap);

  va_end(ap);
  if((str = (char *)MALLOC(bytes+1)) == NULL) {
    OUT_OF_MEMORY
    return;
  }
  va_start(ap, fmt);
  vsnprintf(str, bytes+1, fmt, ap);
  va_end(ap);

  _logprintln(file, line, str);

  FREE(str);
}

void _logprintln_P(const char *file, unsigned int line, const char *msg) {
  PGM_P p = (PGM_P)msg;
  int len = strlen_P((const char *)p);
  char *str = (char *)MALLOC(len+1);
  if(str == NULL) {
    OUT_OF_MEMORY
  }
  strcpy_P(str, p);

  _logprintln(file, line, str);

  FREE(str);
}

void _logprintf_P(const char *file, unsigned int line, const char *fmt, ...) {
  PGM_P p = (PGM_P)fmt;
  int len = strlen_P((const char *)p);
  char *foo = (char *)MALLOC(len+1);
  if(foo == NULL) {
    OUT_OF_MEMORY
    return;
  }
  strcpy_P(foo, p);

  char *str = NULL;

  va_list ap;
  va_start(ap, fmt);

  int bytes = vsnprintf(NULL, 0, foo, ap);

  va_end(ap);
  if((str = (char *)MALLOC(bytes+1)) == NULL) {
    OUT_OF_MEMORY
    FREE(foo);
    return;
  }
  va_start(ap, fmt);
  vsnprintf(str, bytes+1, foo, ap);
  va_end(ap);

  _logprintln(file, line, str);

  FREE(foo);
  FREE(str);
}