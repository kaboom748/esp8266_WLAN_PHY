#include <ModemFirmware.h>
AirModem::Firmware modem(2);
void setup() { modem.begin(); }
void loop() { modem.poll(); }
