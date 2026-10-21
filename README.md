## CircuitsCourses_TronGame
# 🕹️ Multiplayer Snake sur XIAO ESP32-S3 (Console IoT & Rétrogaming)

Ce projet transforme un microcontrôleur **Seeed Studio XIAO ESP32-S3** en une console de jeu miniature et connectée. Grâce à sa puce Wi-Fi intégrée et l'implémentation de **WebSockets**, il gère une architecture **Serveur / Client** permettant à **4 joueurs maximum** de s'affronter en réseau local sans routeur externe (le serveur héberge son propre point d'accès Wi-Fi).

---

## 🛠️ Schématique et Architecture Matérielle (PCB IFT-S3)

La conception matérielle de la carte **IFT-S3** s'articule autour des composants suivants :

![](/image/capturePCB1.png) 

![](/image/capturePCB2.png)

* **Microcontrôleur :** Seeed Studio XIAO ESP32-S3 (connecteur U1).
* **Affichage OLED :** Écran 128x64 pixels (contrôleur SSD1306, interface I2C à 400 kHz) branché sur le connecteur `J1` (`SDA`, `SCL`, `3.3V`, `GND`).
* **Matrice de Clavier $3\times3$ :** Grille de 8 boutons (`Up`, `Right`, `A`, `Left`, `Select`, `B`, `Down`, `Start`) sécurisée par des diodes anti-retour `1N4148WS` pour éviter les courants de rebouclage lors d'appuis multiples.
* **Alimentation & Puissance :** Emplacement pour batterie Li-Po (`J7`), interrupteur de mise sous tension (`S3`), et étages MOSFET optionnels (`IRLZ44N`) pilotés par `Tr_GATE` / `Tr_GATE2`.
* **Périphériques additionnels :** Ligne de LED Neopixel RGB optionnelle connectée sur la broche `RGB_LEDs` (`GPIO4/D3/A3`) via une résistance de protection de 330 ohms (`R4`).

### Pinout de la Matrice de Clavier
* **Colonnes (`COL_PINS`) :** `D7` (Col 0), `D1` (Col 1), `D6` (Col 2).
* **Lignes (`ROW_PINS`) :** `D8` (Ligne 0), `D10` (Ligne 1), `D9` (Ligne 2).

---

## 📡 Fonctionnement Logiciel & Réseau

### 1. Le Rôle Serveur (J1 - Hôte)
* Lorsqu'un joueur choisit **"1. Créer Partie"**, l'ESP32 génère un point d'accès Wi-Fi sécurisé unique (SSID du type `ESP32_GAME_XXXX` avec le mot de passe `jeu12345`).
* Il agit en tant que **Serveur WebSocket** (port `81`).
* Il gère la physique globale du jeu (`updateServerSnakePhysics`), détecte les collisions, fait progresser les serpents, place la nourriture et diffuse l'état complet (`STATE:...`) à tous les clients connectés.

### 2. Le Rôle Client (J2, J3, J4)
* En choisissant **"2. Rejoindre"**, l'appareil scanne les réseaux Wi-Fi, liste les salons `ESP32_GAME_...` et s'y connecte.
* Le serveur attribue dynamiquement un identifiant (`J2`, `J3` ou `J4`).
* Le client transmet ses directions (`DIR:id,direction`) et affiche le rendu graphique des données reçues.

---

## 🚀 Guide d'Utilisation

### Étape 1 : Téléversement du Code
1. Ouvrez l'IDE Arduino et installez les dépendances requises :
   * `U8g2` (par oliver)
   * `WebSockets` (par Markus Sattler)
   * Le package de cartes **ESP32** (par Espressif Systems) pour le *XIAO ESP32-S3*.
2. Connectez la carte en USB-C et téléversez le code source.

### Étape 2 : Lancement d'une Partie (Hôte / J1)
1. Au démarrage, naviguez dans le **Menu Principal** à l'aide des touches **Haut / Bas** et validez avec **A**.

![](/image/MenuDepart.jpg) 

2. Sélectionnez **1. Creer Partie** et confirmez.

4. Le salon d'attente (SERVEUR) s'affiche avec le nom du réseau Wi-Fi et le compteur de joueurs ($1/4$).

![](/image/AffichageMenuHote.jpg) 

4. Pressez **START** pour lancer la partie une fois les participants rejoints.

![](/image/AffichageEnJeu.png) 

### Étape 3 : Rejoindre une Partie (Clients / J2 à J4)
1. Sélectionnez **2. Rejoindre** depuis le menu principal.
2. Choisissez la partie détectée dans la liste et validez.
3. Patientez dans le salon d'attente que l'hôte donne le signal de départ.

### 🎮 Contrôles en Jeu
* **Croix directionnelle (Haut, Bas, Gauche, Droite) :** Piloter le serpent.
* **START :** Mettre le jeu en pause (réservé à l'hôte).

![](/image/MenuPause.jpg)   

* **SELECT :** Quitter la partie en cours / déconnecter le serveur.
