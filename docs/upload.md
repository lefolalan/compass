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

`tools/upload/upload.py` envoie un fichier image à la carte et attend sa réponse. Il compile `fw/proto/epaper.proto` à la volée, donc aucun code généré n'est versionné. Il se contente d'envoyer, et la conversion d'une image quelconque vers ce format revient à l'outil `tools/convert`, décrit plus bas.

```sh
pip install -r firmware/tools/upload/requirements.txt
python firmware/tools/upload/upload.py 192.168.1.42 firmware/tools/upload/assets/test-pattern.bin
```

L'adresse de la carte s'affiche sur son écran d'accueil. L'option `--port` change le port. L'outil sort avec le code 0 quand la carte confirme l'affichage, et avec le code 1 accompagné d'un message sur la sortie d'erreur dans tous les autres cas. Il refuse localement un fichier qui ne fait pas 15000 octets.

Le dossier `tools/upload/assets/` contient des images prêtes à l'envoi.

| Fichier | Contenu |
|---|---|
| `test-pattern.bin` | Mire avec cadre, repères des quatre coins, damier et pavés noir et blanc. Elle révèle une erreur d'orientation, d'ordre des bits ou de polarité. |
| `soleil.bin` | Soleil tramé, produit par `tools/convert` à partir de `fw/assets/picture.png`. |
| `bonjour.bin` | Texte seul, avec un bandeau noir. |

## Outil `tools/convert`

`tools/convert/convert.py` transforme une image quelconque en fichier image prêt à l'envoi. Il accepte tout format que Pillow sait lire. Sa seule dépendance est Pillow, que le venv du workspace contient déjà. Partir d'une photo demande donc une conversion puis un envoi.

```sh
pip install -r firmware/tools/convert/requirements.txt
python firmware/tools/convert/convert.py photo.jpg photo.bin --preview photo-apercu.png
python firmware/tools/upload/upload.py 192.168.1.42 photo.bin
```

L'outil enchaîne les étapes suivantes.

- Il redresse l'image selon l'orientation EXIF qu'un appareil photo y a notée, puis il compose les zones transparentes sur un fond blanc.
- Il adapte l'image aux 400 par 300 pixels de la dalle en conservant ses proportions. Avec `--fit contain`, le réglage par défaut, toute l'image tient à l'écran entre des bandes blanches. Avec `--fit cover`, l'image remplit l'écran et ce qui dépasse est rogné autour du centre. Une image plus petite que la dalle est agrandie dans les deux cas.
- Il convertit l'image en noir et blanc. Le tramage Floyd-Steinberg, appliqué par défaut, rend les nuances d'une photo. Avec `--dither none`, un seuillage simple à mi-gris le remplace, ce qui convient à une image déjà en noir et blanc comme un texte ou un logo.
- Il écrit le fichier de 15000 octets. Avec `--preview`, il écrit aussi un aperçu PNG, reconstruit à partir des octets relus dans ce fichier et non à partir de l'image intermédiaire. L'aperçu montre donc ce que la dalle affichera.

L'outil sort avec le code 0 quand le fichier est écrit, et avec le code 1 accompagné d'un message sur la sortie d'erreur dans tous les autres cas, par exemple quand la source est illisible ou n'est pas une image. Il refuse d'écrire par-dessus le fichier source.

Deux définitions sont partagées pour ne pas exister en double. Les dimensions de la dalle viennent de `tools/frame_format.py`, que `upload.py` utilise aussi. La conversion elle-même (orientation, fond blanc, tramage et polarité des bits) est celle de `fw/scripts/img2c.py`, que le build du firmware applique à l'image de l'écran d'accueil.

Le fichier `soleil.bin` se régénère à partir de cette même image avec la commande suivante.

```sh
python firmware/tools/convert/convert.py firmware/fw/assets/picture.png firmware/tools/upload/assets/soleil.bin
```

Les tests de l'outil se lancent avec pytest, présent dans le venv du workspace. Ils lancent l'outil comme le ferait un utilisateur, puis ils relisent le fichier produit bit par bit sans passer par le code de l'outil, ce qui vérifie la taille, la polarité et l'ordre des bits.

```sh
python -m pytest firmware/tools
```
