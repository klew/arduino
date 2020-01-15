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

#include <SPI.h>
#include <Ethernet.h>
#include <SuplaDevice.h>
#include <SuplaImpulseCounter.h>

/*
 * This example requires DHT sensor library installed. 
 * https://github.com/adafruit/DHT-sensor-library
 */

void setup() {

  Serial.begin(9600);

  // ﻿Replace the falowing GUID
  char GUID[SUPLA_GUID_SIZE] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  // ﻿with GUID that you can retrieve from https://www.supla.org/arduino/get-guid


  // Ethernet MAC address
  uint8_t mac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};

  /*
   * Having your device already registered at cloud.supla.org,
   * you want to change CHANNEL sequence or remove any of them,
   * then you must also remove the device itself from cloud.supla.org.
   * Otherwise you will get "Channel conflict!" error.
   */
    
  // CHANNEL0 - Impulse Counter on pin 34, without status LED (it is not implemented yet), counting raising edge (from LOW to HIGH), no pullup on pin, and 10 ms debounce timeout
  SuplaDevice.addImpulseCounter(34, 0, true, false, 10);
  
  // CHANNEL1 - Impulse Counter on pin 34, without status LED (it is not implemented yet), counting folling edge (from HIGH to LOW), with pullup on pin, and 50 ms debounce timeout
  SuplaDevice.addImpulseCounter(35, 0, false, true, 50);
  

  SuplaImpulseCounter::clearStorage();


  /*
   * SuplaDevice Initialization.
   * Server address, LocationID and LocationPassword are available at https://cloud.supla.org 
   * If you do not have an account, you can create it at https://cloud.supla.org/account/create
   * SUPLA and SUPLA CLOUD are free of charge
   * 
   */

  SuplaDevice.begin(GUID,              // Global Unique Identifier 
                    mac,               // Ethernet MAC address
                    "svr1.supla.org",  // SUPLA server address
                    0,                 // Location ID 
                    "");               // Location Password
    
}

void loop() {
  SuplaDevice.iterate();
}