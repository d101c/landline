#!/usr/bin/env python3
"""Horodate ce qu'un client abonné voit du moteur pendant un essai de reconnexion.

Se connecte au socket du démon, s'abonne, et écrit une ligne à chaque changement
de l'état des liens et des services, à chaque notification et à chaque changement
de la liste des appels. Lecture seule : n'envoie que « subscribe ».
Ne lance pas le démon ; s'il ne tourne pas, attend qu'il arrive.

    scripts/chrono-moteur.py                 affiche sur la sortie standard
    scripts/chrono-moteur.py essai-A1.log    écrit aussi dans le fichier

Les numéros des appels et le texte des notifications ne sont jamais écrits.
"""
import datetime
import json
import os
import socket
import sys
import time

SUIVIS = ("classic_connected", "le_connected", "ancs_ready", "map_open", "pbap_open")


def chemin_socket():
    base = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(base, "landline", "landlined.sock")


def maintenant():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def resume_etat(evenement):
    etat = {cle: bool(evenement.get(cle)) for cle in SUIVIS}
    appels = evenement.get("calls")
    etat["calls_available"] = bool(appels.get("available")) if isinstance(appels, dict) else False
    return etat


def lignes(evenement, precedent):
    """Les lignes à écrire pour un événement, et l'état des liens à retenir."""
    commande = evenement.get("command")
    if commande == "bt_connection_changed":
        etat = resume_etat(evenement)
        sorties = [f"{cle}={'oui' if valeur else 'non'}" for cle, valeur in etat.items()
                   if precedent is None or precedent.get(cle) != valeur]
        return sorties, etat
    if commande == "bt_notification":
        genre = "déjà présente" if evenement.get("silent") else "nouvelle"
        return [f"notification {genre} app={evenement.get('app_id', '?')}"], precedent
    if commande == "bt_calls":
        etats = ",".join(str(appel.get("state", "?")) for appel in evenement.get("calls") or [])
        return [f"appels=[{etats}]"], precedent
    if commande == "bt_pair_confirm_request":
        return ["demande de confirmation d'appairage"], precedent
    return [], precedent


def ecouter(sortie):
    precedent = None
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as lien:
        lien.connect(chemin_socket())
        lien.sendall(b'{"command":"subscribe"}\n')
        sortie(f"{maintenant()} abonné")
        tampon = b""
        while True:
            bloc = lien.recv(65536)
            if not bloc:
                sortie(f"{maintenant()} socket fermé par le démon")
                return
            tampon += bloc
            while b"\n" in tampon:
                brut, tampon = tampon.split(b"\n", 1)
                try:
                    evenement = json.loads(brut)
                except ValueError:
                    continue  # la ligne « OK » n'est pas du JSON
                if not isinstance(evenement, dict):
                    continue
                sorties, precedent = lignes(evenement, precedent)
                for texte in sorties:
                    sortie(f"{maintenant()} {texte}")


def main():
    fichier = open(sys.argv[1], "a", encoding="utf-8") if len(sys.argv) > 1 else None

    def sortie(texte):
        print(texte, flush=True)
        if fichier:
            fichier.write(texte + "\n")
            fichier.flush()

    try:
        while True:
            try:
                ecouter(sortie)
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
