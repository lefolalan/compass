# Envoi d'images par Wi-Fi

Un client envoie au firmware le contenu complet de l'écran, et la carte l'affiche. Le protocole est défini dans `fw/proto/epaper.proto`, seule source de vérité pour les deux côtés. Le firmware le décode avec nanopb, et les outils du PC avec le paquet `protobuf`.

## Format d'une image

Une image est le contenu brut de tout l'écran, sans en-tête.

- La taille est de 400 par 300 pixels, à 1 bit par pixel, soit exactement 15000 octets.
- Les pixels sont rangés ligne par ligne, de gauche à droite puis de haut en bas, avec le bit de poids fort en premier dans chaque octet.
- Un bit à 1 est un pixel noir.

C'est la trame d'un fichier PBM binaire privée de son en-tête, et aussi la convention de `struct canvas_bitmap` dans le firmware. La rotation `CONFIG_APP_DISPLAY_FLIP_180` s'applique côté carte, donc une image est toujours produite à l'endroit.

## Protocole

Le firmware écoute en TCP sur le port 7400, réglable avec `CONFIG_APP_UPLOAD_PORT`. Une connexion porte un seul échange, c'est-à-dire un message `Request` du client puis un message `Response` de la carte. Chaque message voyage au format délimité de protobuf, soit sa longueur en varint suivie du message encodé.

`Request` contient une commande dans un `oneof`, ce qui permet d'en ajouter sans casser le format. La seule commande actuelle est `ShowFrame`, avec les champs `width`, `height` et `pixels`. La carte remplace tout l'écran, lance un rafraîchissement complet, puis répond. La réponse arrive donc environ 5 s après l'envoi. Waveshare recommande de laisser au moins 180 s entre deux rafraîchissements complets pour ménager la dalle, et le firmware n'impose pas ce délai.

| `Response.status` | Signification |
|---|---|
| `STATUS_OK` | L'image est affichée. |
| `STATUS_INVALID_REQUEST` | Le message est illisible, ou l'image ne correspond pas à la dalle. Le champ `detail` donne la raison. |
| `STATUS_DISPLAY_ERROR` | L'image n'est pas affichée. Le champ `detail` précise si la dalle n'a pas terminé son rafraîchissement à temps, si elle est encore occupée par un appel précédent, ou si elle a refusé l'image. |

La carte applique les règles suivantes.

- Elle sert un seul client à la fois, et les suivants attendent leur tour.
- Elle abandonne un client qui reste muet plus de 10 s.
- Elle attend la dalle 20 s au plus. Passé ce délai elle répond `STATUS_DISPLAY_ERROR`, puis elle refuse aussitôt les images suivantes tant que la dalle n'a pas terminé, au lieu de faire attendre le client. Le dépannage de ce cas est décrit dans [firmware.md](firmware.md).
- Elle refuse avant toute lecture un message annoncé plus long que la plus grande requête valide.

Le protocole n'a ni authentification ni chiffrement. Toute machine du réseau local peut changer l'affichage, donc la carte doit rester sur un réseau de confiance.

## Outil `tools/upload`

`tools/upload/upload.py` envoie un fichier image à la carte et attend sa réponse. Il compile `fw/proto/epaper.proto` à la volée, donc aucun code généré n'est versionné. Il se contente d'envoyer, et la conversion d'une image quelconque vers ce format fera l'objet d'un autre outil.

```sh
pip install -r firmware/tools/upload/requirements.txt
python firmware/tools/upload/upload.py 192.168.1.42 firmware/tools/upload/assets/test-pattern.bin
```

L'adresse de la carte s'affiche sur son écran d'accueil. L'option `--port` change le port. L'outil sort avec le code 0 quand la carte confirme l'affichage, et avec le code 1 accompagné d'un message sur la sortie d'erreur dans tous les autres cas. Il refuse localement un fichier qui ne fait pas 15000 octets.

Le dossier `tools/upload/assets/` contient des images prêtes à l'envoi.

| Fichier | Contenu |
|---|---|
| `test-pattern.bin` | Mire avec cadre, repères des quatre coins, damier et pavés noir et blanc. Elle révèle une erreur d'orientation, d'ordre des bits ou de polarité. |
| `soleil.bin` | Image tramée en plein écran. |
| `bonjour.bin` | Texte seul, avec un bandeau noir. |
