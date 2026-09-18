# Firmware e-paper pour Feather ESP32-C6

Firmware Zephyr qui pilote un écran e-paper Waveshare 4,2 pouces (400x300, noir et blanc) depuis une carte Adafruit ESP32-C6 Feather. Au démarrage, il rejoint le réseau Wi-Fi, affiche un écran d'accueil qui donne son adresse IP et l'état de sa batterie, puis affiche chaque image qu'un client lui envoie. Le protocole d'envoi et l'outil associé sont décrits dans [upload.md](upload.md). Les sources se trouvent dans le dossier `fw/` du dépôt.

## Matériel

- Carte [Adafruit ESP32-C6 Feather](https://www.adafruit.com/product/5933), avec 4 Mo de flash et la console sur le port USB natif.
- Module [Waveshare 4.2inch e-Paper](https://www.waveshare.com/4.2inch-e-paper-module.htm), PCB marqué Rev2.1.

| Module e-paper | GPIO | Pastille du Feather | Rôle |
|---|---|---|---|
| VCC | | 3V | Alimentation |
| GND | | GND | Masse |
| DIN | IO22 | MOSI | SPI MOSI |
| CLK | IO21 | SCK | SPI SCK |
| CS | IO2 | A5 | Sélection SPI |
| DC | IO3 | A4 | Donnée ou commande |
| RST | IO5 | IO5 ou A3 | Reset |
| BUSY | IO6 | IO6 ou A2 | Occupation du contrôleur |

Sur ce Feather, l'IO5 et l'IO6 sortent chacun sur deux pastilles, comme l'indique la [page de brochage d'Adafruit](https://learn.adafruit.com/adafruit-esp32-c6-feather/pinouts). Rien d'autre ne doit donc être branché sur A2 ni sur A3. Cette page liste par erreur un « IO12 » parmi les broches numériques. La pastille concernée, en position D12 du format Feather, porte en réalité l'IO14, car l'IO12 et l'IO13 sont les lignes USB de l'ESP32-C6.

Alimenter le module en 3,3 V et pas en 5 V, car ses sorties suivent le niveau de VCC et les GPIO de l'ESP32-C6 ne tolèrent pas le 5 V. Le câblage est décrit côté logiciel dans `fw/boards/adafruit_feather_esp32c6_esp32c6_hpcore.overlay`.

## Révision de la dalle

Le marquage Rev2.1 du PCB désigne la carte driver et pas la dalle. Deux dalles existent, avec deux contrôleurs différents, et le choix se fait à la compilation avec l'option `PANEL`.

| `PANEL` | Contrôleur | BUSY au repos | État |
|---|---|---|---|
| `v1` (défaut) | UC8176 | Niveau haut | Validé sur le matériel |
| `v2` | SSD1683 | Niveau bas | Compile, jamais testé sur matériel |

Waveshare indique qu'une dalle V2 porte une étiquette "V2" au dos. En cas de doute, le niveau de repos de BUSY tranche. Avec la mauvaise valeur de `PANEL`, l'initialisation de la dalle n'aboutit pas et la console affiche l'erreur `Panel init not finished after 20 s`.

## Installation du workspace

Le dépôt est le dépôt manifeste d'un workspace west. Il doit se trouver dans un dossier nommé `firmware/`, lui-même placé dans un dossier de workspace qui recevra Zephyr et ses modules (environ 1,5 Go).

Prérequis sur la machine :

- Zephyr SDK 1.0.1 avec la toolchain `riscv64-zephyr-elf`.
- Python 3 avec `venv` (testé avec Python 3.12), CMake, Ninja et `dtc`.

Depuis le dossier du workspace, qui contient `firmware/` :

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -l firmware
west update --narrow -o=--depth=1
west packages pip --install
pip install -r firmware/tools/upload/requirements.txt
west blobs fetch hal_espressif
```

Le fichier `requirements.txt` de l'outil d'envoi sert aussi au build, car le générateur nanopb a besoin des paquets `protobuf` et `grpcio-tools`. La commande `west blobs fetch` télécharge les bibliothèques radio binaires d'Espressif, sans lesquelles le driver Wi-Fi ne se compile pas.

Zephyr est épinglé sur un SHA de `main` dans `west.yml`, car aucune release ne contient encore à la fois la carte `adafruit_feather_esp32c6` et le support `solomon,ssd1683`.

## Configurer le Wi-Fi

Les identifiants du réseau ne sont pas versionnés. Ils vivent dans `fw/wifi.conf`, que git ignore et que `fw/CMakeLists.txt` fusionne dans la configuration quand il existe.

```sh
cp firmware/fw/wifi.conf.example firmware/fw/wifi.conf
```

Renseigner ensuite `CONFIG_APP_WIFI_SSID` et `CONFIG_APP_WIFI_PSK` dans ce fichier. Le réseau doit être disponible en 2,4 GHz, seule bande de l'ESP32-C6, avec une sécurité WPA2 ou WPA3 personnelle. Sans ce fichier, le build affiche un avertissement et le firmware ne rejoint aucun réseau.

Le firmware attend une adresse IP pendant 30 s au plus avant d'afficher l'écran d'accueil. Si le réseau n'est pas joignable à temps, l'écran indique `Wi-Fi hors ligne`, puis le driver continue ses tentatives en arrière-plan. Quand la carte obtient ensuite une adresse que l'écran d'accueil n'affiche pas, que le réseau arrive après coup ou que le DHCP attribue une autre adresse après une coupure, le firmware redessine l'écran d'accueil avec cette adresse et une mesure fraîche de la batterie. Ce rafraîchissement attend au moins 180 s après le précédent, délai que le fabricant de la dalle demande entre deux rafraîchissements complets. Une simple perte du Wi-Fi ne redessine rien, et l'écran d'accueil ne revient plus une fois qu'une image envoyée l'a remplacé.

## Compiler, flasher, lire la console

Depuis le dossier du workspace, avec le venv activé :

```sh
west build firmware/fw                          # Dalle v1 par défaut
west build -p always firmware/fw -- -DPANEL=v2  # Dalle v2
west flash
west espressif monitor                          # Quitter avec Ctrl+]
```

Si plusieurs ports série sont présents, préciser le port avec `west flash --esp-device /dev/ttyACM0` et `west espressif monitor -p /dev/ttyACM0`. La console passe par l'USB natif de l'ESP32-C6, donc ce qui est affiché avant l'ouverture du port est perdu. Ouvrir le moniteur puis appuyer sur le bouton reset pour voir le démarrage complet.

## Personnaliser l'écran d'accueil

- Le texte et la mise en page se trouvent dans `compose_boot_screen()` du fichier `fw/src/main.c`. Les chaînes sont en UTF-8 et les polices couvrent l'ASCII, le Latin-1 et quelques caractères français supplémentaires. Un caractère absent s'affiche `?`.
- L'image est le fichier `fw/assets/picture.png`. Elle est réduite pour tenir dans 160x160 pixels puis tramée en noir et blanc à chaque build. Les dimensions maximales se règlent dans `fw/CMakeLists.txt`.
- Les polices de `fw/src/fonts/` sont générées par `fw/scripts/font2c.py` à partir d'un fichier TrueType, puis versionnées. La commande d'exemple figure en tête du script.
- Si l'image apparaît tête en bas par rapport au montage, ajouter `CONFIG_APP_DISPLAY_FLIP_180=y` dans `fw/prj.conf`. Ce réglage s'applique aussi aux images reçues par Wi-Fi.

Un rafraîchissement complet dure plusieurs secondes et fait clignoter la dalle. Il faut donc composer tout l'écran avant d'appeler `canvas_flush()`.

## Dépannage

- Quand la dalle ne termine pas un appel en 20 s, le firmware ne se fige pas. Le serveur continue de répondre, il renvoie une erreur aux clients tant que l'appel est en cours, et l'affichage reprend seul ensuite, ce qu'annonce le message `The stalled panel call ended`. L'erreur affichée sur la console existe en trois variantes, qui désignent chacune une cause différente.
- La variante `... because the BUSY line floats` signale que rien ne pilote la ligne BUSY, qui suit alors les résistances de tirage internes du microcontrôleur. La liaison est coupée entre le module et la carte. Vérifier d'abord le fil BUSY et ses contacts, puis le câble et le connecteur du module. Sur le Feather, l'IO6 sort sur deux pastilles, `6` et `A2`, ce qui permet de tester un contact en déplaçant le fil sans modifier le firmware.
- La variante `... and the panel really drives BUSY` signale une dalle qui ne termine réellement pas. C'est l'état constaté sur le montage de référence, par longs épisodes où la commande PON, qui démarre le booster haute tension de la dalle, n'aboutit plus. Aucun réglage logiciel n'y change rien, et la cause matérielle n'est pas encore identifiée. Les causes possibles sont une valeur de `PANEL` qui ne correspond pas à la dalle, un fil RST mal câblé, la nappe de la dalle mal verrouillée dans son connecteur, ou une alimentation VCC ou GND de mauvaise qualité.
- La variante `... although BUSY is released` signale un appel bloqué côté microcontrôleur, dans le transfert SPI par exemple, et pas dans la dalle.
- L'avertissement `Access point refused the key` signale que le point d'accès a été trouvé mais que la négociation WPA a échoué. La cause habituelle est une clé erronée dans `fw/wifi.conf`.
- L'avertissement `Access point not found` signale qu'aucun point d'accès compatible ne porte ce nom en 2,4 GHz. Vérifier le nom du réseau, accents et majuscules compris, ainsi que la portée.
- Le message `SHA-256 comparison failed ... Attempting to boot anyway` de la ROM au démarrage est normal, car l'image Zephyr ne contient pas d'empreinte.
