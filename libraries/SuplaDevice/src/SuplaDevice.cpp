/*
 Copyright (C) AC SOFTWARE SP. Z O.O.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#define SUPLADEVICE_CPP

#include <Arduino.h>

#include "SuplaDevice.h"
#include "SuplaImpulseCounter.h"
#include "supla-common/IEEE754tools.h"
#include "supla-common/log.h"
#include "supla-common/srpc.h"
#include "supla/channel.h"
#include "supla/element.h"
#include "supla/io.h"
#include "supla/timer.h"

#define RS_STOP_DELAY  500
#define RS_START_DELAY 1000

#define RS_RELAY_OFF  0
#define RS_RELAY_UP   2
#define RS_RELAY_DOWN 1

#define RS_DIRECTION_NONE 0
#define RS_DIRECTION_UP   2
#define RS_DIRECTION_DOWN 1

void SuplaDeviceClass::status(int status, const char *msg) {
  static int currentStatus = STATUS_UNKNOWN;
  if (impl_arduino_status != NULL) {
    impl_arduino_status(status, msg);
  } else {
    if (currentStatus != status) {
      currentStatus = status;
      supla_log(LOG_DEBUG, "Current status: [%d] %s", status, msg);
    }
  }
}

SuplaDeviceClass::SuplaDeviceClass()
    : port(-1), connectionFailCounter(0), networkIsNotReadyCounter(0) {
  srpc = NULL;
  registered = 0;
  last_iterate_time = 0;
  wait_for_iterate = 0;
  channel_pin = NULL;
  roller_shutter = NULL;
  rs_count = 0;
  channel_pin_count = 0;

  impl_rs_save_position = NULL;
  impl_rs_load_position = NULL;
  impl_rs_save_settings = NULL;
  impl_rs_load_settings = NULL;
}

SuplaDeviceClass::~SuplaDeviceClass() {
  if (channel_pin != NULL) {
    free(channel_pin);
    channel_pin = NULL;
  }

  if (roller_shutter != NULL) {
    free(roller_shutter);
    roller_shutter = NULL;
  }

  rs_count = 0;
}

bool SuplaDeviceClass::suplaDigitalRead_isHI(int channelNumber, uint8_t pin) {
  return Supla::Io::digitalRead(channelNumber, pin) ==
         (channel_pin[channelNumber].hiIsLo ? LOW : HIGH);
}

void SuplaDeviceClass::suplaDigitalWrite_setHI(int channelNumber,
                                               uint8_t pin,
                                               bool hi) {
  if (channel_pin[channelNumber].hiIsLo) {
    hi = hi ? LOW : HIGH;
  }

  Supla::Io::digitalWrite(channelNumber, pin, hi);
}

void SuplaDeviceClass::setStatusFuncImpl(
    _impl_arduino_status impl_arduino_status) {
  this->impl_arduino_status = impl_arduino_status;
}

bool SuplaDeviceClass::isInitialized(bool msg) {
  if (srpc != NULL) {
    if (msg)
      status(STATUS_ALREADY_INITIALIZED, "SuplaDevice is already initialized");

    return true;
  }

  return false;
}

bool SuplaDeviceClass::begin(char GUID[SUPLA_GUID_SIZE],
                             const char *Server,
                             const char *email,
                             char authkey[SUPLA_AUTHKEY_SIZE],
                             unsigned char version) {
  if (isInitialized(true)) return false;

  if (Supla::Network::Instance() == NULL) {
    status(STATUS_MISSING_NETWORK_INTERFACE, "Network Interface not defined!");
    return false;
  }

  memcpy(Supla::Channel::reg_dev.GUID, GUID, SUPLA_GUID_SIZE);
  memcpy(Supla::Channel::reg_dev.AuthKey, authkey, SUPLA_AUTHKEY_SIZE);

  setString(Supla::Channel::reg_dev.Email, email, SUPLA_EMAIL_MAXSIZE);
  setString(
      Supla::Channel::reg_dev.ServerName, Server, SUPLA_SERVER_NAME_MAXSIZE);

  bool emptyGuidDetected = true;
  for (int i = 0; i < SUPLA_GUID_SIZE; i++) {
    if (Supla::Channel::reg_dev.GUID[i] != 0) {
      emptyGuidDetected = false;
    }
  }
  if (emptyGuidDetected) {
    status(STATUS_INVALID_GUID, "Invalid GUID");
    return false;
  }

  if (Supla::Channel::reg_dev.ServerName[0] == NULL) {
    status(STATUS_UNKNOWN_SERVER_ADDRESS, "Unknown server address");
    return false;
  }

  if (Supla::Channel::reg_dev.Email[0] == NULL) {
    status(STATUS_MISSING_CREDENTIALS, "Unknown email address");
    return false;
  }

  bool emptyAuthKeyDetected = true;
  for (int i = 0; i < SUPLA_AUTHKEY_SIZE; i++) {
    if (Supla::Channel::reg_dev.AuthKey[i] != 0) {
      emptyAuthKeyDetected = false;
      break;
    }
  }
  if (emptyAuthKeyDetected) {
    status(STATUS_MISSING_CREDENTIALS, "Unknown AuthKey");
    return false;
  }

  if (strnlen(Supla::Channel::reg_dev.Name, SUPLA_DEVICE_NAME_MAXSIZE) == 0) {
#if defined(ARDUINO_ARCH_ESP8266)
    setString(
        Supla::Channel::reg_dev.Name, "ESP8266", SUPLA_DEVICE_NAME_MAXSIZE);
#elif defined(ARDUINO_ARCH_ESP32)
    setString(Supla::Channel::reg_dev.Name, "ESP32", SUPLA_DEVICE_NAME_MAXSIZE);
#else
    setString(
        Supla::Channel::reg_dev.Name, "ARDUINO", SUPLA_DEVICE_NAME_MAXSIZE);
#endif
  }

  setString(Supla::Channel::reg_dev.SoftVer, "2.3.1", SUPLA_SOFTVER_MAXSIZE);

  Supla::Network::Setup();

  TsrpcParams srpc_params;
  srpc_params_init(&srpc_params);
  srpc_params.data_read = &Supla::data_read;
  srpc_params.data_write = &Supla::data_write;
  srpc_params.on_remote_call_received = &Supla::message_received;
  srpc_params.user_params = this;

  srpc = srpc_init(&srpc_params);
  Supla::Network::SetSrpc(srpc);

  // Set Supla protocol interface version
  srpc_set_proto_version(srpc, version);

  supla_log(LOG_DEBUG, "Using Supla protocol version %d", version);

  if (rs_count > 0) {
    for (int a = 0; a < rs_count; a++) {
      rs_load_settings(&roller_shutter[a]);
      rs_load_position(&roller_shutter[a]);

      Supla::Channel::reg_dev.channels[roller_shutter[a].channel_number]
          .value[0] = (roller_shutter[a].position - 100) / 100;
    }
  }

  // Iterate all elements and load configuration (TODO)
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onLoadConfig();
  }

  // Load counters values from EEPROM storage
  SuplaImpulseCounter::loadStorage();

  // Enable timers
  Supla::initTimers();

  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onInit();
  }

  for (int a = 0; a < channel_pin_count; a++) {
    SuplaImpulseCounter *ptr = SuplaImpulseCounter::getCounterByChannel(a);
    if (ptr) {
      _supla_int64_t value = ptr->getCounter();
      ptr->clearIsChanged();
      memcpy(Supla::Channel::reg_dev.channels[a].value, &value, 8);
    }
  }

  status(STATUS_INITIALIZED, "SuplaDevice initialized");
  return true;
}

void SuplaDeviceClass::setName(const char *Name) {
  if (isInitialized(true)) return;
  setString(Supla::Channel::reg_dev.Name, Name, SUPLA_DEVICE_NAME_MAXSIZE);
}

int SuplaDeviceClass::addChannel(int pin1,
                                 int pin2,
                                 bool hiIsLo,
                                 bool bistable) {
  if (isInitialized(true)) return -1;

  if (Supla::Channel::reg_dev.channel_count >= SUPLA_CHANNELMAXCOUNT) {
    status(STATUS_CHANNEL_LIMIT_EXCEEDED, "Channel limit exceeded");
    return -1;
  }

  channel_pin_count++;

  if (bistable && (pin1 == 0 || pin2 == 0)) bistable = false;

  // !!! Channel number is always equal to channel array idx
  // Supla::Channel::reg_dev.channels[idx]
  Supla::Channel::reg_dev.channels[Supla::Channel::reg_dev.channel_count]
      .Number = Supla::Channel::reg_dev.channel_count;
  channel_pin = (SuplaChannelPin *)realloc(
      channel_pin,
      sizeof(SuplaChannelPin) * (Supla::Channel::reg_dev.channel_count + 1));
  channel_pin[Supla::Channel::reg_dev.channel_count].pin1 = pin1;
  channel_pin[Supla::Channel::reg_dev.channel_count].pin2 = pin2;
  channel_pin[Supla::Channel::reg_dev.channel_count].hiIsLo = hiIsLo;
  channel_pin[Supla::Channel::reg_dev.channel_count].bistable = bistable;
  channel_pin[Supla::Channel::reg_dev.channel_count].time_left =
      0;  // 100*Supla::Channel::reg_dev.channel_count;
  channel_pin[Supla::Channel::reg_dev.channel_count].vc_time = 0;
  channel_pin[Supla::Channel::reg_dev.channel_count].bi_time_left = 0;
  channel_pin[Supla::Channel::reg_dev.channel_count].last_val =
      Supla::Io::digitalRead(Supla::Channel::reg_dev.channel_count,
                             bistable ? pin2 : pin1);

  Supla::Channel::reg_dev.channel_count++;

  return Supla::Channel::reg_dev.channel_count - 1;
}

int SuplaDeviceClass::addRelay(int relayPin1,
                               int relayPin2,
                               bool hiIsLo,
                               bool bistable,
                               _supla_int_t functions) {
  int c = addChannel(relayPin1, relayPin2, hiIsLo, bistable);
  if (c == -1) return -1;

  uint8_t _HI = hiIsLo ? LOW : HIGH;
  uint8_t _LO = hiIsLo ? HIGH : LOW;

  Supla::Channel::reg_dev.channels[c].Type = SUPLA_CHANNELTYPE_RELAY;
  Supla::Channel::reg_dev.channels[c].FuncList = functions;

  if (relayPin1 != 0) {
    pinMode(relayPin1, OUTPUT);
    Supla::Io::digitalWrite(Supla::Channel::reg_dev.channels[c].Number,
                            relayPin1,
                            hiIsLo ? HIGH : LOW);

    if (bistable == false)
      Supla::Channel::reg_dev.channels[c].value[0] =
          Supla::Io::digitalRead(Supla::Channel::reg_dev.channels[c].Number,
                                 relayPin1) == _HI
              ? 1
              : 0;
  }

  if (relayPin2 != 0)
    if (bistable) {
      pinMode(relayPin2, INPUT);
      Supla::Channel::reg_dev.channels[c].value[0] =
          Supla::Io::digitalRead(Supla::Channel::reg_dev.channels[c].Number,
                                 relayPin2) == HIGH
              ? 1
              : 0;

    } else {
      pinMode(relayPin2, OUTPUT);
      Supla::Io::digitalWrite(Supla::Channel::reg_dev.channels[c].Number,
                              relayPin2,
                              hiIsLo ? HIGH : LOW);

      if (Supla::Channel::reg_dev.channels[c].value[0] == 0 &&
          Supla::Io::digitalRead(Supla::Channel::reg_dev.channels[c].Number,
                                 relayPin2) == _HI)
        Supla::Channel::reg_dev.channels[c].value[0] = 2;
    }

  return c;
}

bool SuplaDeviceClass::addRelay(int relayPin, bool hiIsLo) {
  return addRelay(relayPin,
                  0,
                  hiIsLo,
                  false,
                  SUPLA_BIT_FUNC_CONTROLLINGTHEGATEWAYLOCK |
                      SUPLA_BIT_FUNC_CONTROLLINGTHEGATE |
                      SUPLA_BIT_FUNC_CONTROLLINGTHEGARAGEDOOR |
                      SUPLA_BIT_FUNC_CONTROLLINGTHEDOORLOCK |
                      SUPLA_BIT_FUNC_POWERSWITCH | SUPLA_BIT_FUNC_LIGHTSWITCH |
                      SUPLA_BIT_FUNC_STAIRCASETIMER) > -1;
}

bool SuplaDeviceClass::addRelay(int relayPin) {
  return addRelay(relayPin, false) > -1;
}

bool SuplaDeviceClass::addRollerShutterRelays(int relayPin1,
                                              int relayPin2,
                                              bool hiIsLo) {
  int channel_number = addRelay(relayPin1,
                                relayPin2,
                                hiIsLo,
                                false,
                                SUPLA_BIT_FUNC_CONTROLLINGTHEROLLERSHUTTER);

  if (channel_number > -1) {
    roller_shutter = (SuplaDeviceRollerShutter *)realloc(
        roller_shutter, sizeof(SuplaDeviceRollerShutter) * (rs_count + 1));
    memset(&roller_shutter[rs_count], 0, sizeof(SuplaDeviceRollerShutter));

    roller_shutter[rs_count].channel_number = channel_number;
    roller_shutter[rs_count].position = 0;
    Supla::Channel::reg_dev.channels[channel_number].value[0] = -1;

    rs_count++;

    return true;
  }

  return false;
}

bool SuplaDeviceClass::addRollerShutterRelays(int relayPin1, int relayPin2) {
  return addRollerShutterRelays(relayPin1, relayPin2, false);
}

void SuplaDeviceClass::setRollerShutterButtons(int channel_number,
                                               int btnUpPin,
                                               int btnDownPin) {
  SuplaDeviceRollerShutter *rs = rsByChannelNumber(channel_number);
  if (rs) {
    if (btnUpPin > 0) {
      pinMode(btnUpPin, INPUT_PULLUP);
    }
    rs->btnUp.pin = btnUpPin;
    rs->btnUp.value = 1;

    if (btnDownPin > 0) {
      pinMode(btnDownPin, INPUT_PULLUP);
    }
    rs->btnDown.pin = btnDownPin;
    rs->btnDown.value = 1;
  }
}

bool SuplaDeviceClass::addImpulseCounter(int impulsePin,
                                         int statusLedPin,
                                         bool detectLowToHigh,
                                         bool inputPullup,
                                         unsigned long debounceDelay) {
  int c = addChannel(0, 0, false, false);
  if (c == -1) return false;

  Supla::Channel::reg_dev.channels[c].Type = SUPLA_CHANNELTYPE_IMPULSE_COUNTER;

  // Init channel value with "0"
  memset(Supla::Channel::reg_dev.channels[c].value, 0, 8);

  SuplaImpulseCounter::create(
      c, impulsePin, statusLedPin, detectLowToHigh, inputPullup, debounceDelay);
}

void SuplaDeviceClass::setString(char *dst, const char *src, int max_size) {
  if (src == NULL) {
    dst[0] = 0;
    return;
  }

  int size = strlen(src);

  if (size + 1 > max_size) size = max_size - 1;

  memcpy(dst, src, size);
}

void SuplaDeviceClass::setRollerShutterFuncImpl(
    _impl_rs_save_position impl_rs_save_position,
    _impl_rs_load_position impl_rs_load_position,
    _impl_rs_save_settings impl_rs_save_settings,
    _impl_rs_load_settings impl_rs_load_settings) {
  this->impl_rs_save_position = impl_rs_save_position;
  this->impl_rs_load_position = impl_rs_load_position;
  this->impl_rs_save_settings = impl_rs_save_settings;
  this->impl_rs_load_settings = impl_rs_load_settings;
}

void SuplaDeviceClass::iterate_relay(SuplaChannelPin *pin,
                                     TDS_SuplaDeviceChannel_C *channel,
                                     unsigned long time_diff,
                                     int channel_number) {
  if (pin->bi_time_left != 0) {
    if (time_diff >= pin->bi_time_left) {
      // switch off bistable relay
      Supla::Io::digitalWrite(
          channel->Number, pin->pin1, pin->hiIsLo ? HIGH : LOW);
      pin->bi_time_left = 0;

    } else if (pin->bi_time_left > 0) {
      pin->bi_time_left -= time_diff;
    }
  }

  if (pin->time_left != 0) {
    if (time_diff >= pin->time_left) {
      pin->time_left = 0;

      if (channel->Type == SUPLA_CHANNELTYPE_RELAY)
        // switch off relay
        channel->value[0] = 0;
      channelSetValue(channel_number, 0, 0);

    } else if (pin->time_left > 0) {
      pin->time_left -= time_diff;
    }
  }

  // if relay type is "bistable relay", then read relay state every 200 ms and
  // update server
  if (channel->Type == SUPLA_CHANNELTYPE_RELAY && pin->bistable) {
    if (time_diff >= pin->vc_time) {
      pin->vc_time -= time_diff;

    } else {
      uint8_t val = Supla::Io::digitalRead(channel->Number, pin->pin2);

      if (val != pin->last_val) {
        pin->last_val = val;
        pin->vc_time = 200;
        channel->value[0] = val == HIGH ? 1 : 0;

        channelValueChanged(channel->Number, val == HIGH ? 1 : 0);
      }
    }
  }
}

void SuplaDeviceClass::rs_save_position(SuplaDeviceRollerShutter *rs) {
  if (impl_rs_save_position) {
    impl_rs_save_position(rs->channel_number, rs->position);
  }
}

void SuplaDeviceClass::rs_load_position(SuplaDeviceRollerShutter *rs) {
  if (impl_rs_load_position) {
    impl_rs_load_position(rs->channel_number, &rs->position);
  }
}

void SuplaDeviceClass::rs_save_settings(SuplaDeviceRollerShutter *rs) {
  if (impl_rs_save_settings) {
    impl_rs_save_settings(
        rs->channel_number, rs->full_opening_time, rs->full_closing_time);
  }
}

void SuplaDeviceClass::rs_load_settings(SuplaDeviceRollerShutter *rs) {
  if (impl_rs_load_settings) {
    impl_rs_load_settings(
        rs->channel_number, &rs->full_opening_time, &rs->full_closing_time);
  }
}

void SuplaDeviceClass::rs_set_relay(SuplaDeviceRollerShutter *rs,
                                    SuplaChannelPin *pin,
                                    uint8_t value,
                                    bool cancel_task,
                                    bool stop_delay) {
  if (cancel_task) {
    rs_cancel_task(rs);
  }

  unsigned long now = millis();

  if (value == RS_RELAY_OFF) {
    if (rs->cvr1.active) {
      return;
    }

    rs->cvr2.active = false;
    rs->cvr1.value = value;

    if (now - rs->start_time >= RS_STOP_DELAY) {
      rs->cvr1.time = now;
    } else {
      rs->cvr1.time = now + RS_STOP_DELAY - (now - rs->start_time);
    }

    rs->cvr1.active = true;

  } else {
    if (rs->cvr2.active) {
      return;
    }

    rs->cvr1.active = false;
    rs->cvr2.value = value;

    int _pin = value == RS_RELAY_DOWN ? pin->pin2 : pin->pin1;

    if (suplaDigitalRead_isHI(rs->channel_number, _pin)) {
      rs_set_relay(rs, pin, RS_RELAY_OFF, false, stop_delay);
      rs->cvr2.time = rs->cvr1.time + RS_START_DELAY;
    } else {
      if (now - rs->stop_time >= RS_START_DELAY) {
        rs->cvr2.time = now;
      } else {
        rs->cvr2.time = now + RS_START_DELAY - (now - rs->stop_time);
      }
    }

    rs->cvr2.active = true;
  }
}

void SuplaDeviceClass::rs_set_relay(int channel_number, uint8_t value) {
  SuplaDeviceRollerShutter *rs = rsByChannelNumber(channel_number);

  if (rs) {
    rs_set_relay(rs, &channel_pin[channel_number], value, true, true);
  }
};

void SuplaDeviceClass::rs_calibrate(SuplaDeviceRollerShutter *rs,
                                    unsigned long full_time,
                                    unsigned long time,
                                    int dest_pos) {
  if (full_time > 0 && (rs->position < 100 || rs->position > 10100)) {
    full_time *= 1.1;  // 10% margin

    if (time >= full_time) {
      rs->position = dest_pos;
      rs->save_position = 1;
    }
  }
}

void SuplaDeviceClass::rs_move_position(SuplaDeviceRollerShutter *rs,
                                        SuplaChannelPin *pin,
                                        unsigned long full_time,
                                        unsigned long *time,
                                        bool up) {
  if (rs->position < 100 || rs->position > 10100 || full_time == 0) {
    return;
  };

  int last_pos = rs->position;
  unsigned long p = (*time) * 100.00 / full_time * 100;
  unsigned long x = p * (full_time) / 10000;

  if (p > 0) {
    if (up) {
      if (int(rs->position - p) <= 100) {
        rs->position = 100;
      } else {
        rs->position -= p;
      }
    } else {
      if (int(rs->position + p) >= 10100) {
        rs->position = 10100;
      } else {
        rs->position += p;
      }
    }

    if (last_pos != rs->position) {
      rs->save_position = 1;
    }
  }

  if ((up && rs->position == 100) || (!up && rs->position == 10100)) {
    if ((*time) >= full_time * 1.1) {
      rs_set_relay(rs, pin, RS_RELAY_OFF, false, false);
    }

    return;
  }

  if (x <= (*time)) {
    (*time) -= x;
  } else {  // something goes wrong
    (*time) = 0;
  }
}

bool SuplaDeviceClass::rs_time_margin(unsigned long full_time,
                                      unsigned long time,
                                      uint8_t m) {
  return (full_time > 0 && (time * 100 / full_time) < m) ? true : false;
}

void SuplaDeviceClass::rs_task_processing(SuplaDeviceRollerShutter *rs,
                                          SuplaChannelPin *pin) {
  if (!rs->task.active) {
    return;
  }

  if (rs->position < 100 || rs->position > 10100) {
    if (!suplaDigitalRead_isHI(rs->channel_number, pin->pin1) &&
        !suplaDigitalRead_isHI(rs->channel_number, pin->pin2) &&
        rs->full_opening_time > 0 && rs->full_closing_time > 0) {
      if (rs->task.percent < 50) {
        rs_set_relay(rs, pin, RS_RELAY_UP, false, false);
      } else {
        rs_set_relay(rs, pin, RS_RELAY_DOWN, false, false);
      }
    }

    return;
  }

  uint8_t percent = (rs->position - 100) / 100;

  if (rs->task.direction == RS_DIRECTION_NONE) {
    if (percent > rs->task.percent) {
      rs->task.direction = RS_DIRECTION_UP;
      rs_set_relay(rs, pin, RS_RELAY_UP, false, false);

    } else if (percent < rs->task.percent) {
      rs->task.direction = RS_DIRECTION_DOWN;
      rs_set_relay(rs, pin, RS_RELAY_DOWN, false, false);

    } else {
      rs->task.active = 0;
      rs_set_relay(rs, pin, RS_RELAY_OFF, false, false);
    }

  } else if ((rs->task.direction == RS_DIRECTION_UP &&
              percent <= rs->task.percent) ||
             (rs->task.direction == RS_DIRECTION_DOWN &&
              percent >= rs->task.percent)) {
    if (rs->task.percent == 0 &&
        rs_time_margin(rs->full_opening_time, rs->up_time, 5)) {  // margin 5%

      // supla_log(LOG_DEBUG, "UP MARGIN 5%");

    } else if (rs->task.percent == 100 &&
               rs_time_margin(rs->full_closing_time, rs->down_time, 5)) {
      // supla_log(LOG_DEBUG, "DOWN MARGIN 5%");

    } else {
      rs->task.active = 0;
      rs_set_relay(rs, pin, RS_RELAY_OFF, false, false);
    }
  }
}

void SuplaDeviceClass::rs_add_task(SuplaDeviceRollerShutter *rs,
                                   unsigned char percent) {
  if ((rs->position - 100) / 100 == percent) return;

  if (percent > 100) percent = 100;

  rs->task.percent = percent;
  rs->task.direction = RS_DIRECTION_NONE;
  rs->task.active = 1;
}

void SuplaDeviceClass::rs_cancel_task(SuplaDeviceRollerShutter *rs) {
  if (rs == NULL) return;

  rs->task.active = 0;
  rs->task.percent = 0;
  rs->task.direction = RS_DIRECTION_NONE;
}

void SuplaDeviceClass::rs_cvr_processing(SuplaDeviceRollerShutter *rs,
                                         SuplaChannelPin *pin,
                                         SuplaDeviceRollerShutterCVR *cvr) {
  unsigned long now = millis();

  if (cvr->active && cvr->time <= now) {
    cvr->active = false;

    if (cvr->value == RS_RELAY_UP) {
      rs->start_time = now;
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin1, false);
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin2, true);
    } else if (cvr->value == RS_RELAY_DOWN) {
      rs->start_time = now;
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin2, false);
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin1, true);
    } else {
      rs->stop_time = now;
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin1, false);
      suplaDigitalWrite_setHI(rs->channel_number, pin->pin2, false);
    }
  }
}

bool SuplaDeviceClass::rs_button_released(SuplaDeviceRollerShutterButton *btn) {
  if (btn->pin > 0) {
    uint8_t v = digitalRead(btn->pin);
    if (v != btn->value && millis() - btn->time >= 50) {
      btn->value = v;
      return v == 1;
    }
  }

  return false;
}

void SuplaDeviceClass::rs_buttons_processing(SuplaDeviceRollerShutter *rs) {
  if (rs_button_released(&rs->btnUp)) {
    if (SuplaDevice.rollerShutterMotorIsOn(rs->channel_number)) {
      SuplaDevice.rollerShutterStop(rs->channel_number);
    } else {
      SuplaDevice.rollerShutterReveal(rs->channel_number);
    }

  } else if (rs_button_released(&rs->btnDown)) {
    if (SuplaDevice.rollerShutterMotorIsOn(rs->channel_number)) {
      SuplaDevice.rollerShutterStop(rs->channel_number);
    } else {
      SuplaDevice.rollerShutterShut(rs->channel_number);
    };
  }
}

void SuplaDeviceClass::iterate_rollershutter(
    SuplaDeviceRollerShutter *rs,
    SuplaChannelPin *pin,
    TDS_SuplaDeviceChannel_C *channel) {
  rs_cvr_processing(rs, pin, &rs->cvr1);
  rs_cvr_processing(rs, pin, &rs->cvr2);

  if (rs->last_iterate_time == 0) {
    rs->last_iterate_time = millis();
    return;
  }

  unsigned long time_diff = millis() - rs->last_iterate_time;

  if (suplaDigitalRead_isHI(rs->channel_number, pin->pin1)) {  // DOWN

    rs->up_time = 0;
    rs->down_time += time_diff;

    rs_calibrate(rs, rs->full_closing_time, rs->down_time, 1100);
    rs_move_position(rs, pin, rs->full_closing_time, &rs->down_time, false);

  } else if (suplaDigitalRead_isHI(rs->channel_number, pin->pin2)) {  // UP

    rs->up_time += time_diff;
    rs->down_time = 0;

    rs_calibrate(rs, rs->full_opening_time, rs->up_time, 100);
    rs_move_position(rs, pin, rs->full_opening_time, &rs->up_time, true);

  } else {
    if (rs->up_time != 0) {
      rs->up_time = 0;
    }

    if (rs->down_time != 0) {
      rs->down_time = 0;
    }
  }

  rs_task_processing(rs, pin);

  if (rs->last_iterate_time - rs->tick_1s >= 1000) {  // 1000 == 1 sec.

    if (rs->last_position != rs->position) {
      rs->last_position = rs->position;
      channelValueChanged(rs->channel_number, (rs->position - 100) / 100);
    }

    if (rs->up_time > 600000 || rs->down_time > 600000) {  // 10 min. - timeout
      rs_set_relay(rs, pin, RS_RELAY_OFF, false, false);
    }

    if (rs->save_position) {
      rs->save_position = 0;
      rs_save_position(rs);
    }

    rs->tick_1s = millis();
  }

  rs->last_iterate_time = millis();
  rs_buttons_processing(rs);
}

void SuplaDeviceClass::iterate_impulse_counter(
    SuplaChannelPin *pin,
    TDS_SuplaDeviceChannel_C *channel,
    unsigned long time_diff,
    int channel_number) {
  if (channel->Type == SUPLA_CHANNELTYPE_IMPULSE_COUNTER &&
      pin->time_left <= 0) {
    pin->time_left = 5000;
    SuplaImpulseCounter *ptr =
        SuplaImpulseCounter::getCounterByChannel(channel_number);
    if (ptr && ptr->isChanged()) {
      _supla_int64_t value = ptr->getCounter();
      ptr->clearIsChanged();
      memcpy(channel->value, &value, 8);
      srpc_ds_async_channel_value_changed(srpc, channel_number, channel->value);
    }
  }
}

void SuplaDeviceClass::onTimer(void) {
  for (int a = 0; a < rs_count; a++) {
    iterate_rollershutter(
        &roller_shutter[a],
        &channel_pin[roller_shutter[a].channel_number],
        &Supla::Channel::reg_dev.channels[roller_shutter[a].channel_number]);
  }
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onTimer();
  }
}

void SuplaDeviceClass::onFastTimer(void) {
  // Iteration over all impulse counters will count incomming impulses. It is
  // after SuplaDevice initialization (because we have to read stored counter
  // values) and before any other operation like connection to Supla cloud
  // (because we want to count impulses even when we have connection issues.
  SuplaImpulseCounter::iterateAll();
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onFastTimer();
  }
}

void SuplaDeviceClass::iterate(void) {
  if (!isInitialized(false)) return;

  unsigned long _millis = millis();
  unsigned long time_diff = abs(_millis - last_iterate_time);

  // Iterate all elements
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->iterateAlways();
  }

  SuplaImpulseCounter::updateStorageOccasionally();

  if (wait_for_iterate != 0 && _millis < wait_for_iterate) {
    return;

  } else {
    wait_for_iterate = 0;
  }

  // Restart network after >1 min of failed connection attempts
  if (connectionFailCounter > 30) {
    connectionFailCounter = 0;
    supla_log(LOG_DEBUG,
              "Connection fail counter overflow. Trying to setup network "
              "interface again");
    Supla::Network::Setup();
    return;
  }

  if (!Supla::Network::IsReady()) {
    wait_for_iterate = millis() + 100;
    status(STATUS_NETWORK_DISCONNECTED, "No connection to network");
    networkIsNotReadyCounter++;
    if (networkIsNotReadyCounter > 20) {
      networkIsNotReadyCounter = 0;
      connectionFailCounter++;
    }
    return;
  }
  networkIsNotReadyCounter = 0;

  if (!Supla::Network::Connected()) {
    status(STATUS_SERVER_DISCONNECTED, "Not connected to Supla server");

    registered = 0;

    int result =
        Supla::Network::Connect(Supla::Channel::reg_dev.ServerName, port);
    if (1 == result) {
      connectionFailCounter = 0;
      supla_log(LOG_DEBUG, "Connected to Supla Server");
    } else {
      supla_log(LOG_DEBUG,
                "Connection fail (%d). Server: %s",
                result,
                Supla::Channel::reg_dev.ServerName);

      Supla::Network::Disconnect();
      wait_for_iterate = millis() + 2000;
      connectionFailCounter++;
      return;
    }
  }

  Supla::Network::Iterate();

  if (srpc_iterate(srpc) == SUPLA_RESULT_FALSE) {
    status(STATUS_ITERATE_FAIL, "Iterate fail");
    Supla::Network::Disconnect();

    wait_for_iterate = millis() + 5000;
    return;
  }

  if (registered == 0) {
    registered = -1;
    status(STATUS_REGISTER_IN_PROGRESS, "Register in progress");
    if (!srpc_ds_async_registerdevice_e(srpc, &Supla::Channel::reg_dev)) {
      supla_log(LOG_DEBUG, "Fatal SRPC failure!");
    }
    Supla::Channel::clearAllUpdateReady();

  } else if (registered == 1) {
    if (Supla::Network::Ping() == false) {
      supla_log(LOG_DEBUG, "TIMEOUT - lost connection with server");
      Supla::Network::Disconnect();
    }

    if (time_diff > 0) {
      // Iterate all elements
      for (auto element = Supla::Element::begin(); element != nullptr;
           element = element->next()) {
        if (!element->iterateConnected(srpc)) {
          break;
        }
      }

      for (int a = 0; a < channel_pin_count; a++) {
        iterate_relay(&channel_pin[a],
                      &Supla::Channel::reg_dev.channels[a],
                      time_diff,
                      a);
        iterate_impulse_counter(&channel_pin[a],
                                &Supla::Channel::reg_dev.channels[a],
                                time_diff,
                                a);
      }

      last_iterate_time = millis();
    }
  }
}

void SuplaDeviceClass::onVersionError(TSDC_SuplaVersionError *version_error) {
  status(STATUS_PROTOCOL_VERSION_ERROR, "Protocol version error");
  Supla::Network::Disconnect();

  wait_for_iterate = millis() + 5000;
}

void SuplaDeviceClass::onRegisterResult(
    TSD_SuplaRegisterDeviceResult *register_device_result) {
  _supla_int_t activity_timeout = 0;

  switch (register_device_result->result_code) {
    // OK scenario
    case SUPLA_RESULTCODE_TRUE:
      activity_timeout = register_device_result->activity_timeout;
      Supla::Network::Instance()->setActivityTimeout(activity_timeout);
      registered = 1;
      supla_log(LOG_DEBUG,
                "Device registered (activity timeout %d s, server version: %d, "
                "server min version: %d)",
                register_device_result->activity_timeout,
                register_device_result->version,
                register_device_result->version_min);
      last_iterate_time = millis();
      status(STATUS_REGISTERED_AND_READY, "Registered and ready.");

      if (activity_timeout != ACTIVITY_TIMEOUT) {
        supla_log(
            LOG_DEBUG, "Changing activity timeout to %d", ACTIVITY_TIMEOUT);
        TDCS_SuplaSetActivityTimeout at;
        at.activity_timeout = ACTIVITY_TIMEOUT;
        srpc_dcs_async_set_activity_timeout(srpc, &at);
      }

      return;

      // NOK scenarios
    case SUPLA_RESULTCODE_BAD_CREDENTIALS:
      status(STATUS_BAD_CREDENTIALS, "Bad credentials!");
      break;

    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      status(STATUS_TEMPORARILY_UNAVAILABLE, "Temporarily unavailable!");
      break;

    case SUPLA_RESULTCODE_LOCATION_CONFLICT:
      status(STATUS_LOCATION_CONFLICT, "Location conflict!");
      break;

    case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
      status(STATUS_CHANNEL_CONFLICT, "Channel conflict!");
      break;
    case SUPLA_RESULTCODE_DEVICE_DISABLED:
      status(STATUS_DEVICE_IS_DISABLED, "Device is disabled!");
      break;

    case SUPLA_RESULTCODE_LOCATION_DISABLED:
      status(STATUS_LOCATION_IS_DISABLED, "Location is disabled!");
      break;

    case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
      status(STATUS_DEVICE_LIMIT_EXCEEDED, "Device limit exceeded!");
      break;

    case SUPLA_RESULTCODE_GUID_ERROR:
      status(STATUS_INVALID_GUID, "Incorrect device GUID!");
      break;

    case SUPLA_RESULTCODE_AUTHKEY_ERROR:
      status(STATUS_INVALID_GUID, "Incorrect AuthKey!");
      break;

    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      status(STATUS_INVALID_GUID, "Registration disabled!");
      break;

    case SUPLA_RESULTCODE_NO_LOCATION_AVAILABLE:
      status(STATUS_INVALID_GUID, "No location available!");
      break;

    case SUPLA_RESULTCODE_USER_CONFLICT:
      status(STATUS_INVALID_GUID, "User conflict!");
      break;

    default:
      supla_log(LOG_ERR,
                "Register result code %i",
                register_device_result->result_code);
      break;
  }

  Supla::Network::Disconnect();
  wait_for_iterate = millis() + 5000;
}

void SuplaDeviceClass::channelValueChanged(int channel_number, char v) {
  if (srpc != NULL && registered == 1) {
    char value[SUPLA_CHANNELVALUE_SIZE];
    memset(value, 0, SUPLA_CHANNELVALUE_SIZE);

    value[0] = v;
    memcpy(Supla::Channel::reg_dev.channels[channel_number].value, value, 8);

    supla_log(
        LOG_DEBUG, "Value changed (channel: %d, value: %d)", channel_number, v);

    srpc_ds_async_channel_value_changed(srpc, channel_number, value);
  }
}

void SuplaDeviceClass::channelSetValue(int channel,
                                       char value,
                                       _supla_int_t DurationMS) {
  bool success = false;

  uint8_t _HI = channel_pin[channel].hiIsLo ? LOW : HIGH;
  uint8_t _LO = channel_pin[channel].hiIsLo ? HIGH : LOW;

  if (Supla::Channel::reg_dev.channels[channel].Type ==
      SUPLA_CHANNELTYPE_RELAY) {
    if (channel_pin[channel].bistable) {
      // ignore change of bistable relay state if we are in the middle of
      // changing its state or it already has target state enabled
      if (channel_pin[channel].bi_time_left > 0 ||
          Supla::Io::digitalRead(
              Supla::Channel::reg_dev.channels[channel].Number,
              channel_pin[channel].pin2) == value) {
        value = -1;
      } else {
        // set bistable relay controlling pin to "1" for 0.5 s to toggle
        // bistable relay state
        value = 1;
        channel_pin[channel].bi_time_left = 500;
      }
    }

    if (value == 0) {
      if (channel_pin[channel].pin1 != 0) {
        Supla::Io::digitalWrite(
            Supla::Channel::reg_dev.channels[channel].Number,
            channel_pin[channel].pin1,
            _LO);
        success = Supla::Io::digitalRead(
                      Supla::Channel::reg_dev.channels[channel].Number,
                      channel_pin[channel].pin1) == _LO;
      }

      if (channel_pin[channel].pin2 != 0 &&
          channel_pin[channel].bistable == false) {
        Supla::Io::digitalWrite(
            Supla::Channel::reg_dev.channels[channel].Number,
            channel_pin[channel].pin2,
            _LO);
        if (!success) {
          success = Supla::Io::digitalRead(
                        Supla::Channel::reg_dev.channels[channel].Number,
                        channel_pin[channel].pin2) == _LO;
        }
      }

    } else if (value == 1) {
      if (channel_pin[channel].pin2 != 0 &&
          channel_pin[channel].bistable == false) {
        Supla::Io::digitalWrite(
            Supla::Channel::reg_dev.channels[channel].Number,
            channel_pin[channel].pin2,
            _LO);
        delay(50);
      }
      if (channel_pin[channel].pin1 != 0) {
        Supla::Io::digitalWrite(
            Supla::Channel::reg_dev.channels[channel].Number,
            channel_pin[channel].pin1,
            _HI);
        if (!success) {
          success = Supla::Io::digitalRead(
                        Supla::Channel::reg_dev.channels[channel].Number,
                        channel_pin[channel].pin1) == _HI;
        }

        if (DurationMS > 0) {
          channel_pin[channel].time_left = DurationMS;
        }
      }
    }
    if (success) {
      Supla::Channel::reg_dev.channels[channel].value[0] = value;
    }

    if (channel_pin[channel].bistable) {
      success = false;
      delay(50);
    }
  };

  if (success && registered == 1 && srpc) {
    channelValueChanged(Supla::Channel::reg_dev.channels[channel].Number,
                        value);
  }
}

SuplaDeviceRollerShutter *SuplaDeviceClass::rsByChannelNumber(
    int channel_number) {
  for (int a = 0; a < rs_count; a++) {
    if (roller_shutter[a].channel_number == channel_number) {
      return &roller_shutter[a];
    }
  }

  return NULL;
}

void SuplaDeviceClass::channelSetValueByServer(
    TSD_SuplaChannelNewValue *new_value) {
  for (int a = 0; a < Supla::Channel::reg_dev.channel_count; a++)
    if (new_value->ChannelNumber ==
        Supla::Channel::reg_dev.channels[a].Number) {
      if (Supla::Channel::reg_dev.channels[a].Type == SUPLA_CHANNELTYPE_RELAY) {
        // Control rollet shutter by server
        if (Supla::Channel::reg_dev.channels[a].FuncList ==
            SUPLA_BIT_FUNC_CONTROLLINGTHEROLLERSHUTTER) {
          SuplaDeviceRollerShutter *rs =
              rsByChannelNumber(new_value->ChannelNumber);
          if (rs != NULL) {
            char v = new_value->value[0];

            unsigned long ct = new_value->DurationMS & 0xFFFF;
            unsigned long ot = (new_value->DurationMS >> 16) & 0xFFFF;

            if (ct < 0) {
              ct = 0;
            }

            if (ot < 0) {
              ot = 0;
            }

            ct *= 100;
            ot *= 100;

            if (ct != rs->full_closing_time || ot != rs->full_opening_time) {
              rs->full_closing_time = ct;
              rs->full_opening_time = ot;
              rs->position = -1;

              rs_save_settings(rs);
              rs->save_position = 1;
            }

            if (v >= 10 && v <= 110) {
              rs_add_task(rs, v - 10);
            } else {
              if (v == 1) {
                rs_set_relay(rs->channel_number, RS_RELAY_DOWN);
              } else if (v == 2) {
                rs_set_relay(rs->channel_number, RS_RELAY_UP);
              } else {
                rs_set_relay(rs->channel_number, RS_RELAY_OFF);
              }
            }
          }

          // Control relay by server
        } else {
          channelSetValue(new_value->ChannelNumber,
                          new_value->value[0],
                          new_value->DurationMS);
        }
      }
      break;
    }
}

void SuplaDeviceClass::channelSetActivityTimeoutResult(
    TSDC_SuplaSetActivityTimeoutResult *result) {
  Supla::Network::Instance()->setActivityTimeout(result->activity_timeout);
  supla_log(
      LOG_DEBUG, "Activity timeout set to %d s", result->activity_timeout);
}

bool SuplaDeviceClass::relayOn(int channel_number, _supla_int_t DurationMS) {
  channelSetValue(channel_number, HIGH, DurationMS);
}

bool SuplaDeviceClass::relayOff(int channel_number) {
  channelSetValue(channel_number, LOW, 0);
}

void SuplaDeviceClass::rollerShutterReveal(int channel_number) {
  rs_set_relay(channel_number, RS_RELAY_UP);
}

void SuplaDeviceClass::rollerShutterShut(int channel_number) {
  rs_set_relay(channel_number, RS_RELAY_DOWN);
}

void SuplaDeviceClass::rollerShutterStop(int channel_number) {
  rs_set_relay(channel_number, RS_RELAY_OFF);
}

bool SuplaDeviceClass::rollerShutterMotorIsOn(int channel_number) {
  return channel_number < Supla::Channel::reg_dev.channel_count &&
         (suplaDigitalRead_isHI(channel_number,
                                channel_pin[channel_number].pin1) ||
          suplaDigitalRead_isHI(channel_number,
                                channel_pin[channel_number].pin2));
}

void SuplaDeviceClass::setServerPort(int value) {
  port = value;
}

SuplaDeviceClass SuplaDevice;
