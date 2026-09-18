# Écran e-paper connecté pour Feather ESP32-C6

Un écran e-paper Waveshare 4,2 pouces (400x300, noir et blanc) piloté par une carte Adafruit ESP32-C6 Feather sous Zephyr. La carte rejoint le Wi-Fi de la maison, affiche son adresse, puis affiche les images qu'un outil du PC lui envoie.

## Organisation du dépôt

| Dossier | Contenu |
|---|---|
| `fw/` | Firmware Zephyr, avec la définition du protocole dans `fw/proto/`. |
| `tools/` | Outils du PC. `tools/convert/` transforme une image quelconque en image binaire, et `tools/upload/` envoie une image binaire à la carte. |
| `docs/` | Documentation. |

Le fichier `west.yml` reste à la racine, car ce dépôt est le dépôt manifeste du workspace west et doit être cloné dans un dossier nommé `firmware/`.

## Documentation

- [docs/firmware.md](docs/firmware.md) couvre le matériel, le câblage, l'installation du workspace, la configuration du Wi-Fi, le build et le dépannage.
- [docs/upload.md](docs/upload.md) couvre le format des images, le protocole d'envoi et les outils `tools/upload` et `tools/convert`.

## En bref

Depuis le dossier du workspace, avec le venv activé :

```sh
cp firmware/fw/wifi.conf.example firmware/fw/wifi.conf   # Puis renseigner le réseau
west build firmware/fw
west flash
python firmware/tools/upload/upload.py <adresse affichée> firmware/tools/upload/assets/test-pattern.bin
```
