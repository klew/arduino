# SUPLA project

[SUPLA](https://www.supla.org) is an open source project for home automation. 

# SuplaDevice library for Arduino IDE

SuplaDevice is a library for [Arduino IDE](https://www.arduino.cc/en/main/software) that allows to implement devices working with [Supla](https://www.supla.org).

## Hardware requirements

### Arduino Mega
SuplaDevice works with Arduino Mega boards. Currently Arduino Uno is not supported because RAM limitations. It should work on other Arduino boards with at least 8 kB of RAM.
Following network interfaces are supported:
* Ethernet Shield with W5100 chipset
* ENC28J60 (not recommended - see Supported hardware section)

Warning: WiFi shields are currently not supported

### ESP8266
ESP8266 boards are supported. Network connection is done via internal WiFi. Tested with ESP8266 boards 2.6.3.
Most probably it will work with other ESP8266 compatible boards. 

### ESP32
Experimental support for ESP32 boards is provided. Some issues seen with reconnection to WiFi router which requires further analysis.

## Installation

Before you start, you will need to:
1. install Arduino IDE,
2. install support for your board
3. install driver for your USB to serial converter device (it can be external device, or build in on your board)
4. make sure that communication over serial interface with your board is working (i.e. check some example Arduino application)
5. download and install this librarary by copying SuplaDevice folder into your Arduino library folder

Steps 1-4 are standard Arudino IDE setup procedures not related to SuplaDevice library. You can find many tutorials on Internet with detailed instructions. Tutorials doesn't have to be related in any way with Supla. 

After step 5 you should see Supla example applications in Arduino IDE examples. Select one and have fun! Example file requires adjustments before you compile them and upload to your board. Please read all comments in example and make proper adjustments. 

## Usage

### Network interfaces
Supported network interfaces for Arduino Mega:
* Ethernet Shield - with W5100 chipset. Include `<supla/network/ethernet_shield.h>` and add `Supla::EthernetShield ethernet;` as a global variable.
* ENC28J60 - it requires additional UIPEthenet library (https://github.com/ntruchsess/arduino_uip). Include `<supla/network/ENC28J60.h>` and 
add `Supla::ENC28J60 ethernet;` as a global variable. Warning: network initialization on this library is blocking. In case of missing ENC28J60 board
or some other problem with network, program will stuck on initialization and will not work until connection is properly esablished.
Second warning: UIPEthernet library is consuming few hundred of bytes of RAM memory more, compared to standard Ethernet library. 

Supported network interface for ESP8266:
* There is native WiFi controller. Include `<supla/network/esp_wifi.h>` and add `Supla::ESPWifi wifi(ssid, password);` as a global variable and provide SSID and password in constructor.

Supported network interface for ESP32:
* There is native WiFi controller. Include `<supla/network/esp32_wifi.h>` and add `Supla::ESP32Wifi wifi(ssid, password);` as a global variable and provide SSID and password in constructor.

### Exmaples

Each example can run on Arduino Mega, ESP8266, or ESP32 board. Please read comments in example files and uncomment proper library for your network interface.

SuplaSomfy, Supla_RollerShutter_FRAM - those examples are not updated yet.

### Folder structure

* `supla-common` - Supla protocol definitions and structures. There are also methods to handle low level communication with Supla server, like message coding, decoding, sending and receiving. Those files are common with `supla-core` and the same code is run on multiple Supla platforms and services
* `supla/network` - implementation of network interfaces for supported boards
* `supla/sensor` - implementation of Supla sensor channels (thermometers, open/close sensors, etc.)
* `supla/control` - implementation of Supla control channels (various combinations of relays, buttons, action triggers)

Some functions from above folders have dependencies to external libraries. Please check documentation included in header files.

### How does it work?

Everything that is visible in Supla (in Cloud, on API, mobile application) is called "channel". Supla channels are used to control relays, read temperature, check if garage door is open. 

SuplaDevice implements support for channels in `Channel` and `ChannelExtended` classes. Instances of those classes are part of objects called `Element` which are building blocks for any SuplaDevice application.

All sensors, relays, buttons objects inherits from `Element` class. Each instance of such object will automatically register in SuplaDevice and proper virtual methods will be called by SuplaDevice in a specified way.

All elements have to be constructed before `SuplaDevice.begin()` method is called.

Supla channel number is assigned to each elemement with channel in an order of creation of objects. First channel will get number 0, second 1, etc. Supla server will not accept registration of device when order of channels is changed, or some channel is removed. In such case, you should remove device from Supla Cloud and register it again from scratch.

`Element` class defines follwoing virtual methods that are called by SuplaDevice:
1. `onInit` - called within `SuplaDevice.begin()` method. It should: TODO...

...

## How to migrate programs written in SuplaDevice libraray versions 1.6 and older

For Arduino Mega applications include proper network interface header:
```
// Choose proper network interface for your card:
// Arduino Mega with EthernetShield W5100:
#include <supla/network/ethernet_shield.h>
// Ethernet MAC address
uint8_t mac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
Supla::EthernetShield ethernet(mac);
//
// Arduino Mega with ENC28J60:
// #include <supla/network/ENC28J60.h>
// Supla::ENC28J60 ethernet(mac);

```

For ESP8266 based applications include wifi header and provide WIFI SSID and password:
```
// ESP8266 based board:
#include <supla/network/esp_wifi.h>
Supla::ESPWifi wifi("your_wifi_ssid", "your_wifi_password");
```

In case of ESP8266 remove all methods that defined network interface. They were usually added on the bottom of ino file, after end of loop() method, i.e.:
```
// Supla.org ethernet layer
    int supla_arduino_tcp_read(void *buf, int count) {
...
SuplaDeviceCallbacks supla_arduino_get_callbacks(void) {
...
```

Remove also those lines:
```
#define SUPLADEVICE_CPP
WiFiClient client;
```
You may also remove all WIFI related includes from ino file.

Common instruction for all boards:

If you use local IP address, please provide it in constructor of your network inteface class, i.e.:
```
Supla::EthernetShield ethernet(mac, localIp);
```

After that go to SuplaDevice.begin() method. Old code looked like this
```
SuplaDevice.begin(GUID,              // Global Unique Identifier 
                  mac,               // Ethernet MAC address
                  "svr1.supla.org",  // SUPLA server address
                  locationId,                 // Location ID 
                  locationPassword);          // Location Password
```
This method requires now different set of parameters:
```
  SuplaDevice.begin(GUID,              // Global Unique Identifier 
                    "svr1.supla.org",  // SUPLA server address
                    "mail@address.pl",     // Email address
                    AUTHKEY);          // Authentication key
```
What is different? Well, GUID and Supla server address it the same as previously. MAC address, location ID, location password are removed.
MAC address was moved to network interface class. Location ID and password were replaced with new authentication method - via email address
and authentication key. You can generate your authentication key in the same way as GUID (it is actually in exactly the same format):
```
// Generate AUTHKEY from https://www.supla.org/arduino/get-guid
char AUTHKEY[SUPLA_AUTHKEY_SIZE] =  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
```

Next change is for custom digitalWrite and digitalRead methods. Those can be used to create virtual digital pins. Instead of adding custom
callback method that overrides digitalWrite/Read method, you should create a new class which inhertis from Supla::Io base class and define
your own customDigitalRead/Write methods. Here is short example (you can put this code in ino file, before setup()):
```
#include <supla/io.h>

class MyDigitalRead : public Supla::Io {
  public:
    int customDigitalRead(int channelNumber, uint8_t pin) {
      if (channelNumber == MY_EXTRA_VIRTUAL_CHANNEL) {
        return someCustomSourceOfData.getValue();
      } else {
        return ::digitalRead(pin);
      }
    }
}

MyDigitalRead instanceMyDigitalRead;
```



## History

Version 2.3.0


## Credits


## License


Please check it [here](https://github.com/SUPLA/supla-cloud/blob/master/LICENSE).

