#include "Arduino.h"
#include "ESP8266TDM.h"
extern "C" {
#include "user_interface.h"
}

#ifndef NOEUD_A
#define NOEUD_A 0 // 1 sur ESP A, 0 sur ESP B.
#endif

RH_ESP8266FSK radio;
ESP8266TDM liaison(radio);
const char local = NOEUD_A ? 'A' : 'B';
const char distant = NOEUD_A ? 'B' : 'A';
char texte[ESP8266TDM::MAX_PAYLOAD];
char avis[96] = {};
uint8_t longueur = 0;
bool tropLong = false, pret = false;
uint32_t acquittements = 0;

void inviter() {
  Serial.printf("ESP %c : tape un message pour %c (1 a 14 octets), puis Entree.\n", local, distant);
}

void lireClavier() {
  // Un caractere par tour : aucune attente bloquante pendant la reception RF.
  if (avis[0] || !Serial.available() || radio.mode() == RHGenericDriver::RHModeTx) return;
  const char c = Serial.read();
  if (c == '\r') return;
  if (c != '\n') {
    if (longueur < sizeof(texte)) texte[longueur++] = c;
    else tropLong = true;
    return;
  }

  uint16_t id = 0;
  if (tropLong) {
    snprintf(avis, sizeof(avis), "Refuse : maximum 14 octets. Rien n'a ete envoye.");
  } else if (longueur) {
    if (liaison.send(reinterpret_cast<const uint8_t*>(texte), longueur, &id))
      snprintf(avis, sizeof(avis), "En file vers %c, id=%u (pas encore acquitte).", distant, id);
    else
      snprintf(avis, sizeof(avis), "File pleine : message non envoye, retape-le plus tard.");
  }
  longueur = 0;
  tropLong = false;
}

void setup() {
  Serial.begin(115200);
  system_update_cpu_freq(160);
  delay(250);
  Serial.println("\nMessagerie ESP8266 TDM - bibliotheque 0.4.1");
  Serial.println("Moniteur serie : 115200 bauds, fin de ligne LF.");
  inviter();
  pret = liaison.begin(NOEUD_A ? ESP8266TDM::MASTER : ESP8266TDM::FOLLOWER);
  if (!pret) Serial.println("ERREUR : initialisation radio impossible.");
}

void loop() {
  if (!pret) { delay(10); return; }
  liaison.poll();
  lireClavier();

  // Les affichages attendent une garde TDM pour ne pas perturber la radio.
  if (liaison.maintenanceWindow()) {
    if (avis[0]) {
      Serial.println(avis);
      avis[0] = 0;
      inviter();
    }
    ESP8266TDM::Message message;
    if (liaison.recv(message)) {
      Serial.printf("Recu de %c, id=%u : ", distant, message.id);
      Serial.write(message.data, message.length);
      Serial.println();
      inviter();
    }
    const uint32_t total = liaison.statistics().acknowledged;
    if (total != acquittements) {
      Serial.printf("ACK de %c : +%lu message(s) confirme(s), total=%lu.\n", distant,
                    (unsigned long)(total - acquittements), (unsigned long)total);
      acquittements = total;
    }
  }
  yield();
}
