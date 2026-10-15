#include "sensors.h"
#include "strlib.h"
#include "uart0.h"

void initSensors() {

}

uint16_t readSensor1() {
    return 23;
}

void printSensorData() {
    uint16_t sensor1_val = readSensor1();
    char out[10];
    putsUart0("Sensor 1: ");
    to_string(sensor1_val, out, 10);
    putsUart0(out);
    putsUart0("\n");
}
