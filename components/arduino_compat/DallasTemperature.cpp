#include "DallasTemperature.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <algorithm>

#define DS18B20_CMD_CONVERTTEMP     0x44
#define DS18B20_CMD_RSCRATCHPAD     0xBE
#define DS18B20_CMD_WSCRATCHPAD     0x4E
#define DS18B20_CMD_CPYSCRATCHPAD   0x48
#define DS18B20_CMD_RECALL          0xB8
#define DS18B20_CMD_RPWRSUPPLY      0xB4
#define DS18B20_CMD_SEARCHROM       0xF0
#define DS18B20_CMD_READROM         0x33
#define DS18B20_CMD_MATCHROM        0x55
#define DS18B20_CMD_SKIPROM         0xCC
#define DS18B20_CMD_ALARMSEARCH     0xEC

#define MAX_SENSORS 20

DallasTemperature::DallasTemperature(OneWire* wire)
    : _wire(wire), _waitForConversion(true), _deviceCount(0),
      _resolution(12)
{
    memset(_devices, 0, sizeof(_devices));
}

void DallasTemperature::begin() {
    _deviceCount = 0;
    DeviceAddress addr;
    _wire->reset_search();
    while (_wire->search(addr) && _deviceCount < MAX_SENSORS) {
        if (OneWire::crc8(addr, 7) == addr[7]) {
            memcpy(_devices[_deviceCount], addr, 8);
            _deviceCount++;
        }
    }
}

uint8_t DallasTemperature::getDeviceCount() {
    return _deviceCount;
}

bool DallasTemperature::getAddress(DeviceAddress& addr, uint8_t idx) {
    if (idx >= _deviceCount) return false;
    memcpy(addr, _devices[idx], 8);
    return true;
}

void DallasTemperature::requestTemperatures() {
    _wire->reset();
    _wire->skip();
    _wire->write(DS18B20_CMD_CONVERTTEMP);

    if (_waitForConversion) {
        unsigned int delay = 94;
        switch (_resolution) {
            case 9:  delay = 94; break;
            case 10: delay = 188; break;
            case 11: delay = 375; break;
            default: delay = 750; break;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

float DallasTemperature::getTempC(const DeviceAddress& addr) {
    _wire->reset();
    _wire->select(addr);
    _wire->write(DS18B20_CMD_RSCRATCHPAD);

    uint8_t data[9];
    for (int i = 0; i < 9; i++) {
        data[i] = _wire->read();
    }

    if (OneWire::crc8(data, 8) != data[8]) {
        return -127.0;
    }

    int16_t raw = (data[1] << 8) | data[0];
    return raw * 0.0625f;
}

void DallasTemperature::setResolution(uint8_t resolution) {
    _resolution = std::min(std::max(resolution, (uint8_t)9), (uint8_t)12);
    for (uint8_t i = 0; i < _deviceCount; i++) {
        setResolution(_devices[i], _resolution);
    }
}

void DallasTemperature::setResolution(const DeviceAddress& addr, uint8_t resolution) {
    resolution = std::min(std::max(resolution, (uint8_t)9), (uint8_t)12);

    _wire->reset();
    _wire->select(addr);
    _wire->write(DS18B20_CMD_RSCRATCHPAD);

    uint8_t data[9];
    for (int i = 0; i < 9; i++) {
        data[i] = _wire->read();
    }

    uint8_t newConfig = ((resolution - 9) << 5) | 0x1F;

    _wire->reset();
    _wire->select(addr);
    _wire->write(DS18B20_CMD_WSCRATCHPAD);
    _wire->write(data[2]);
    _wire->write(data[3]);
    _wire->write(newConfig);
}

void DallasTemperature::setWaitForConversion(bool wait) {
    _waitForConversion = wait;
}
