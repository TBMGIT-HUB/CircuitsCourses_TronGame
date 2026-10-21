/*
 * Snake Multijoueur pour XIAO ESP32-S3 + OLED 128x64 I2C + 8 boutons en matrice 3x3
 * Architecture Serveur / Client via WebSockets Wi-Fi (jusqu'a 4 joueurs : J1..J4)
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>

// ======================= CONFIGURATION ÉCRAN & CLAVIER =======================

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#define I2C_CLOCK_HZ 400000

#define DIODE_COL2ROW 1

const uint8_t COL_PINS[3] = { D7, D1, D6 };
const uint8_t ROW_PINS[3] = { D8, D10, D9 };

#if DIODE_COL2ROW
  #define ACTIVE_LEVEL HIGH
  #define ROW_MODE     INPUT_PULLDOWN
#else
  #define ACTIVE_LEVEL LOW
  #define ROW_MODE     INPUT_PULLUP
#endif

enum Key : uint8_t {
  K_UP, K_RIGHT, K_A,
  K_LEFT, K_SELECT, K_B,
  K_DOWN, K_START, K_NONE
};
const uint8_t KEY_COUNT = 8;
const uint8_t DEBOUNCE_MS = 8;

uint8_t stableMask = 0, lastRaw = 0, pressedEdge = 0;
uint32_t rawSince = 0;

uint8_t scanMatrix() {
  uint8_t raw = 0;
  for (uint8_t c = 0; c < 3; c++) {
    pinMode(COL_PINS[c], OUTPUT);
    digitalWrite(COL_PINS[c], ACTIVE_LEVEL);
    delayMicroseconds(10);
    for (uint8_t r = 0; r < 3; r++) {
      uint8_t k = r * 3 + c;
      if (k < KEY_COUNT && digitalRead(ROW_PINS[r]) == ACTIVE_LEVEL) raw |= (1 << k);
    }
    pinMode(COL_PINS[c], INPUT);
  }
  return raw;
}

void updateKeys() {
  uint8_t raw = scanMatrix();
  uint32_t now = millis();
  if (raw != lastRaw) {
    lastRaw = raw;
    rawSince = now;
  } else if (raw != stableMask && (now - rawSince) >= DEBOUNCE_MS) {
    pressedEdge |= (raw ^ stableMask) & raw;
    stableMask = raw;
  }
}

bool wasPressed(uint8_t k) { return pressedEdge & (1 << k); }

// ======================= RÉGLAGES WI-FI & MULTIJOUEUR =======================
const char* PASSWORD = "jeu12345";
const uint16_t PORT    = 81;
const int MAX_CLIENTS  = 3;   // 1 serveur (J1) + 3 clients (J2, J3, J4)

enum Role { ROLE_AUCUN, ROLE_SERVEUR, ROLE_CLIENT };
Role roleActuel = ROLE_AUCUN;

enum EtatMenu {
  MENU_PRINCIPAL,
  CONFIRM_CREATION,
  SCAN_RESEAUX,
  LISTE_PARTIES,
  CONFIRM_REJOINDRE,
  SALON_ATTENTE,
  PLACEMENT_INITIAL, // Affichage des positions avant le décompte
  DECOMPTE,
  EN_JEU,
  PAUSE_JEU,
  GAME_OVER,
  HOTE_PARTI
};
EtatMenu etatMenu = MENU_PRINCIPAL;

int indexMenuPrincipal = 0;
int indexPartie = 0;
int nombrePartiesTrouvees = 0;
String ssidsTrouves[10];

WebSocketsServer serveur(PORT);
WebSocketsClient client;
char ssidServeur[32];
bool dirty = true;

int myPlayerId = 0;                 // 0 = J1 (hote), 1 = J2, 2 = J3, 3 = J4
bool slotConnecte[4] = { true, false, false, false }; // cote serveur : qui est connecte
int participantsPartie = 1;         // nb de joueurs au debut de la manche
int gameOverChoice = 0;
int gagnant = -1;                   // -1 = pas de gagnant / egalite
String ssidAffiche = "";            // nom de la partie (cree ou rejointe)
bool idRecu = false;                // client : a-t-il recu son numero de joueur ?
int motifFin = 0;                   // 0 = hote parti, 1 = partie pleine / deja commencee

// Decompte non bloquant
uint32_t decompteStart = 0;
int decompteAffiche = -1;
uint32_t placementStart = 0;

int nbJoueurs() {
  int n = 0;
  for (int i = 0; i < 4; i++) if (slotConnecte[i]) n++;
  return n;
}

void broadcastTXT(const String& msg) {
  serveur.broadcastTXT(msg.c_str());
}

// ======================= MOTEUR SNAKE =======================
const int W = 30, H = 11; // Hauteur réduite de 1 case (11 au lieu de 12) pour laisser de la marge en bas
const int CELL = 4;
const int OX = 4, OY = 16;
const int MAXLEN = 100;

struct Snake {
  int bx[MAXLEN + 1], by[MAXLEN + 1];
  int len;
  int dir;   // direction reellement suivie au dernier pas : 1:Gauche 2:Droite 3:Haut 4:Bas 0:Stop
  int ndir;  // direction demandee, appliquee au prochain pas
  bool active;
};

Snake snakes[4];
int fx = 15, fy = 5;
uint32_t lastStep = 0;
const uint16_t STEP_MS = 130;
Preferences prefs;
uint32_t hiScore = 0;

void centerText(int y, const char* str) {
  int w = u8g2.getStrWidth(str);
  u8g2.drawStr((128 - w) / 2, y, str);
}

bool caseOccupee(int x, int y) {
  for (int p = 0; p < 4; p++) {
    if (!snakes[p].active) continue;
    for (int i = 0; i < snakes[p].len; i++) {
      if (snakes[p].bx[i] == x && snakes[p].by[i] == y) return true;
    }
  }
  return false;
}

void placerNourriture() {
  for (int essai = 0; essai < 100; essai++) {
    int x = random(W), y = random(H);
    if (!caseOccupee(x, y)) { fx = x; fy = y; return; }
  }
  fx = random(W);
  fy = random(H);
}

void initSnakeGame() {
  int cornerX[4] = { 3, W - 4, 3, W - 4 };
  int cornerY[4] = { 2, 2, H - 3, H - 3 };
  int startDir[4] = { 2, 1, 2, 1 };

  participantsPartie = nbJoueurs();
  gagnant = -1;

  for (int p = 0; p < 4; p++) {
    snakes[p].len = 3;
    snakes[p].active = slotConnecte[p];
    snakes[p].dir = snakes[p].ndir = snakes[p].active ? startDir[p] : 0;

    for (int i = 0; i < snakes[p].len; i++) {
      int offsetX = (startDir[p] == 2) ? -i : i;
      snakes[p].bx[i] = cornerX[p] + offsetX;
      snakes[p].by[i] = cornerY[p];
    }
  }
  placerNourriture();
  lastStep = millis();
}

// Déclaration anticipée
void broadcastGameState();

void lancerPlacementInitial() {
  if (roleActuel == ROLE_SERVEUR) {
    initSnakeGame();
    broadcastGameState();
  }
  etatMenu = PLACEMENT_INITIAL;
  placementStart = millis();
  dirty = true;
}

void lancerDecompte() {
  etatMenu = DECOMPTE;
  decompteStart = millis();
  decompteAffiche = -1;
  dirty = true;
}

void quitterEnClient() {
  client.disconnect();
  delay(150);
  ESP.restart();
}

void fermerServeur() {
  broadcastTXT("HOST_LEFT");
  uint32_t t0 = millis();
  while (millis() - t0 < 300) { serveur.loop(); delay(5); }
  serveur.close();
  delay(50);
  ESP.restart();
}

void hoteParti() {
  if (etatMenu == HOTE_PARTI) return;
  motifFin = idRecu ? 0 : 1;
  etatMenu = HOTE_PARTI;
  dirty = true;
}

void setDirection(int p, int newDir) {
  if (p < 0 || p > 3 || !snakes[p].active) return;
  if (newDir < 1 || newDir > 4) return;
  int cur = snakes[p].dir;
  bool opposite = (cur == 1 && newDir == 2) || (cur == 2 && newDir == 1) ||
                  (cur == 3 && newDir == 4) || (cur == 4 && newDir == 3);
  if (!opposite) snakes[p].ndir = newDir;
}

// ======================= RÉSEAU SNAKE =======================

const char* readInt(const char* p, int& v) {
  v = 0;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  if (neg) v = -v;
  return p;
}

const char* skipSep(const char* p) {
  if (*p) p++;
  return p;
}

void broadcastGameState() {
  String packet;
  packet.reserve(700);
  packet = "STATE:" + String(fx) + "," + String(fy);
  for (int p = 0; p < 4; p++) {
    packet += ";" + String(p) + "," + String(snakes[p].active ? 1 : 0) + "," + String(snakes[p].len);
    for (int i = 0; i < snakes[p].len; i++) {
      packet += "," + String(snakes[p].bx[i]) + "," + String(snakes[p].by[i]);
    }
  }
  broadcastTXT(packet);
}

void sendClientDirection(int dir) {
  String packet = "DIR:" + String(myPlayerId) + "," + String(dir);
  client.sendTXT(packet);
}

void parseClientMessage(int slot, const char* msg) {
  if (strncmp(msg, "DIR:", 4) == 0) {
    int id, d;
    const char* p = readInt(msg + 4, id);
    p = skipSep(p);
    readInt(p, d);
    setDirection(slot, d);
  }
}

void parseServerMessage(const char* msg) {
  if (strncmp(msg, "ID:", 3) == 0) {
    int id;
    readInt(msg + 3, id);
    if (id >= 0 && id < 4) myPlayerId = id;
    idRecu = true;
    dirty = true;
  }
  else if (strcmp(msg, "START_GAME") == 0 || strcmp(msg, "RESET_GAME") == 0) {
    lancerPlacementInitial();
  }
  else if (strcmp(msg, "PAUSE_GAME") == 0) {
    etatMenu = PAUSE_JEU;
    dirty = true;
  }
  else if (strcmp(msg, "RESUME_GAME") == 0) {
    etatMenu = EN_JEU;
    dirty = true;
  }
  else if (strcmp(msg, "HOST_LEFT") == 0) {
    hoteParti();
  }
  else if (strncmp(msg, "STATE:", 6) == 0) {
    const char* p = msg + 6;
    p = readInt(p, fx);
    p = skipSep(p);
    p = readInt(p, fy);

    while (*p == ';') {
      p++;
      int pId, act, len;
      p = readInt(p, pId);  p = skipSep(p);
      p = readInt(p, act);  p = skipSep(p);
      p = readInt(p, len);
      if (pId < 0 || pId > 3 || len < 0 || len > MAXLEN) break;
      snakes[pId].active = (act != 0);
      snakes[pId].len = len;
      for (int i = 0; i < len; i++) {
        int x, y;
        p = skipSep(p); p = readInt(p, x);
        p = skipSep(p); p = readInt(p, y);
        snakes[pId].bx[i] = x;
        snakes[pId].by[i] = y;
      }
    }
    if (etatMenu == DECOMPTE) {
      etatMenu = EN_JEU;
    }
    dirty = true;
  }
  else if (strncmp(msg, "GAMEOVER_SCREEN", 15) == 0) {
    gagnant = -1;
    if (msg[15] == ':') readInt(msg + 16, gagnant);
    etatMenu = GAME_OVER;
    gameOverChoice = 0;
    dirty = true;
  }
}

// ======================= CALLBACKS WEBSOCKETS =======================
void surEvenementServeur(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  (void)length;
  switch (type) {
    case WStype_CONNECTED: {
      if (num >= MAX_CLIENTS || etatMenu != SALON_ATTENTE) {
        serveur.disconnect(num);
        break;
      }
      int slot = num + 1;
      slotConnecte[slot] = true;
      char buf[8];
      snprintf(buf, sizeof(buf), "ID:%d", slot);
      serveur.sendTXT(num, buf);
      dirty = true;
      break;
    }
    case WStype_DISCONNECTED:
      if (num < MAX_CLIENTS && slotConnecte[num + 1]) {
        slotConnecte[num + 1] = false;
        snakes[num + 1].active = false;
        if (etatMenu == EN_JEU && participantsPartie > 1) participantsPartie--;
        dirty = true;
      }
      break;
    case WStype_TEXT:
      parseClientMessage(num + 1, (const char*)payload);
      break;
    default: break;
  }
}

void surEvenementClient(WStype_t type, uint8_t* payload, size_t length) {
  (void)length;
  switch (type) {
    case WStype_TEXT:
      parseServerMessage((const char*)payload);
      break;
    case WStype_DISCONNECTED:
      hoteParti();
      break;
    default: break;
  }
}

// ======================= PHYSIQUE DU SNAKE (SERVEUR) =======================
void updateServerSnakePhysics() {
  uint32_t now = millis();
  if (now - lastStep < STEP_MS) return;
  lastStep = now;

  for (int p = 0; p < 4; p++) {
    if (!snakes[p].active) continue;
    snakes[p].dir = snakes[p].ndir;
    if (snakes[p].dir == 0) continue;

    int nx = snakes[p].bx[0], ny = snakes[p].by[0];
    if (snakes[p].dir == 1) nx--;
    else if (snakes[p].dir == 2) nx++;
    else if (snakes[p].dir == 3) ny--;
    else if (snakes[p].dir == 4) ny++;

    bool collision = (nx < 0 || nx >= W || ny < 0 || ny >= H);
    for (int o = 0; o < 4 && !collision; o++) {
      if (!snakes[o].active) continue;
      int checkLen = (p == o) ? snakes[o].len - 1 : snakes[o].len;
      for (int i = 0; i < checkLen; i++) {
        if (snakes[o].bx[i] == nx && snakes[o].by[i] == ny) {
          collision = true;
          break;
        }
      }
    }

    if (collision) {
      snakes[p].active = false;
      continue;
    }

    bool eat = (nx == fx && ny == fy);

    for (int i = snakes[p].len - 1; i > 0; i--) {
      snakes[p].bx[i] = snakes[p].bx[i - 1];
      snakes[p].by[i] = snakes[p].by[i - 1];
    }
    snakes[p].bx[0] = nx;
    snakes[p].by[0] = ny;

    if (eat) {
      if (snakes[p].len < MAXLEN) {
        snakes[p].bx[snakes[p].len] = snakes[p].bx[snakes[p].len - 1];
        snakes[p].by[snakes[p].len] = snakes[p].by[snakes[p].len - 1];
        snakes[p].len++;
      }
      placerNourriture();
    }
  }

  int vivants = 0, dernier = -1;
  for (int p = 0; p < 4; p++) {
    if (snakes[p].active) { vivants++; dernier = p; }
  }
  bool fin = (participantsPartie <= 1) ? (vivants == 0) : (vivants <= 1);

  broadcastGameState();
  dirty = true;

  if (fin) {
    gagnant = (participantsPartie > 1 && vivants == 1) ? dernier : -1;
    etatMenu = GAME_OVER;
    gameOverChoice = 0;
    broadcastTXT("GAMEOVER_SCREEN:" + String(gagnant));
  }
}

// ======================= AFFICHAGE DES MENUS & JEU =======================
void afficherMenuOLED() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  switch (etatMenu) {
    case MENU_PRINCIPAL:
      u8g2.drawStr(14, 10, "=== MENU JEU ===");
      u8g2.drawStr(10, 30, (indexMenuPrincipal == 0) ? "> 1. Creer Partie" : "  1. Creer Partie");
      u8g2.drawStr(10, 45, (indexMenuPrincipal == 1) ? "> 2. Rejoindre" : "  2. Rejoindre");
      u8g2.setFont(u8g2_font_4x6_tr);
      u8g2.drawStr(10, 60, "Haut/Bas:Nav  A:Valider");
      break;

    case CONFIRM_CREATION:
      u8g2.drawStr(10, 15, "Creer une partie ?");
      u8g2.drawStr(15, 35, "A : OUI (Valider)");
      u8g2.drawStr(15, 50, "B : NON (Annuler)");
      break;

    case SCAN_RESEAUX:
      centerText(30, "Recherche de");
      centerText(42, "parties en cours...");
      break;

    case LISTE_PARTIES:
      u8g2.drawStr(5, 12, "Parties trouvees :");
      if (nombrePartiesTrouvees == 0) {
        centerText(35, "Aucune partie !");
        u8g2.drawStr(5, 60, "B : Retour");
      } else {
        for (int i = 0; i < nombrePartiesTrouvees && i < 4; i++) {
          char ligne[40];
          snprintf(ligne, sizeof(ligne), "%s %s", (indexPartie == i) ? ">" : " ", ssidsTrouves[i].c_str());
          u8g2.drawStr(5, 25 + (i * 10), ligne);
        }
        u8g2.drawStr(5, 62, "A: Select  B: Retour");
      }
      break;

    case CONFIRM_REJOINDRE:
      u8g2.drawStr(10, 10, "Rejoindre :");
      u8g2.drawStr(10, 25, ssidsTrouves[indexPartie].c_str());
      u8g2.drawStr(10, 45, "A : OUI | B : NON");
      break;

    case SALON_ATTENTE: {
      char buffer[32];
      if (roleActuel == ROLE_SERVEUR) {
        u8g2.drawStr(5, 10, "SERVEUR (HOTE)");
        u8g2.drawStr(5, 25, ssidAffiche.c_str());
        snprintf(buffer, sizeof(buffer), "Joueurs: %d/4", nbJoueurs());
        u8g2.drawStr(5, 38, buffer);
        u8g2.drawStr(5, 50, "> VOUS ETES J1 <");
        u8g2.setFont(u8g2_font_4x6_tr);
        u8g2.drawStr(5, 60, "START:lancer   B:retour menu");
      } else {
        u8g2.drawStr(5, 10, "CLIENT CONNECTE");
        u8g2.drawStr(5, 25, ssidAffiche.c_str());
        snprintf(buffer, sizeof(buffer), "> VOUS ETES J%d <", myPlayerId + 1);
        u8g2.drawStr(5, 38, buffer);
        u8g2.setFont(u8g2_font_4x6_tr);
        u8g2.drawStr(5, 50, "Attente de l'hote...");
        u8g2.drawStr(5, 60, "B : quitter");
      }
      break;
    }

    case PLACEMENT_INITIAL: {
      u8g2.drawFrame(OX - 1, OY - 1, W * CELL + 2, H * CELL + 2);
      
      for (int p = 0; p < 4; p++) {
        if (!snakes[p].active) continue;
        for (int i = 0; i < snakes[p].len; i++) {
          int x = OX + snakes[p].bx[i] * CELL;
          int y = OY + snakes[p].by[i] * CELL;
          u8g2.drawBox(x, y, CELL - 1, CELL - 1);
        }
        int headX = OX + snakes[p].bx[0] * CELL;
        int headY = OY + snakes[p].by[0] * CELL;
        char label[4];
        snprintf(label, sizeof(label), "J%d", p + 1);
        u8g2.setFont(u8g2_font_4x6_tr);
        int textX = headX + (snakes[p].bx[0] < W / 2 ? 5 : -12);
        int textY = headY + (snakes[p].by[0] < H / 2 ? 8 : -2);
        u8g2.drawStr(textX, textY, label);
      }
      
      char infoJoueur[24];
      snprintf(infoJoueur, sizeof(infoJoueur), "Positions (Vous: J%d)", myPlayerId + 1);
      u8g2.setFont(u8g2_font_4x6_tr);
      u8g2.drawStr(2, 7, infoJoueur);
      break;
    }

    case DECOMPTE: {
      const char* etapes[] = { "3", "2", "1", "GO !" };
      int s = decompteAffiche;
      if (s < 0) s = 0;
      if (s > 3) s = 3;
      u8g2.setFont(u8g2_font_helvB14_tr);
      centerText(38, etapes[s]);
      char buffer[24];
      snprintf(buffer, sizeof(buffer), "Vous etes J%d", myPlayerId + 1);
      u8g2.setFont(u8g2_font_4x6_tr);
      centerText(58, buffer);
      break;
    }

    case EN_JEU: {
      u8g2.drawFrame(OX - 1, OY - 1, W * CELL + 2, H * CELL + 2);
      u8g2.drawFrame(OX + fx * CELL, OY + fy * CELL, CELL, CELL);

      for (int p = 0; p < 4; p++) {
        if (!snakes[p].active) continue;
        bool moi = (p == myPlayerId);
        for (int i = 1; i < snakes[p].len; i++) {
          int x = OX + snakes[p].bx[i] * CELL;
          int y = OY + snakes[p].by[i] * CELL;
          if (moi) u8g2.drawBox(x, y, CELL - 1, CELL - 1);
          else     u8g2.drawFrame(x, y, CELL - 1, CELL - 1);
        }
        u8g2.drawBox(OX + snakes[p].bx[0] * CELL, OY + snakes[p].by[0] * CELL, CELL, CELL);
      }

      char infoJoueur[16];
      snprintf(infoJoueur, sizeof(infoJoueur), "J%d (Vous)", myPlayerId + 1);
      u8g2.setFont(u8g2_font_4x6_tr);
      u8g2.drawStr(2, 7, infoJoueur);
      break;
    }

    case PAUSE_JEU: {
      u8g2.drawFrame(OX - 1, OY - 1, W * CELL + 2, H * CELL + 2);
      u8g2.setFont(u8g2_font_helvB14_tr);
      centerText(38, "PAUSE");
      u8g2.setFont(u8g2_font_4x6_tr);
      centerText(52, roleActuel == ROLE_SERVEUR ? "START: Reprendre" : "Attente de l'hote...");
      break;
    }

    case GAME_OVER: {
      u8g2.drawStr(24, 11, "FIN DE PARTIE");
      char buffer[24];
      if (gagnant >= 0) snprintf(buffer, sizeof(buffer), "J%d GAGNE !", gagnant + 1);
      else              snprintf(buffer, sizeof(buffer), "Pas de gagnant");
      centerText(24, buffer);

      if (roleActuel == ROLE_SERVEUR) {
        u8g2.drawStr(15, 38, (gameOverChoice == 0) ? "> Recommencer" : "  Recommencer");
        u8g2.drawStr(15, 50, (gameOverChoice == 1) ? "> Arreter" : "  Arreter");
        u8g2.setFont(u8g2_font_4x6_tr);
        u8g2.drawStr(5, 62, "Haut/Bas:Nav  A:Valider");
      } else {
        u8g2.drawStr(5, 38, "Attente de l'hote");
        u8g2.setFont(u8g2_font_4x6_tr);
        u8g2.drawStr(5, 62, "B : Quitter la partie");
      }
      break;
    }

    case HOTE_PARTI:
      u8g2.drawStr(5, 11, "!! HOTE PARTI !!");
      if (motifFin == 0) {
        u8g2.drawStr(5, 26, "L'hote a quitte la");
        u8g2.drawStr(5, 38, "partie. Le serveur");
        u8g2.drawStr(5, 50, "n'existe plus.");
      } else {
        u8g2.drawStr(5, 26, "Connexion refusee:");
        u8g2.drawStr(5, 38, "partie pleine ou");
        u8g2.drawStr(5, 50, "deja commencee.");
      }
      u8g2.setFont(u8g2_font_4x6_tr);
      u8g2.drawStr(5, 62, "A : Retour au menu");
      break;
  }
  u8g2.sendBuffer();
}

// ======================= DÉMARRAGE RÉSEAU =======================
void demarrerServeur() {
  roleActuel = ROLE_SERVEUR;
  myPlayerId = 0;
  for (int i = 0; i < 4; i++) slotConnecte[i] = (i == 0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();

  uint8_t mac[6];
  WiFi.macAddress(mac);

  bool nomUnique = false;
  int tentative = 0;
  while (!nomUnique && tentative < 10) {
    if (tentative == 0) {
      snprintf(ssidServeur, sizeof(ssidServeur), "ESP32_GAME_%02X%02X", mac[4], mac[5]);
    } else {
      snprintf(ssidServeur, sizeof(ssidServeur), "ESP32_GAME_%02X%02X%d", mac[4], mac[5], (int)random(10, 99));
    }
    nomUnique = true;
    for (int i = 0; i < n; ++i) {
      if (WiFi.SSID(i) == String(ssidServeur)) { nomUnique = false; break; }
    }
    tentative++;
  }
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssidServeur, PASSWORD, 1, 0, MAX_CLIENTS);

  ssidAffiche = String(ssidServeur);
  serveur.begin();
  serveur.onEvent(surEvenementServeur);
  serveur.enableHeartbeat(3000, 2000, 2);
  etatMenu = SALON_ATTENTE;
  dirty = true;
}

void scannerEtListerParties() {
  etatMenu = SCAN_RESEAUX;
  afficherMenuOLED();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  nombrePartiesTrouvees = 0;
  indexPartie = 0;

  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.startsWith("ESP32_GAME_") && nombrePartiesTrouvees < 10) {
      ssidsTrouves[nombrePartiesTrouvees++] = ssid;
    }
  }
  WiFi.scanDelete();
  etatMenu = LISTE_PARTIES;
  dirty = true;
}

void demarrerClient() {
  String targetSSID = ssidsTrouves[indexPartie];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  centerText(35, "Connexion...");
  u8g2.sendBuffer();

  WiFi.begin(targetSSID.c_str(), PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    etatMenu = MENU_PRINCIPAL;
    dirty = true;
    return;
  }

  roleActuel = ROLE_CLIENT;
  myPlayerId = 0;
  idRecu = false;
  ssidAffiche = targetSSID;
  client.begin(WiFi.gatewayIP(), PORT, "/");
  client.onEvent(surEvenementClient);
  client.setReconnectInterval(2000);
  client.enableHeartbeat(3000, 2000, 2);

  etatMenu = SALON_ATTENTE;
  dirty = true;
}

// ======================= SETUP & LOOP =======================
void setup() {
  for (uint8_t r = 0; r < 3; r++) pinMode(ROW_PINS[r], ROW_MODE);
  for (uint8_t c = 0; c < 3; c++) pinMode(COL_PINS[c], INPUT);

  u8g2.begin();
  u8g2.setBusClock(I2C_CLOCK_HZ);
  randomSeed(analogRead(A0));

  prefs.begin("snake", false);
  hiScore = prefs.getUInt("hi", 0);

  dirty = true;
}

void loop() {
  pressedEdge = 0;
  updateKeys();

  if (roleActuel == ROLE_SERVEUR) {
    serveur.loop();
  } else if (roleActuel == ROLE_CLIENT && etatMenu != HOTE_PARTI) {
    client.loop();
    if (etatMenu >= SALON_ATTENTE && etatMenu <= GAME_OVER && WiFi.status() != WL_CONNECTED) {
      hoteParti();
    }
  }

  switch (etatMenu) {
    case MENU_PRINCIPAL:
      if (wasPressed(K_UP) || wasPressed(K_DOWN)) {
        indexMenuPrincipal = 1 - indexMenuPrincipal;
        dirty = true;
      } else if (wasPressed(K_A)) {
        if (indexMenuPrincipal == 0) { etatMenu = CONFIRM_CREATION; dirty = true; }
        else scannerEtListerParties();
      }
      break;

    case CONFIRM_CREATION:
      if (wasPressed(K_A)) demarrerServeur();
      else if (wasPressed(K_B)) { etatMenu = MENU_PRINCIPAL; dirty = true; }
      break;

    case LISTE_PARTIES:
      if (nombrePartiesTrouvees > 0) {
        if (wasPressed(K_DOWN)) {
          indexPartie = (indexPartie + 1) % nombrePartiesTrouvees;
          dirty = true;
        } else if (wasPressed(K_UP)) {
          indexPartie = (indexPartie - 1 + nombrePartiesTrouvees) % nombrePartiesTrouvees;
          dirty = true;
        } else if (wasPressed(K_A)) {
          etatMenu = CONFIRM_REJOINDRE;
          dirty = true;
        }
      }
      if (wasPressed(K_B)) { etatMenu = MENU_PRINCIPAL; dirty = true; }
      break;

    case CONFIRM_REJOINDRE:
      if (wasPressed(K_A)) demarrerClient();
      else if (wasPressed(K_B)) { etatMenu = LISTE_PARTIES; dirty = true; }
      break;

    case SALON_ATTENTE:
      if (roleActuel == ROLE_SERVEUR) {
        if (wasPressed(K_START)) {
          broadcastTXT("START_GAME");
          lancerPlacementInitial();
        } else if (wasPressed(K_B)) {
          fermerServeur();
        }
      } else if (wasPressed(K_B)) {
        quitterEnClient();
      }
      break;

    case PLACEMENT_INITIAL: {
      if (millis() - placementStart >= 3000) {
        lancerDecompte();
      }
      break;
    }

    case DECOMPTE: {
      int step = (int)((millis() - decompteStart) / 1000);
      if (step != decompteAffiche) { decompteAffiche = step; dirty = true; }
      if (step >= 4) {
        etatMenu = EN_JEU;
        dirty = true;
      }
      break;
    }

    case EN_JEU: {
      if (wasPressed(K_START) && roleActuel == ROLE_SERVEUR) {
        etatMenu = PAUSE_JEU;
        broadcastTXT("PAUSE_GAME");
        dirty = true;
        break;
      }

      int newDir = 0;
      if (wasPressed(K_LEFT))  newDir = 1;
      if (wasPressed(K_RIGHT)) newDir = 2;
      if (wasPressed(K_UP))    newDir = 3;
      if (wasPressed(K_DOWN))  newDir = 4;

      if (wasPressed(K_SELECT)) {
        if (roleActuel == ROLE_SERVEUR) fermerServeur();
        else quitterEnClient();
      }

      if (newDir != 0) {
        if (roleActuel == ROLE_SERVEUR) setDirection(0, newDir);
        else sendClientDirection(newDir);
      }
      if (roleActuel == ROLE_SERVEUR) updateServerSnakePhysics();
      break;
    }

    case PAUSE_JEU: {
      if (wasPressed(K_START) && roleActuel == ROLE_SERVEUR) {
        etatMenu = EN_JEU;
        broadcastTXT("RESUME_GAME");
        dirty = true;
      } else if (wasPressed(K_SELECT)) {
        if (roleActuel == ROLE_SERVEUR) fermerServeur();
        else quitterEnClient();
      }
      break;
    }

    case GAME_OVER:
      if (roleActuel == ROLE_SERVEUR) {
        if (wasPressed(K_UP) || wasPressed(K_DOWN)) {
          gameOverChoice = 1 - gameOverChoice;
          dirty = true;
        } else if (wasPressed(K_A)) {
          if (gameOverChoice == 0) {
            broadcastTXT("RESET_GAME");
            lancerPlacementInitial();
          } else {
            fermerServeur();
          }
        }
      } else if (wasPressed(K_B)) {
        quitterEnClient();
      }
      break;

    case HOTE_PARTI:
      if (wasPressed(K_A) || wasPressed(K_B)) ESP.restart();
      break;

    default: break;
  }

  if (dirty) {
    dirty = false;
    afficherMenuOLED();
  }
}