#pragma once

#include <stdint.h>
#include "OneWire.h"

typedef uint8_t DeviceAddress[8];

class DallasTemperature {
public:
    DallasTemperature(OneWire* wire);
    void begin();
    void requestTemperatures();
    void setResolution(uint8_t resolution);
    void setResolution(const DeviceAddress& addr, uint8_t resolution);
    uint8_t getDeviceCount();
    float getTempC(const DeviceAddress& addr);
    bool getAddress(DeviceAddress& addr, uint8_t idx);
    void setWaitForConversion(bool wait);

private:
    OneWire* _wire;
    bool _waitForConversion;
    uint8_t _deviceCount;
    uint8_t _resolution;
    uint8_t _devices[20][8];
};
