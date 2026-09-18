# Firmware e-paper pour Feather ESP32-C6

Firmware Zephyr qui pilote un écran e-paper Waveshare 4,2 pouces (400x300, noir et blanc) depuis une carte Adafruit ESP32-C6 Feather. Le périmètre actuel est un MVP. Au démarrage, il compose un écran avec du texte accentué et une image tramée, puis rafraîchit la dalle une fois. Le cycle complet prend environ 5 s.

## Matériel

- Carte [Adafruit ESP32-C6 Feather](https://www.adafruit.com/product/5933), avec 4 Mo de flash et la console sur le port USB natif.
- Module [Waveshare 4.2inch e-Paper](https://www.waveshare.com/4.2inch-e-paper-module.htm), PCB marqué Rev2.1.

| Module e-paper | Feather | Rôle |
|---|---|---|
| VCC | 3V3 | Alimentation |
| GND | GND | Masse |
| DIN | IO22 | SPI MOSI |
| CLK | IO21 | SPI SCK |
| CS | IO2 | Sélection SPI |
| DC | IO3 | Donnée ou commande |
| RST | IO5 | Reset |
| BUSY | IO6 | Occupation du contrôleur |

Alimenter le module en 3,3 V et pas en 5 V, car ses sorties suivent le niveau de VCC et les GPIO de l'ESP32-C6 ne tolèrent pas le 5 V. Le câblage est décrit côté logiciel dans `fw/boards/adafruit_feather_esp32c6_esp32c6_hpcore.overlay`.

## Révision de la dalle

Le marquage Rev2.1 du PCB désigne la carte driver et pas la dalle. Deux dalles existent, avec deux contrôleurs différents, et le choix se fait à la compilation avec l'option `PANEL`.

| `PANEL` | Contrôleur | BUSY au repos | État |
|---|---|---|---|
| `v1` (défaut) | UC8176 | Niveau haut | Validé sur le matériel |
| `v2` | SSD1683 | Niveau bas | Compile, jamais testé sur matériel |

Waveshare indique qu'une dalle V2 porte une étiquette "V2" au dos. En cas de doute, le niveau de repos de BUSY tranche. Avec la mauvaise valeur de `PANEL`, le firmware reste bloqué et affiche l'avertissement `Panel still busy` sur la console.

## Installation du workspace

Le projet est le dépôt manifeste d'un workspace west. Il doit se trouver dans un dossier nommé `firmware/`, lui-même placé dans un dossier de workspace qui recevra Zephyr et ses modules (environ 1 Go).

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
```

Zephyr est épinglé sur un SHA de `main` dans `west.yml`, car aucune release ne contient encore à la fois la carte `adafruit_feather_esp32c6` et le support `solomon,ssd1683`.

## Compiler, flasher, lire la console

Depuis le dossier du workspace, avec le venv activé :

```sh
west build firmware/fw                       # Dalle v1 par défaut
west build -p always firmware/fw -- -DPANEL=v2  # Dalle v2
west flash
west espressif monitor                       # Quitter avec Ctrl+]
```

Si plusieurs ports série sont présents, préciser le port avec `west flash --esp-device /dev/ttyACM0` et `west espressif monitor -p /dev/ttyACM0`. La console passe par l'USB natif de l'ESP32-C6, donc ce qui est affiché avant l'ouverture du port est perdu. Ouvrir le moniteur puis appuyer sur le bouton reset pour voir le démarrage complet.

## Personnaliser l'écran

- Le texte et la mise en page se trouvent dans `compose_screen()` du fichier `fw/src/main.c`. Les chaînes sont en UTF-8 et les polices couvrent l'ASCII, le Latin-1 et quelques caractères français supplémentaires. Un caractère absent s'affiche `?`.
- L'image est le fichier `fw/assets/picture.png`. Elle est réduite pour tenir dans 160x160 pixels puis tramée en noir et blanc à chaque build. Les dimensions maximales se règlent dans `fw/CMakeLists.txt`.
- Les polices de `fw/src/fonts/` sont générées par `fw/scripts/font2c.py` à partir d'un fichier TrueType, puis versionnées. La commande d'exemple figure en tête du script.
- Si l'image apparaît tête en bas par rapport au montage, ajouter `CONFIG_APP_DISPLAY_FLIP_180=y` dans `fw/prj.conf`.

Un rafraîchissement complet dure plusieurs secondes et fait clignoter la dalle. Il faut donc composer tout l'écran avant d'appeler `canvas_flush()`.

## Dépannage

- L'avertissement `Panel still busy` apparaît après 20 s quand le contrôleur ne libère jamais BUSY. Les causes possibles sont une valeur de `PANEL` qui ne correspond pas à la dalle, un fil BUSY ou RST mal câblé, la nappe de la dalle mal verrouillée dans son connecteur, ou une alimentation VCC ou GND de mauvaise qualité.
- Le message `SHA-256 comparison failed ... Attempting to boot anyway` de la ROM au démarrage est normal, car l'image Zephyr ne contient pas d'empreinte.
