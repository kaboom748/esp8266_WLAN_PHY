#include <ModemFirmware.h>
AirModem::Firmware modem(1);
void setup() { modem.begin(); }
void loop() { modem.poll(); }
