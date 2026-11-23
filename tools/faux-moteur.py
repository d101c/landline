#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Faux moteur : parle le protocole de docs/moteur-api.md sur un socket de test.

Sert à essayer l'extension sans démon, sans Bluetooth et sans iPhone. Toutes
les données sont inventées.

    tools/faux-moteur.py --socket /tmp/essai/faux.sock
    tools/faux-moteur.py --socket /tmp/essai/faux.sock --etat hors-portee
    tools/faux-moteur.py --socket /tmp/essai/faux.sock message attendre:9 appel

Les scénarios se donnent en arguments (joués dans l'ordre au démarrage, dès
qu'un client est abonné) ou se tapent sur l'entrée standard, un par ligne :

    relie               iPhone relié, batterie 96 %
    hors-portee         iPhone parti : liens coupés
    liens-battants [N [s]]  les liens montent et retombent N fois (défaut 8), une bascule
                        toutes les s secondes (défaut 1,5), sans qu'aucun service ne s'ouvre
    liens-stables       fin des battements : relié pour de bon
    reconnexion         le lien tombe et remonte ; toute la pile de notifications de l'iPhone
                        est renvoyée, marquée silent
    renvoi-bruyant      la même pile renvoyée sans silent, sous d'autres numéros
    eloigne [motif [verrou]]  l'iPhone part, avec le motif et l'état du verrou de bt_presence
    quotidien-ancien / quotidien-nouveau  moteur d'avant ou d'après bt_audio, bt_presence,
                        bt_battery_low
    appels-sans-son-ici / appels-son-ici  l'ordinateur peut-il porter le son des appels
    son-coince / son-libre  le son d'un appel pris ici ne repart pas vers l'iPhone, et le
                        moteur ne le dit pas (l'extension le constate au bout de 3 s)
    son-refuse          il ne repart pas et le moteur le dit (bt_call_audio_result en échec) ;
                        son-libre pour revenir à la normale
    notification-appli whatsapp|telegram|signal|discord|teams|mail|groupe[-vocal|-photo|-video] [expéditeur]
    message [texte]     un message de Camille arrive
    message-leo [texte] un message de Léo arrive
    message-de QUI [texte]
                        un message de camille, leo, numero (numéro brut, sans
                        nom), code (code court 38200), email (adresse iMessage)
                        ou banque (expéditeur alphanumérique)
    message-numero, message-code, message-email, message-long, message-emoji,
    message-vide        cas tirés de vraies données : expéditeur sans nom, code
                        de vérification, adresse e-mail, texte de plusieurs
                        centaines de caractères avec lien, emoji, pièce jointe
                        (texte vide)
    message-suite       trois messages de Camille coup sur coup
    historique-reel     des conversations plus anciennes de toutes ces sortes
    appel-inconnu       appel entrant d'un numéro sans nom
    appel               appel entrant de Camille
    decroche            l'appel est pris (comme depuis l'iPhone)
    raccroche           l'appel se termine
    batterie-faible     batterie à 12 %
    batterie N          batterie à N %
    nom [texte]         l'iPhone change de nom (sans texte : « iPhone de Camille »)
    permission          prérequis système manquants (bt_status.capability.setup)
    permission-ok       prérequis satisfaits
    a-relier            l'iPhone a oublié l'ordinateur
    sans-iphone         aucun iPhone choisi
    appairage           demande de confirmation du code d'appairage
    appairage-partiel   l'ordinateur garde un appairage incomplet (BR/EDR seul,
                        sans clés LE) fait hors du parcours : bt_pair répond
                        already_paired et le lien LE ne monte jamais
    echecs-authentification
                        appairage que l'iPhone ne reconnaît plus : le lien
                        tombe et retombe (org.bluez.Reason.Authentication)
    sans-le             les prochains appairages restent sans moitié LE
                        (avec-le pour revenir à la normale)
    delai QUOI N        durée en secondes de : balayage (avant que l'iPhone se
                        montre, 1), demande (avant le code d'appairage, 1.5),
                        oubli (bt_unpair, 0.5), liens (montée des liens après
                        l'appairage, 0.8)
    contrat-ancien      se comporter comme le moteur d'avant le contrat
                        d'appairage (docs/contrat-appairage.md) : pas de champs
                        bond_state / needs_repair, bt_pair répond already_paired
                        sur un appairage inutilisable, bt_unpair garde la
                        sélection (contrat-nouveau pour revenir)
    expire-appairage    le code n'a pas été confirmé à temps (échec « timeout »)
    interrupteur X      un réglage activé sur l'iPhone après appairage :
                        notifications, messages ou contacts
    obex-injoignable    le service OBEX du système ne répond pas au moteur : liens
                        montés, messages et contacts en échec « other », et
                        l'iPhone ne propose donc pas leurs interrupteurs
                        (obex-retabli pour revenir)
    bus-ferme           la même panne dite par le moteur récent : motif
                        « bus_closed », puis le moteur sort de lui-même deux
                        secondes plus tard et revient réparé (comme relancé par
                        systemd)
    obex-tenace         la même panne, qu'une relance du moteur ne répare pas
    relance             le moteur est relancé (systemctl restart) : tous les
                        clients sont déconnectés, les liens retombent puis
                        remontent (delai relance N, 1.5 s), et la panne OBEX
                        disparaît sauf après obex-tenace
    refuse-oubli        les prochains bt_unpair échouent (status error) ;
                        accepte-oubli pour revenir
    bluetooth-eteint    le Bluetooth de l'ordinateur est coupé (bluetooth-allume
                        pour le rallumer)
    rafale [N]          N notifications déjà présentes sur l'iPhone (40 par
                        défaut), comme à chaque reconnexion
    notification        une notification d'une autre appli arrive
    pile [N]            N notifications (10 par défaut) se sont accumulées sur
                        l'iPhone pendant son absence : renvoyées, silent, par
                        « reconnexion »
    retire-notification la dernière notification est retirée sur l'iPhone
    appel-manque [nom]  l'iPhone publie un appel manqué (appli Téléphone, catégorie 2)
    repondeur           l'iPhone publie un message vocal (appli Téléphone, catégorie 3)
    refuse-envoi        le prochain envoi de message échouera

Lien classique à la demande (étude 11). Par défaut le faux moteur se comporte
comme le moteur d'avant : aucun des champs, événements ni commandes ci-dessous.

    sobre               moteur récent, mode « lean » : lien LE seul au repos, lien
                        classique ouvert par bail (bt_lease, envoi, arrivée d'un
                        message, appel), gardé un moment puis fermé ; publie
                        link_mode, classic_state, messages, contacts,
                        calls_available, le_state… ; émet bt_send_progress,
                        bt_incoming_call, bt_call_progress
    complet             moteur récent, mode « full » : mêmes champs et événements,
                        les deux liens tenus en permanence
    protocole-ancien    retour au moteur d'avant
    sans-accord / accord
                        l'accord des notifications n'est pas encore mémorisé (état
                        normal d'un premier démarrage) : mode appliqué « full »,
                        motif notifications_off
    garde-le-lien / rend-le-lien
                        l'iPhone tient à garder le lien classique : mode appliqué
                        « full », motif phone_keeps_link, le réglage restant « lean »
    lien-inutilise [fautes [cause]]
                        retour à la maison (18 novembre) : l'iPhone est vu, un lien LE
                        monte et meurt 0,9 s plus tard sans avoir servi ; le_state
                        waiting, le_cause unsteady (défaut) ou holding_off, le_strikes
                        donné (défaut 1), présence away out_of_range
    rappel              fin d'un retrait (holding_off) : le_cause unsteady
    reveil              sortie de veille sans lien LE : waiting / after_resume,
                        présence away computer_suspended
    notifications-en-pause [cause]
                        le lien LE manque depuis plus de 180 s : le_state paused,
                        cause phone_silent (défaut), after_resume, phone_away,
                        unresolved_address, advert_off_air ; en mode complet les
                        messages et les appels marchent toujours
    notifications-reprises
    tolerance [etat]    publie le_conn_params (applied par défaut, ou pending,
                        refused, no_right, waiting, off) et le_conn_values
                        (60 ms, latence 0, 6 s quand appliqués ; 30 ms et 720 ms
                        quand refusés) ; « tolerance aucune » retire les champs
    refuse-ouverture / accepte-ouverture
                        l'ouverture du lien classique échoue (iPhone injoignable)
    son-silence         mode sobre, son voulu ici : le bail « audio » est rendu,
                        comme le moteur après un silence (#43) ; le lien se referme
    mains-libres jamais|normal
                        le mains-libres n'arrive pas après l'ouverture du lien
    composition normale|echec-lien|echec-mains-libres|refus|sans-appel
                        ce que donne le prochain bt_call_dial (sans-appel : accepté
                        par l'iPhone, et aucun appel n'apparaît : no_call)
    son-appel normal|echec|inutilise
                        le son d'un appel voulu ici : il vient (audio_here), l'iPhone
                        n'ouvre pas le lien voix (audio_failed, no_voice_link), ou il
                        l'ouvre et rien ne le joue (voice_link_unused)
    delai QUOI N        aussi : ouverture (du lien classique, 1.5), mains-libres
                        (après l'ouverture, 3), maintien (avant fermeture, 4),
                        composition (avant que l'appel apparaisse, 1), sonnerie
                        (délai au bout duquel answer_here devient unavailable, 15),
                        reessai (durée pendant laquelle bt_retry_profiles tient le lien, 30),
                        envoi (durée de l'envoi une fois le lien ouvert, 0.8)
    appel, appel-inconnu, raccroche, decroche
                        en protocole récent : l'appel est d'abord annoncé par
                        bt_incoming_call (answer_here connecting), bt_calls suit
                        quand le mains-libres arrive ; « raccroche » pendant la
                        sonnerie = l'appelant renonce (appel manqué)
    attendre:N          pause de N secondes (N décimal)
    quitter             arrêt

Chaque commande reçue d'un client est écrite sur la sortie standard, précédée
de « RECU ».
"""

import argparse
import asyncio
import json
import os
import sys
import time

CAMILLE = {"thread": "tel:+41790000001", "name": "Camille Martin", "address": "+41790000001"}
LEO = {"thread": "tel:+41790000002", "name": "Léo", "address": "+41790000002"}
# Ce que de vraies données contiennent : pas de nom, code court, adresse e-mail.
SENDERS = {
    "camille": CAMILLE,
    "leo": LEO,
    "numero": {"thread": "tel:+41765550142", "name": "", "address": "+41765550142"},
    # Jamais dans l'historique : un expéditeur dont aucun fil n'existe encore.
    "inconnu": {"thread": "tel:+41215550177", "name": "", "address": "+41215550177"},
    "code": {"thread": "tel:38200", "name": "38200", "address": "38200"},
    "email": {"thread": "lea.rochat@exemple.ch", "name": "", "address": "lea.rochat@exemple.ch"},
    "banque": {"thread": "tel:BanqueDemo", "name": "BanqueDemo", "address": "BanqueDemo"},
}
LONG_TEXT = (
    "Bonjour, votre colis n° 99.00.123456.78901234 n'a pas pu être remis aujourd'hui car personne "
    "n'était présent. Vous pouvez choisir une nouvelle date de livraison ou un point de retrait "
    "jusqu'à demain 23 h 59 : https://colis.exemple.ch/suivi/99001234567890123?lang=fr&canal=sms "
    "Passé ce délai, il sera déposé à l'office de poste le plus proche, où il restera sept jours. "
    "Merci de ne pas répondre à ce message.")
CODE_TEXT = "482913 est votre code de vérification Exemple. Ne le partagez avec personne."
UNKNOWN_CALLER = "+41225550199"
ADDRESS = "AA:BB:CC:00:00:01"
CALL_PATH = "/org/pipewire/Telephony/ag0/call1"

SETUP_STEPS = [
    {"what": "Notification mirroring needs BlueZ's experimental bearer API (Experimental = true).",
     "command": "sudo sed -i 's/^#Experimental.*/Experimental = true/' /etc/bluetooth/main.conf"},
    {"what": "Messages and Contacts need L2CAP ERTM, which the kernel has disabled.",
     "command": "echo N | sudo tee /sys/module/bluetooth/parameters/disable_ertm"},
]


class FauxMoteur:
    def __init__(self, etat):
        self.clients = {}          # writer -> abonné ?
        self.linked = True
        self.paired = True
        self.chosen = True
        self.battery = 96
        self.setup = []
        self.calls = []
        self.audio = "idle"
        self.notifications = []
        self.next_uid = 40
        self.next_handle = 100
        self.refuse_send = False
        self.pair_operation = None
        self.nearby = False
        self.powered = True
        self.calls_enabled = False     # défaut du vrai moteur
        self.le_bond = True            # l'appairage couvre aussi le LE
        self.half = False              # BR/EDR monté sans le LE
        self.auth_fail = False         # le lien tombe en boucle
        self.no_le = False             # les prochains appairages restent sans LE
        self.flap = 0
        self.legacy = False            # moteur d'avant le contrat d'appairage
        self.delays = {"balayage": 1.0, "demande": 1.5, "oubli": 0.5, "liens": 0.8, "relance": 1.5,
                       "ouverture": 1.5, "mains-libres": 3.0, "maintien": 4.0, "composition": 1.0,
                       "sonnerie": 15.0, "envoi": 0.8, "reessai": 30.0,
                       "son": 2.0, "sans-appel": 2.0}
        # Lien classique à la demande (étude 11). None : moteur d'avant, aucun
        # champ nouveau ; "lean" ou "full" : le réglage link_mode.
        self.protocol = None
        self.forced = ""               # motif qui impose le mode complet : phone_keeps_link, notifications_off
        self.classic = "idle"          # classic_state
        self.leases = set()
        self.classic_timer = None      # ouverture ou maintien en cours
        self.waiters = []              # à rappeler quand l'ouverture aboutit ou échoue
        self.hands_free = False
        self.hands_free_never = False
        self.hands_free_timer = None
        self.refuse_open = False
        self.le_pause = None           # cause de la pause des notifications
        self.le_fields = False         # publier le_state / le_cause / le_detail
        self.le_wait = None            # cause publiée avec waiting, None : reconnecting
        self.le_strikes = 0            # liens LE morts avant service
        self.tolerance = None          # le_conn_params publié, None : champ absent
        self.ring = None               # appel annoncé par bt_incoming_call
        self.dial_op = None            # composition en cours
        self.dial_mode = "normale"
        # Le son d'un appel voulu ici (docs/moteur-quotidien.md, §7).
        self.sound_mode = "normal"     # normal, echec (no_voice_link), inutilise (voice_link_unused)
        self.audio_wanted = None       # here, phone : ce qui a été demandé pour l'appel
        self.audio_failed = False
        self.audio_reason = ""
        self.last_sync = int(time.time()) - 600
        self.obex_down = False         # obexd injoignable : MAP et PBAP en échec « other »
        self.obex_error = "other"      # motif publié : « other », ou « bus_closed » (moteur récent)
        self.obex_stubborn = False     # ... et une relance n'y change rien
        self.refuse_unpair = False
        self.switches = {"notifications": True, "messages": True, "contacts": True}
        self.fresh_pairing = False     # après un oubli, l'iPhone remet ses réglages à zéro
        # Le quotidien (docs/moteur-quotidien.md). « quotidien-ancien » : moteur
        # d'avant, qui ne connaît ni bt_audio, ni bt_presence, ni bt_battery_low.
        self.daily = True
        self.phone_audio = "phone"     # réglage : phone, here
        self.call_audio = "phone"
        self.call_switchable = True
        self.name = "iPhone de Camille"  # le nom que l'iPhone se donne
        self.stuck_here = False        # le son d'un appel ne repart pas vers l'iPhone
        self.refuse_phone = False      # ... et le moteur répond que non
        self.lock_on_away = False
        self.lock_seconds = 30
        self.presence = None
        self.shaky = 0                 # liens qui battent : bascules restantes
        self.shaky_services = False    # ... et aucun service ne s'ouvre pendant ce temps
        now = int(time.time())
        self.messages = [
            self._message(CAMILLE, "Tu as vu le programme de samedi ?", now - 5400, read=True),
            self._message(LEO, "Bien reçu, merci !", now - 2520, read=True),
        ]
        self.history = [
            {"number": "+41790000001", "name": "Camille Martin", "type": "received", "timestamp": now - 7200},
            {"number": "+41790000002", "name": "Léo", "type": "missed", "timestamp": now - 86400},
            {"number": "+41220000003", "name": "", "type": "dialed", "timestamp": now - 172800},
        ]
        self.apply_state(etat)

    # --- état ---------------------------------------------------------------

    def apply_state(self, etat):
        self.half, self.auth_fail = False, False
        if etat in ("a-relier", "sans-iphone"):
            self.nearby = False    # il faudra un balayage pour le revoir
        if etat == "relie":
            self.le_bond = True
            self.linked, self.paired, self.chosen, self.setup = True, True, True, []
        elif etat == "hors-portee":
            self.linked = False
        elif etat == "permission":
            self.linked, self.setup = False, list(SETUP_STEPS)
        elif etat == "a-relier":
            self.linked, self.paired = False, False
        elif etat == "sans-iphone":
            self.linked, self.paired, self.chosen = False, False, False

    def _message(self, who, body, timestamp, read=False, outgoing=False):
        self.next_handle += 1
        return {
            "handle": f"{self.next_handle:016X}", "thread": who["thread"], "address": who["address"],
            "name": who["name"], "body": body, "timestamp": timestamp, "outgoing": outgoing,
            "read": read, "folder": "sent" if outgoing else "inbox",
        }

    def bond(self):
        """Santé de l'appairage (docs/contrat-appairage.md, section 1)."""
        if self.legacy:
            return {}
        if not self.paired:
            return {"bond_state": "none", "bond_detail": "", "needs_repair": False}
        if self.auth_fail:
            return {"bond_state": "broken", "bond_detail": "auth_failures", "needs_repair": True}
        if not self.le_bond:
            return {"bond_state": "partial", "bond_detail": "bredr_only", "needs_repair": True}
        return {"bond_state": "ok", "bond_detail": "", "needs_repair": False}

    # --- lien classique à la demande ----------------------------------------

    def lean(self):
        """Le mode réellement appliqué est le mode sobre."""
        return self.protocol == "lean" and not self.forced

    def le_up(self):
        return self.linked and not self.le_pause

    def classic_up(self):
        if self.lean():
            return self.le_up() and self.classic in ("open", "lingering")
        return self.linked

    def demand_fields(self):
        if self.protocol is None:
            return {}
        sw, classic, le = self.switches, self.classic_up(), self.le_up()
        present = self.linked if not self.lean() else le

        def service(on):
            if self.obex_down:
                return "unavailable"
            if not on:
                return "forbidden" if self.paired else "unknown"
            return "ready" if classic else "on_demand" if present else "unknown"

        if not self.calls_enabled:
            calls = "off"
        elif (self.hands_free if self.lean() else self.linked):
            calls = "ready"
        elif not self.call_switchable:
            calls = "control_only"
        else:
            calls = "on_demand" if present else "off"
        lean = self.lean()
        return {
            "link_mode": self.protocol,
            "link_mode_effective": "lean" if lean else "full",
            "link_mode_reason": self.forced if self.protocol == "lean" and not lean else "",
            "classic_state": self.classic if lean else ("open" if classic else "idle"),
            "classic_leases": sorted({"reading" if l == "retry" else l for l in self.leases}) if lean else [],
            "classic_opened_by": ("phone" if self.forced == "phone_keeps_link" else "engine") if classic else "",
            "messages": service(sw["messages"]), "contacts": service(sw["contacts"]),
            "calls_available": calls,
            "last_sync": self.last_sync, "next_sync": self.last_sync + 3600,
        }

    def tolerance_fields(self, le):
        if self.tolerance is None:
            return {}
        state = self.tolerance if le or self.tolerance in ("off", "no_right") else "waiting"
        values = None
        if le and state == "applied":
            values = {"interval_ms": 60, "latency": 0, "timeout_ms": 6000}
        elif le and state == "refused":
            values = {"interval_ms": 30, "latency": 0, "timeout_ms": 720}
        return {"le_conn_params": state, "le_conn_values": values}

    def le_state_fields(self):
        if self.protocol is None and not self.le_fields:
            return {}
        if not self.paired:
            return {"le_state": "unavailable", "le_cause": "not_paired", "le_detail": ""}
        if self.le_pause:
            detail = {
                "phone_silent": "The iPhone is in range but has not reopened the notification link.",
                "after_resume": "No notification link since this computer woke up.",
                "phone_away": "The iPhone is out of range or its Bluetooth is off.",
                "unresolved_address": "A device answered with an address this computer could not resolve.",
                "advert_off_air": "The LE advertisement is not registered.",
            }.get(self.le_pause, "The notification link is down.")
            return {"le_state": "paused", "le_cause": self.le_pause, "le_detail": detail}
        strikes = {"le_strikes": self.le_strikes} if self.le_wait or self.le_strikes else {}
        if not self.linked:
            cause = self.le_wait or "reconnecting"
            detail = "Notifications are waiting." if cause == "holding_off" else ""
            return {"le_state": "waiting", "le_cause": cause, "le_detail": detail, **strikes}
        return {"le_state": "connected", "le_cause": "", "le_detail": "", **strikes}

    def cancel_classic_timer(self):
        if self.classic_timer is not None:
            self.classic_timer.cancel()
            self.classic_timer = None

    def take(self, lease, voice=False, then=None):
        """Prend un bail ; « then(ok) » est rappelé quand le lien est ouvert ou a échoué."""
        loop = asyncio.get_running_loop()
        if not self.lean():
            if then:
                then(self.linked)
            return
        if not self.le_up():
            # Rien n'est ouvert sans lien LE ; le bail d'un client reste noté.
            if lease in ("reading", "setup"):
                self.leases.add(lease)
            if then:
                then(False)
            return
        self.leases.add(lease)
        if voice:
            self.want_hands_free()
        if self.classic in ("open", "lingering"):
            self.cancel_classic_timer()
            self.classic = "open"
            self.broadcast(self.connection())
            if then:
                then(True)
            return
        if then:
            self.waiters.append(then)
        if self.classic != "opening":
            self.cancel_classic_timer()
            self.classic = "opening"
            self.broadcast(self.connection())
            self.classic_timer = loop.call_later(self.delays["ouverture"], self.opened)

    def opened(self):
        self.classic_timer = None
        waiters, self.waiters = self.waiters, []
        if self.refuse_open or not self.le_up():
            self.classic = "failed"
            self.leases.clear()
            self.broadcast(self.connection())
            for then in waiters:
                then(False)
            self.classic_timer = asyncio.get_running_loop().call_later(2.0, self.closed)
            return
        self.classic = "open"
        self.last_sync = int(time.time())
        self.broadcast(self.connection())
        for then in waiters:
            then(True)
        if not self.leases:
            self.linger()

    def release(self, lease):
        if lease not in self.leases:
            return
        self.leases.discard(lease)
        if not self.leases and self.classic == "open":
            self.linger()
        elif self.lean():
            self.broadcast(self.connection())

    def linger(self):
        self.cancel_classic_timer()
        self.classic = "lingering"
        self.broadcast(self.connection())
        self.classic_timer = asyncio.get_running_loop().call_later(self.delays["maintien"], self.closed)

    def closed(self):
        self.classic_timer = None
        self.classic = "idle"
        self.hands_free = False
        if self.hands_free_timer is not None:
            self.hands_free_timer.cancel()
            self.hands_free_timer = None
        self.broadcast(self.connection())
        if self.daily and self.protocol is not None:
            self.broadcast(self.audio_state())

    def want_hands_free(self):
        """Une vraie connexion a été demandée : le mains-libres suit, un moment après."""
        if self.hands_free or self.hands_free_never or self.hands_free_timer is not None:
            return
        delay = self.delays["mains-libres"] + (self.delays["ouverture"] if self.classic not in ("open", "lingering") else 0)
        self.hands_free_timer = asyncio.get_running_loop().call_later(delay, self.hands_free_up)

    def hands_free_up(self):
        self.hands_free_timer = None
        if self.classic not in ("open", "lingering") or "call" not in self.leases:
            return
        self.hands_free = True
        self.broadcast(self.connection())
        if self.daily:
            self.broadcast(self.audio_state())
        ring = self.ring
        if ring and ring.get("answered"):
            # Décroché sur l'iPhone avant que le mains-libres soit là : la
            # passerelle montre maintenant un appel en cours, son sur l'iPhone.
            self.calls = [self.call_entry(ring["number"], ring["name"], "active")]
            self.audio = "idle"
            self.broadcast(self.connection())
            self.broadcast(self.calls_event(with_names=False))
            self.ring_end(keep_lease=True)
        elif ring:
            self.calls = [self.call_entry(ring["number"], ring["name"], "incoming")]
            ring.update(answer_here="ready", path=CALL_PATH)
            self.broadcast(self.connection())
            self.broadcast(self.calls_event(with_names=False))
            self.broadcast(self.ring_event())
        if self.dial_op and self.dial_op["stage"] == "hands_free":
            self.dial_go(self.dial_op)

    def call_entry(self, number, name, state):
        return {"path": CALL_PATH, "number": number, "withheld": False, "name": name, "incoming_line": "",
                "state": state, "multiparty": False, "ringing": state == "incoming",
                "outgoing": state in ("dialing", "alerting"), "connected": state == "active"}

    # --- appel annoncé par sa notification -------------------------------------

    def ring_event(self, state="ringing"):
        ring = self.ring
        event = {"command": "bt_incoming_call", "uid": ring["uid"], "state": state, "caller": ring["caller"],
                 "can_answer_on_phone": True, "can_decline": True, "answer_here": ring["answer_here"],
                 "since": ring["since"]}
        if ring.get("path"):
            event["path"] = ring["path"]
        return event

    def ring_start(self, number, name):
        self.next_uid += 1
        if not self.calls_enabled:
            # Contrôle d'appel coupé : la notification ordinaire, rien d'autre.
            n = {"uid": self.next_uid, "app_id": "com.apple.mobilephone", "app_name": "Phone",
                 "title": name or number, "subtitle": "", "body": "Incoming Call", "category": 1,
                 "timestamp": int(time.time()), "silent": False, "positive_action": True,
                 "negative_action": True}
            self.notifications.append(n)
            self.broadcast(dict(n, command="bt_notification"))
            return
        ready = not self.lean() or self.hands_free
        self.ring = {"uid": self.next_uid, "caller": name or number, "number": number, "name": name,
                     "answer_here": "ready" if ready else "connecting", "since": int(time.time())}
        if ready:
            self.ring["path"] = CALL_PATH
            self.calls = [self.call_entry(number, name, "incoming")]
        self.broadcast({"command": "bt_notification", "uid": self.ring["uid"], "app_id": "com.apple.mobilephone",
                        "app_name": "Phone", "title": name or number, "subtitle": "", "body": "Incoming Call",
                        "category": 1, "timestamp": self.ring["since"], "silent": False,
                        "positive_action": True, "negative_action": True})
        self.broadcast(self.ring_event())
        if ready:
            self.broadcast(self.connection())
            self.broadcast(self.calls_event(with_names=False))
            return
        uid = self.ring["uid"]
        self.take("call", voice=True, then=lambda ok: self.ring_unavailable(uid) if not ok else None)
        asyncio.get_running_loop().call_later(self.delays["sonnerie"], self.ring_unavailable, uid)

    def ring_unavailable(self, uid):
        ring = self.ring
        if ring and ring["uid"] == uid and ring["answer_here"] == "connecting":
            ring["answer_here"] = "unavailable"
            self.broadcast(self.ring_event())

    def ring_end(self, missed=False, keep_lease=False):
        ring = self.ring
        if not ring:
            return
        self.broadcast(self.ring_event("ended"))
        self.broadcast({"command": "bt_notification_removed", "uid": ring["uid"]})
        self.ring = None
        if missed:
            self.next_uid += 1
            n = {"uid": self.next_uid, "app_id": "com.apple.mobilephone", "app_name": "Phone",
                 "title": ring["caller"], "subtitle": "", "body": "Missed Call", "category": 2,
                 "timestamp": int(time.time()), "silent": False, "positive_action": True,
                 "negative_action": True}
            self.notifications.append(n)
            self.broadcast(dict(n, command="bt_notification"))
            self.history.insert(0, {"number": ring["number"], "name": ring["name"], "type": "missed",
                                    "timestamp": int(time.time())})
            self.broadcast({"command": "bt_call_history", "calls": self.history})
        if not keep_lease:
            self.release("call")

    # --- composition ------------------------------------------------------------

    def dial_result(self, op, success, reason=None, message=None):
        result = {"command": "bt_call_result", "action": "dial", "success": success}
        if op.get("id") is not None:
            result["operation_id"] = op["id"]
        if reason:
            result["reason"] = reason
        if message:
            result["message"] = message
        self.call_result(op["writer"], result)
        if self.dial_op is op:
            self.dial_op = None
        if not success:
            self.release("call")

    def call_result(self, writer, result):
        """bt_call_result : au demandeur et aux abonnés."""
        if self.protocol is None:
            # Moteur d'avant : réponse directe et diffusion, un abonné la reçoit deux fois.
            self.reply(writer, result)
            self.broadcast(result)
        elif self.clients.get(writer):
            # Moteur du mode sobre : un client abonné ne la reçoit qu'une fois.
            self.broadcast(result)
        else:
            self.reply(writer, result)
            self.broadcast(result)

    def dial_progress(self, op, stage):
        op["stage"] = stage
        event = {"command": "bt_call_progress", "number": op["number"], "stage": stage}
        if op.get("id") is not None:
            event["operation_id"] = op["id"]
        self.broadcast(event)

    def dial(self, writer, c):
        loop = asyncio.get_running_loop()
        op = {"writer": writer, "id": c.get("operation_id"), "number": str(c.get("number", "")),
              "audio": c.get("audio"), "stage": ""}
        if not self.calls_enabled:
            self.dial_result(op, False, "calls_off", "Call control is off.")
            return
        if self.protocol is not None and op["audio"] not in ("here", "phone"):
            # Qui compose depuis l'ordinateur est devant : sans mot du client, ici.
            op["audio"] = "here"
        if self.protocol is None:
            # Moteur d'avant : mains-libres tenu en permanence, pas d'étapes.
            if not self.linked:
                self.dial_result(op, False, None, "No hands-free link.")
                return
            self.dial_placed(op)
            return
        if self.dial_op is not None or self.calls:
            self.dial_result(op, False, "busy", "A call is already in progress.")
            return
        self.dial_op = op
        if not self.lean() and not self.linked:
            # Mode complet sans passerelle : échec immédiat, rien n'ouvre de lien.
            self.dial_result(op, False, "no_hands_free", "No hands-free link.")
            return
        if not self.lean() or self.hands_free:
            self.dial_go(op)
            return
        self.dial_progress(op, "connecting")

        def linked(ok):
            if self.dial_op is not op:
                return
            if not ok or self.dial_mode == "echec-lien":
                self.dial_result(op, False, "no_link", "Couldn’t reach the iPhone.")
                return
            self.dial_progress(op, "hands_free")
            if self.dial_mode == "echec-mains-libres" or self.hands_free_never:
                loop.call_later(self.delays["mains-libres"], lambda: self.dial_op is op and self.dial_result(
                    op, False, "no_hands_free", "The hands-free link did not come up."))
            elif self.hands_free:
                self.dial_go(op)

        if self.dial_mode == "echec-lien":
            loop.call_later(self.delays["ouverture"], linked, False)
        else:
            if self.dial_mode == "echec-mains-libres":
                self.leases.add("call")
            self.take("call", voice=self.dial_mode != "echec-mains-libres", then=linked)

    def dial_go(self, op):
        if self.dial_op is not op:
            return
        self.dial_progress(op, "dialing")
        asyncio.get_running_loop().call_later(self.delays["composition"], self.dial_placed, op)

    def dial_placed(self, op):
        if self.protocol is not None and self.dial_op is not op:
            return
        if self.dial_mode == "refus":
            self.dial_result(op, False, "refused", "The iPhone refused to place the call.")
            return
        if self.dial_mode == "sans-appel":
            # L'iPhone a pris le numéro ; aucun appel ne se montre.
            self.dial_result(op, True)
            asyncio.get_running_loop().call_later(self.delays["sans-appel"], self.dial_no_call, op)
            return
        who = next((w for w in SENDERS.values() if w["address"] == op["number"]), None)
        self.calls = [self.call_entry(op["number"], who["name"] if who else "", "dialing")]
        followed = self.daily and self.protocol is not None
        if self.daily and op["audio"] in ("here", "phone"):
            comes = op["audio"] == "here" and not (followed and self.sound_mode != "normal")
            self.audio = "active" if comes else "idle"
            if followed:
                self.audio_wanted, self.audio_failed, self.audio_reason = op["audio"], False, ""
        self.dial_result(op, True)
        self.broadcast(self.connection())
        self.broadcast(self.calls_event(with_names=False))
        if followed and op["audio"] == "here":
            if self.sound_mode == "normal":
                self.broadcast({"command": "bt_call_progress", "stage": "audio_here"})
            else:
                asyncio.get_running_loop().call_later(self.delays["son"], self.sound_failed)
        asyncio.get_running_loop().call_later(1.5, self.dial_alerting)

    def dial_no_call(self, op):
        event = {"command": "bt_call_progress", "number": op["number"], "stage": "no_call"}
        if op.get("id") is not None:
            event["operation_id"] = op["id"]
        self.broadcast(event)
        self.release("call")

    def sound_failed(self):
        """Le son voulu ici n'est pas venu : dit une fois, l'appel continue."""
        if not self.calls or self.audio == "active" or self.audio_wanted != "here":
            return
        self.audio_failed = True
        self.audio_reason = "voice_link_unused" if self.sound_mode == "inutilise" else "no_voice_link"
        self.broadcast({"command": "bt_call_progress", "stage": "audio_failed", "reason": self.audio_reason})
        self.broadcast(self.connection())

    def dial_alerting(self):
        if self.calls and self.calls[0]["state"] == "dialing":
            self.calls[0]["state"] = "alerting"
            self.broadcast(self.calls_event(with_names=False))

    def dial_cancel(self, c):
        op = self.dial_op
        if op is None or op["stage"] == "dialing":
            return
        if c.get("operation_id") is not None and c.get("operation_id") != op["id"]:
            return
        self.dial_result(op, False, "cancelled", "Cancelled.")

    def reset_demand(self):
        self.cancel_classic_timer()
        if self.hands_free_timer is not None:
            self.hands_free_timer.cancel()
            self.hands_free_timer = None
        self.classic, self.leases, self.waiters = "idle", set(), []
        self.hands_free, self.ring, self.dial_op = False, None, None

    def connection(self):
        if not self.chosen:
            return {"command": "bt_connection_changed", "device_present": False, "device_paired": False,
                    "link_reason": "No paired iPhone selected.", **self.bond()}
        # « up » : ce qui passe par le lien classique ; « le » : le lien des
        # notifications. Les deux ne font qu'un pour le moteur d'avant.
        le = self.le_up()
        up = self.classic_up()
        # Le moteur corrigé cesse de solliciter un iPhone qui refuse le lien.
        classic = up or (self.half and self.flap % 2 == 0 and not (self.auth_fail and not self.legacy))
        sw = self.switches
        if up:
            reason = "Connected."
        elif le:
            reason = "Messages and calls connect when needed."
        elif self.auth_fail:
            reason = "LE connect failed (org.bluez.Reason.Authentication)."
        elif self.half:
            reason = "Connected, but this pairing has no LE keys, so notifications are unavailable."
        else:
            reason = "The iPhone is out of range."
        return {
            "command": "bt_connection_changed",
            "device_present": True, "device_paired": self.paired,
            "classic_connected": classic, "le_available": self.paired and self.le_bond, "le_connected": le,
            "map_open": up and sw["messages"] and not self.obex_down and not self.shaky_services,
            "pbap_open": up and sw["contacts"] and not self.obex_down and not self.shaky_services,
            "map_error": self.obex_error if self.obex_down else "none" if sw["messages"] else "forbidden",
            "pbap_error": self.obex_error if self.obex_down else "none" if sw["contacts"] else "forbidden",
            "map_held": False, "pbap_held": False, "ancs_held": False,
            "ancs_ready": le and sw["notifications"] and not self.shaky_services,
            "ancs_reason": "Notifications are on." if le else "Waiting for the iPhone.",
            "link_reason": reason,
            "profile_reason": "The connection is closed" if self.obex_down and up else "",
            "battery": self.battery if le else None,
            **({"battery_source": "le" if le else None,
                "battery_state": None if not le else "critical" if self.battery <= 10
                else "low" if self.battery <= 20 else "ok"} if self.daily else {}),
            **self.bond(), **self.demand_fields(), **self.le_state_fields(), **self.tolerance_fields(le),
            "calls": None if not self.calls_enabled else {
                "available": self.hands_free if self.lean() else up,
                "path": "/org/pipewire/Telephony/ag0", "state": "connected" if up else "",
                "operator": "Swisscom" if up else "", "signal": 4, "battery": 0, "service": up,
                "roaming": False, "inband_ringtone": True, "address": ADDRESS, "calls": len(self.calls),
                "backend": "pipewire", "indicators": False, "audio": self.audio, "reason": self.audio_reason,
                **({"audio_wanted": self.audio_wanted, "audio_failed": self.audio_failed}
                   if self.daily and self.protocol is not None else {}),
                **({"audio_switchable": self.call_switchable,
                    "audio_route": "here" if self.audio == "active" else "phone",
                    "audio_reject": self.call_audio == "phone"} if self.daily else {}),
            },
        }

    def status(self):
        return {
            "command": "bt_status", "available": True, "version": "faux-moteur",
            "device_address": ADDRESS if self.chosen else "", "adapter": "hci0", "enabled": True,
            "ancs_enabled": True, "ancs_content_enabled": True, "calls_enabled": self.calls_enabled,
            "retention": "none", "retention_ready": True, "desktop_popups_enabled": False,
            "pair_dialog_enabled": False, "popup_previews_enabled": True, "clipboard_auto_share": False,
            "hide_unknown_senders": False, "muted_apps": [], "airpods_enabled": False,
            "airpods_pause": False, "airpods_handoff": False, "lock_on_away": self.lock_on_away,
            "lock_away_seconds": self.lock_seconds, "wake_on_return": False,
            "yield_when_inactive": False, "contacts_by_last_name": False,
            "phone_audio": self.phone_audio if self.daily else "auto",
            "phone_audio_route": "here" if self.linked and self.phone_audio == "here" else "phone",
            "phone_audio_rule": False, "call_audio": self.call_audio, "apple_device_id": "",
            **({} if self.protocol is None else {"link_mode": self.protocol}),
            "adapters": [{"id": "hci0", "address": "00:00:00:00:00:10", "powered": True, "name": "portable-essai"}],
            **self.bond(), **({} if self.legacy else {"bond_address": ADDRESS if self.paired else ""}),
            "capability": {
                "mode": "blocked" if self.setup else "full", "bearer_api": not self.setup,
                "adapter_present": True, "powered": self.powered, "le_central": True, "le_peripheral": True,
                "advertising": True, "class_ok": False, "bonded_device_present": self.paired,
                "bond_has_le": self.paired and self.le_bond,
                "bond_carries_ancs": self.paired and self.le_bond, "secure_connections": True,
                "ertm_disabled": bool(self.setup), "adapter_id": "hci0",
                "reasons": [s["what"] for s in self.setup], "setup": self.setup,
            },
        }

    def devices(self):
        devices = []
        # Vu pendant un balayage, ou connu de BlueZ parce qu'appairé.
        if self.nearby or self.paired:
            up = self.linked
            devices.append({
                "path": "/org/bluez/hci0/dev_AA_BB_CC_00_00_01", "adapter": "hci0", "address": ADDRESS,
                "name": self.name, "paired": self.paired, "bonded": self.paired,
                "trusted": self.paired, "connected": up or self.half, "classic_connected": up or self.half,
                "le_bearer": self.le_bond, "le_bonded": self.paired and self.le_bond, "le_connected": up,
                "preferred_bearer": "", "map": self.paired, "pbap": self.paired, "ancs": self.paired,
                "ancs_notifying": up, "services_resolved": up, "iphone": True,
                "apple_nearby": True, "airpods": False,
                **({} if self.legacy else {
                    "bond_state": "none" if not self.paired else "ok" if self.le_bond else "partial",
                    "bond_detail": "" if not self.paired or self.le_bond else "bredr_only",
                    "classic_bonded": self.paired}),
            })
        return {"command": "bt_devices", "devices": devices}

    def threads(self):
        threads = {}
        for m in self.messages:
            t = threads.setdefault(m["thread"], {
                "thread": m["thread"], "name": m["name"], "address": m["address"], "preview": "",
                "timestamp": 0, "unread": 0, "count": 0, "group": False, "repliable": True})
            t["count"] += 1
            if not m["read"] and not m["outgoing"]:
                t["unread"] += 1
            if m["timestamp"] >= t["timestamp"]:
                t["timestamp"], t["preview"] = m["timestamp"], " ".join(m["body"].split())
        ordered = sorted(threads.values(), key=lambda t: -t["timestamp"])
        return {"command": "bt_threads", "threads": ordered}

    def incoming(self, who, body):
        now = int(time.time())
        message = self._message(who, body, now)
        self.messages.append(message)
        # La notification ANCS de Messages précède le message relu.
        self.next_uid += 1
        self.broadcast({"command": "bt_notification", "uid": self.next_uid,
                        "app_id": "com.apple.MobileSMS", "app_name": "Messages",
                        "title": who["name"] or who["address"],
                        "subtitle": "", "body": body, "category": 4, "timestamp": now,
                        "silent": False, "positive_action": True, "negative_action": True})
        if self.lean():
            # Le message lui-même n'arrive qu'une fois le lien classique ouvert.
            def fetched(ok):
                if ok:
                    self.broadcast(dict(message, command="bt_message"))
                self.release("arrival")
            self.take("arrival", then=fetched)
            return
        self.broadcast(dict(message, command="bt_message"))

    def calls_event(self, with_names):
        calls = []
        for c in self.calls:
            c = dict(c)
            if not with_names:
                c["name"] = ""
            calls.append(c)
        return {"command": "bt_calls", "calls": calls}

    # --- envoi --------------------------------------------------------------

    def _write(self, writer, text):
        try:
            writer.write(text.encode() + b"\n")
        except Exception:
            self.clients.pop(writer, None)

    def reply(self, writer, obj):
        self._write(writer, json.dumps(obj, ensure_ascii=False))

    def broadcast(self, obj):
        for writer, subscribed in list(self.clients.items()):
            if subscribed:
                self.reply(writer, obj)

    def broadcast_state(self):
        self.broadcast(self.status())
        self.broadcast(self.devices())
        self.broadcast(self.connection())
        self.sync_presence()
        if self.daily and self.protocol is not None:
            # Le moteur récent republie la route du son quand les liens bougent.
            self.broadcast(self.audio_state())

    def audio_state(self):
        calls = self.calls_enabled and self.call_switchable
        return {
            "command": "bt_audio", "phone_audio": self.phone_audio,
            "phone_audio_route": self.phone_audio_route(),
            "phone_audio_rule": False, "phone_audio_rule_installed": False,
            "phone_audio_rule_path": "/home/essai/.config/wireplumber/wireplumber.conf.d/zz-landline-phone-audio.conf",
            "call_audio": self.call_audio, "call_audio_route": "here" if self.audio == "active" else "phone",
            "call_audio_switchable": calls, "call_audio_reject": (self.call_audio == "phone") if calls else None,
        }

    def phone_audio_route(self):
        # Comme le vrai : « unavailable » tant que le lien classique est fermé,
        # ce qui est l'état ordinaire du mode sobre.
        if not self.classic_up():
            return "unavailable"
        return "here" if self.phone_audio == "here" else "phone"

    def follow_phone_audio(self):
        """Mode sobre : le son demandé ici prend un bail « audio » qui ouvre le lien (#38).
        Comme le vrai depuis #43, le bail n'est pas tenu pour le choix seul : « son-silence »
        le rend (le moteur le lâche après un silence) ; une nouvelle demande le reprend."""
        if not self.lean():
            return
        if self.phone_audio == "here":
            self.take("audio", then=lambda ok: self.broadcast(self.audio_state()))
        else:
            self.release("audio")

    def presence_state(self, reason="out_of_range", lock=None):
        # Un lien classique ouvert compte encore comme présence.
        if (self.le_up() or self.classic_up()) if self.lean() else self.linked:
            return {"command": "bt_presence", "state": "here", "reason": "", "away_seconds": 0,
                    "lock": "none", "lock_in_seconds": 0, "wake": False}
        if lock is None:
            lock = "off" if not self.lock_on_away else "pending" if reason == "out_of_range" else "spared"
        return {"command": "bt_presence", "state": "away", "reason": reason, "away_seconds": 3,
                "lock": lock, "lock_in_seconds": self.lock_seconds - 3 if lock == "pending" else 0, "wake": False}

    def sync_presence(self, reason="out_of_range", lock=None):
        """Publie bt_presence quand l'iPhone part ou revient (moteur récent)."""
        if not self.daily or not self.chosen:
            return
        state = self.presence_state(reason, lock)
        key = (state["state"], state["reason"], state["lock"])
        if key != self.presence:
            self.presence = key
            self.last_presence = state
            self.broadcast(state)

    def set_call_audio_now(self, here):
        """Déplace le son de l'appel en cours ; « son-coince » : il ne repart pas."""
        if not self.calls:
            return
        if here or not self.stuck_here:
            self.audio = "active" if here else "idle"
        if self.audio == "active":
            self.audio_failed, self.audio_reason = False, ""
        self.broadcast(self.connection())
        self.broadcast(self.audio_state())

    # --- commandes des clients ----------------------------------------------

    async def handle(self, reader, writer):
        self.clients[writer] = False
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                text = line.decode(errors="replace").strip()
                if not text:
                    continue
                print(f"RECU {text}", flush=True)
                try:
                    command = json.loads(text)
                    if not isinstance(command, dict):
                        raise ValueError("pas un objet")
                except ValueError:
                    # Le démon ignore le JSON invalide mais accuse réception.
                    self._write(writer, "OK")
                    continue
                self.dispatch(writer, command)
                await writer.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            self.clients.pop(writer, None)
            writer.close()

    def dispatch(self, writer, c):
        name = c.get("command")
        loop = asyncio.get_running_loop()
        if name == "subscribe":
            self.clients[writer] = True
            self.reply(writer, {"command": "protocol_info", "version": 1,
                                "capabilities": ["bluetooth", "messages", "notifications", "calls"]})
            self.reply(writer, {"command": "state_snapshot", "network_enabled": False, "peers": [], "files": []})
            self.reply(writer, self.connection())
        elif name == "unsubscribe":
            self.clients[writer] = False
            self.reply(writer, {"command": "unsubscribed"})
        elif name == "protocol_info":
            self.reply(writer, {"command": "protocol_info", "version": 1,
                                "capabilities": ["bluetooth", "messages", "notifications", "calls"]})
        elif name == "state_snapshot":
            self.reply(writer, {"command": "state_snapshot", "network_enabled": False, "peers": [], "files": []})
        elif name == "bt_status":
            self.reply(writer, self.status())
        elif name == "bt_connection":
            self.reply(writer, self.connection())
        elif name == "bt_list_devices":
            self.reply(writer, self.devices())
        elif name == "bt_list_notifications":
            self.reply(writer, {"command": "bt_notifications",
                                "notifications": [dict(n, silent=True) for n in self.notifications]})
        elif name == "bt_list_calls":
            calls = self.calls_event(with_names=True)
            if self.protocol is not None:
                calls["incoming"] = self.ring_event() if self.ring else None
            self.reply(writer, calls)
        elif name == "bt_list_call_history":
            self.reply(writer, {"command": "bt_call_history", "calls": self.history})
        elif name == "bt_list_threads":
            self.reply(writer, self.threads())
        elif name == "bt_list_messages":
            thread = c.get("thread")
            self.reply(writer, {"command": "bt_messages", "thread": thread,
                                "messages": [m for m in self.messages if m["thread"] == thread][-200:]})
        elif name == "bt_list_contacts":
            contacts = [{"name": CAMILLE["name"], "favorite": True, "addresses": [CAMILLE["thread"]]},
                        {"name": LEO["name"], "favorite": False, "addresses": [LEO["thread"]]}]
            contacts += [{"name": f"Contact {i:03d}", "favorite": False,
                          "addresses": [f"tel:+4179000{i:04d}"]} for i in range(10, 222)]
            query = str(c.get("query", "")).lower()
            if query:
                contacts = [k for k in contacts if query in k["name"].lower()]
            self.reply(writer, {"command": "bt_contacts", "query": c.get("query", ""),
                                "contacts": contacts[:int(c.get("limit", 100))]})
        elif name == "bt_send_message":
            self._write(writer, "OK")
            if self.protocol is None:
                loop.call_later(0.4, self.finish_send, c)
            else:
                self.send_on_demand(c)
        elif name == "bt_set_link_mode" and self.protocol is not None:
            self._write(writer, "OK")
            if c.get("mode") in ("lean", "full"):
                self.protocol = c["mode"]
                self.reset_demand()
                self.broadcast(self.status())
                self.broadcast(self.connection())
        elif name == "bt_lease" and self.protocol is not None:
            self._write(writer, "OK")
            if c.get("name") in ("reading", "setup"):
                if c.get("held", True) is True:
                    self.take(c["name"])
                else:
                    self.release(c["name"])
        elif name == "bt_retry_profiles" and self.lean():
            # Sur un lien fermé il n'y a rien à réessayer : le lien est tenu un moment.
            self._write(writer, "OK")
            self.take("retry")
            loop.call_later(self.delays["reessai"], self.release, "retry")
        elif name == "bt_call_cancel" and self.protocol is not None:
            self._write(writer, "OK")
            self.dial_cancel(c)
        elif name == "bt_call_dial":
            self.dial(writer, c)
        elif name == "bt_mark_read":
            self._write(writer, "OK")
            handles = c.get("handles") or ([c["handle"]] if "handle" in c else [])
            read = c.get("read", True)
            for m in self.messages:
                if m["handle"] in handles:
                    m["read"] = read
            self.broadcast({"command": "bt_message_read", "handles": handles, "read": read,
                            "success": True, "synced": True})
        elif name == "bt_notification_action":
            self._write(writer, "OK")
            uid = c.get("uid")
            known = any(n["uid"] == uid for n in self.notifications)
            self.broadcast({"command": "bt_notification_action_result", "uid": uid, "success": known})
            if known and c.get("action") == "negative":
                self.notifications = [n for n in self.notifications if n["uid"] != uid]
                self.broadcast({"command": "bt_notification_removed", "uid": uid})
        elif name == "bt_audio" and self.daily:
            self.reply(writer, self.audio_state())
        elif name == "bt_presence" and self.daily:
            self.reply(writer, getattr(self, "last_presence", None) if self.presence else
                       {"command": "bt_presence", "state": "unknown", "reason": "", "away_seconds": 0,
                        "lock": "none", "lock_in_seconds": 0, "wake": False})
        elif name == "bt_set_phone_audio" and self.daily:
            self._write(writer, "OK")
            route = c.get("route")
            ok = route in ("phone", "here", "auto")
            if ok and c.get("persist", True):
                self.phone_audio = route
            self.broadcast({"command": "bt_phone_audio_result", "success": ok, "route": route,
                            "persist": c.get("persist", True), **({} if ok else {"message": "invalid_route"})})
            if ok:
                self.follow_phone_audio()
                self.broadcast(self.status())
                self.broadcast(self.audio_state())
        elif name == "bt_set_call_audio" and self.daily:
            self._write(writer, "OK")
            route = c.get("route")
            ok = route in ("phone", "here")
            persist = c.get("persist", True)
            if ok and persist:
                self.call_audio = route
            # Le moteur répond ce qui s'est passé : un lien voix resté ouvert
            # est un échec, pas un « demandé ».
            refused = (ok and route == "phone" and self.refuse_phone and bool(self.calls)
                       and self.audio == "active")
            self.broadcast({"command": "bt_call_audio_result", "success": ok and not (refused and not persist),
                            "route": route, "persist": persist,
                            "applied_now": ok and bool(self.calls) and not refused,
                            **({"message": "The call audio is still on this computer."} if refused else {})})
            if refused:
                self.broadcast(self.status())
                self.broadcast(self.audio_state())
            elif ok:
                self.broadcast(self.status())
                self.broadcast(self.audio_state())
                self.set_call_audio_now(route == "here")
        elif name == "bt_set_lock_on_away":
            self._write(writer, "OK")
            if "enabled" in c:
                self.lock_on_away = c["enabled"] is True
            if "grace_seconds" in c:
                self.lock_seconds = max(1, min(3600, int(c["grace_seconds"])))
            self.broadcast(self.status())
        elif name == "bt_call_action":
            self.call_action(writer, c)
        elif name == "bt_call_tones":
            result = {"command": "bt_call_result", "action": name[8:], "success": False,
                      "message": "Not available in the test engine."}
            self.reply(writer, result)
            self.broadcast(result)
        elif name == "bt_scan":
            self._write(writer, "OK")
            loop.call_later(self.delays["balayage"], self.finish_scan)
        elif name == "bt_pair":
            self._write(writer, "OK")
            # Le temps que l'iPhone affiche sa demande.
            loop.call_later(self.delays["demande"], self.start_pairing, c.get("operation_id"), c.get("address", ADDRESS),
                            False, c.get("fresh") is True)
        elif name == "bt_unpair":
            self._write(writer, "OK")
            loop.call_later(self.delays["oubli"], self.finish_unpair, c)
        elif name == "bt_set_calls":
            self._write(writer, "OK")
            self.calls_enabled = c.get("enabled") is True
            self.broadcast(self.status())
            self.broadcast(self.connection())
            if self.daily:
                self.broadcast(self.audio_state())
        elif name == "bt_pair_confirm":
            self._write(writer, "OK")
            self.finish_pairing(c)
        elif name in ("accept_device", "forget_device", "pair_request", "discover", "send_file",
                      "file_start", "file_chunk", "file_end"):
            error = {"command": "error", "message": "network_disabled", "request": name}
            if "operation_id" in c:
                error["operation_id"] = c["operation_id"]
            self.reply(writer, error)
        else:
            # Réponse par défaut du démon, y compris aux commandes inconnues.
            self._write(writer, "OK")

    def send_on_demand(self, c):
        """Moteur récent : l'envoi dit où il en est, et ouvre le lien s'il le faut."""
        loop = asyncio.get_running_loop()

        def progress(stage):
            event = {"command": "bt_send_progress", "thread": c.get("thread"), "stage": stage}
            if "operation_id" in c:
                event["operation_id"] = c["operation_id"]
            self.broadcast(event)

        def opened(ok):
            if not ok:
                result = {"command": "bt_send_result", "thread": c.get("thread"), "success": False,
                          "message": "Couldn’t reach the iPhone to send."}
                if "operation_id" in c:
                    result["operation_id"] = c["operation_id"]
                self.broadcast(result)
                return
            progress("sending")
            loop.call_later(self.delays["envoi"], finish)

        def finish():
            self.finish_send(c)
            self.release("send")

        if self.lean() and self.classic not in ("open", "lingering"):
            progress("connecting")
        self.take("send", then=opened)

    def finish_send(self, c):
        result = {"command": "bt_send_result", "thread": c.get("thread"), "success": not self.refuse_send}
        if "operation_id" in c:
            result["operation_id"] = c["operation_id"]
        if self.refuse_send:
            result["message"] = "The iPhone refused the message."
            self.refuse_send = False
            self.broadcast(result)
            return
        self.broadcast(result)
        who = next((w for w in SENDERS.values() if w["thread"] == c.get("thread")), CAMILLE)
        message = self._message(who, str(c.get("body", "")), int(time.time()), read=True, outgoing=True)
        self.messages.append(message)
        self.broadcast(dict(message, command="bt_message"))

    def call_action(self, writer, c):
        action = c.get("action")
        success, message = True, None
        ring = self.ring if self.protocol is not None else None
        # Sans passerelle mains-libres, accepter et refuser passent par les
        # actions de la notification (ANCS), désignée par son uid.
        by_note = ring is not None and not self.calls and c.get("uid") == ring["uid"]
        if action == "decline" and self.protocol is None:
            success, message = False, "Unknown action."
        elif action in ("answer", "hangup", "hangup_all", "decline") and not self.calls and not by_note:
            success, message = False, "There is no call."
        elif by_note and action == "answer" and c.get("audio") == "here":
            success, message = False, "This computer is not connected for the call yet."
        result = {"command": "bt_call_result", "action": action, "success": success}
        if self.protocol is not None:
            result["via"] = "ancs" if by_note else "hfp"
        if message:
            result["message"] = message
        self.call_result(writer, result)
        if not success:
            return
        if by_note:
            if action == "answer":
                # Pris sur l'iPhone : la carte d'appel suivra si le mains-libres arrive.
                ring["answered"] = True
                if self.hands_free_never or "call" not in self.leases:
                    asyncio.get_running_loop().call_later(2.0, self.ring_end)
            else:
                self.ring_end()
            return
        if action == "decline":
            action = "hangup"
        if action == "answer":
            # Moteur récent : « audio » dit où ira le son de cet appel.
            if self.daily and c.get("audio") in ("here", "phone"):
                self.audio = "active" if c["audio"] == "here" else "idle"
            self.scenario("decroche")
            if self.daily:
                self.broadcast(self.connection())
                self.broadcast(self.audio_state())
            self.ring_end(keep_lease=True)
        elif action in ("hangup", "hangup_all"):
            # Refusé ou raccroché d'ici : pas un appel manqué.
            self.ring_end(keep_lease=True)
            self.scenario("raccroche")
        elif action == "audio_here":
            self.audio = "active"
            self.broadcast(self.connection())
        elif action == "audio_phone":
            self.audio = "idle"
            self.broadcast(self.connection())

    def finish_scan(self):
        # L'iPhone, écran Bluetooth ouvert, se montre pendant le balayage.
        self.nearby = True
        self.broadcast(self.devices())
        self.broadcast({"command": "bt_scan_result", "success": True, "message": ""})

    def expire_pairing(self):
        op = self.pair_operation
        if op is None:
            return
        self.pair_operation = None
        extra = {"operation_id": op["id"]} if op["id"] is not None else {}
        self.broadcast({"command": "bt_pair_progress", "step": "unanswered", "detail": "client", **extra})
        self.broadcast({"command": "bt_pair_result", "success": False, "status": "timeout",
                        "message": "Pairing timed out.", "address": op["address"], "dual_bond": False,
                        "auth_strategy_used": "connect", **extra})

    def restart(self):
        # Comme un « systemctl restart » : le socket se ferme pour tout le monde,
        # puis le moteur revient, retrouve obexd et remonte les liens.
        for writer in list(self.clients):
            writer.close()
        self.clients.clear()
        self.reset_demand()
        self.obex_down = self.obex_stubborn
        if self.linked:
            self.linked = False
            asyncio.get_running_loop().call_later(self.delays["relance"], self.links_up)

    def finish_unpair(self, c):
        if self.refuse_unpair:
            result = {"command": "bt_unpair_result", "success": False, "status": "error",
                      "reason": "error", "message": "BlueZ refused to remove the device.",
                      "address": c.get("address", ADDRESS), "removed": False, "selection_cleared": False}
            if "operation_id" in c:
                result["operation_id"] = c["operation_id"]
            self.broadcast(result)
            return
        # BlueZ oublie l'appareil ; le choix du moteur reste en place.
        self.paired, self.linked, self.half, self.auth_fail, self.nearby = False, False, False, False, False
        self.le_bond = True
        self.fresh_pairing = True
        result = {"command": "bt_unpair_result", "success": True, "status": "unpaired",
                  "message": "", "address": c.get("address", ADDRESS)}
        if not self.legacy:
            # Le moteur corrigé efface aussi la sélection.
            result.update(reason="", removed=True, selection_cleared=self.chosen)
            self.chosen = False
        if "operation_id" in c:
            result["operation_id"] = c["operation_id"]
        self.broadcast(result)
        self.broadcast_state()

    def tick_auth_failures(self):
        # Le lien BR/EDR monte, le LE est refusé, tout retombe, et on recommence.
        if not self.auth_fail:
            return
        self.flap += 1
        self.broadcast(self.connection())
        asyncio.get_running_loop().call_later(1.0, self.tick_auth_failures)

    def start_pairing(self, operation_id, address=ADDRESS, force=False, fresh=False):
        extra = {"operation_id": operation_id} if operation_id is not None else {}
        stale = self.paired and (self.auth_fail or not self.le_bond)
        if self.paired and fresh and not self.legacy:
            # « fresh » : l'ancien appairage est retiré, puis tout recommence.
            self.broadcast({"command": "bt_pair_progress", "step": "removing_bond", "detail": "", **extra})
            self.paired, self.linked, self.half, self.auth_fail, self.le_bond = False, False, False, False, True
            self.fresh_pairing = True
            self.broadcast_state()
        elif stale and not force and not self.legacy:
            # Appairage inutilisable : dit tel quel, rien n'est touché.
            detail = self.bond()["bond_detail"]
            self.broadcast({"command": "bt_pair_progress", "step": "stale_bond", "detail": detail, **extra})
            self.broadcast({"command": "bt_pair_result", "success": False, "status": "stale_bond",
                            "reason": "stale_bond", "message": "The existing pairing cannot be used.",
                            "address": address, "dual_bond": False, "auth_strategy_used": "",
                            **self.bond(), **extra})
            return
        if self.paired and not force:
            # Déjà appairé pour BlueZ : pas de code, succès annoncé tout de suite,
            # que le lien fonctionne ou non.
            self.broadcast({"command": "bt_pair_progress", "step": "already_paired",
                            "detail": "iPhone de Camille", **extra})
            self.chosen = True
            if self.le_bond and not self.auth_fail:
                self.linked = True
            else:
                self.half = True
            self.broadcast({"command": "bt_pair_result", "success": True, "status": "already_paired",
                            "message": "iPhone de Camille is already paired.", "address": address,
                            "dual_bond": self.le_bond, "auth_strategy_used": "",
                            **({} if self.legacy else {"reason": ""}), **self.bond(), **extra})
            self.broadcast_state()
            return
        self.pair_operation = {"id": operation_id, "address": address}
        self.broadcast({"command": "bt_pair_progress", "step": "connecting", "detail": "", **extra})
        self.broadcast({"command": "bt_pair_progress", "step": "confirm", "detail": "703405", **extra})
        self.broadcast({"command": "bt_pair_confirm_request", "address": address, "code": "703405",
                        "timeout_seconds": 60, **extra})

    def finish_pairing(self, c):
        op = self.pair_operation
        if op is None:
            return
        if op["id"] is not None and c.get("operation_id") != op["id"]:
            return     # réponse portant un autre identifiant : ignorée
        self.pair_operation = None
        accept = c.get("accept") is True
        extra = {"operation_id": op["id"]} if op["id"] is not None else {}
        self.broadcast({"command": "bt_pair_progress", "step": "confirmed" if accept else "declined",
                        "detail": "client", **extra})
        if accept:
            # Lien créé ; les trois réglages de l'iPhone restent à activer.
            if not self.chosen or self.fresh_pairing:
                self.switches = {"notifications": False, "messages": False, "contacts": False}
            self.fresh_pairing = False
            self.paired, self.chosen, self.le_bond = True, True, not self.no_le
            if self.protocol == "lean":
                self.leases.add("setup")     # pris par le moteur à la fin de l'appairage
            self.linked, self.half = self.le_bond, not self.le_bond
            for step in ("paired", "settling", "soliciting"):
                self.broadcast({"command": "bt_pair_progress", "step": step, "detail": "", **extra})
        self.broadcast({"command": "bt_pair_result", "success": accept,
                        "status": "paired" if accept else "rejected",
                        "message": "" if accept else "Pairing was declined.", "address": op["address"],
                        "dual_bond": accept and self.le_bond, "auth_strategy_used": "connect",
                        **({} if self.legacy else {"reason": "" if accept else "rejected"}),
                        **self.bond(), **extra})
        if accept and self.linked:
            # Les liens montent un instant après le résultat.
            self.linked = False
            self.broadcast_state()
            asyncio.get_running_loop().call_later(self.delays["liens"], self.links_up)
            return
        self.broadcast_state()

    def links_up(self):
        if self.paired and self.le_bond and not self.auth_fail:
            self.linked = True
            if self.lean() and self.leases and self.classic == "idle":
                # Un bail attendait le lien LE (parcours des interrupteurs).
                self.classic = "open"
            self.broadcast_state()

    def tick_shaky(self):
        if self.shaky <= 0:
            return
        self.shaky -= 1
        self.linked = not self.linked
        self.broadcast(self.connection())
        if self.shaky > 0:
            asyncio.get_running_loop().call_later(self.shaky_period, self.tick_shaky)

    # --- scénarios ----------------------------------------------------------

    def scenario(self, line):
        words = line.split(None, 1)
        if not words:
            return True
        name, rest = words[0], (words[1] if len(words) > 1 else "")
        now = int(time.time())
        if name in ("relie", "hors-portee", "a-relier", "sans-iphone"):
            self.reset_demand()
            if name == "relie":
                self.obex_down, self.obex_stubborn = False, False
                self.le_pause, self.forced = None, ""
                self.le_wait, self.le_strikes, self.shaky_services = None, 0, False
                self.battery = 96
                self.switches = {"notifications": True, "messages": True, "contacts": True}
            else:
                self.calls, self.audio = [], "idle"
            self.apply_state(name)
            self.broadcast_state()
        elif name == "permission":
            self.apply_state("permission")
            self.broadcast_state()
        elif name == "permission-ok":
            self.setup = []
            self.broadcast_state()
        elif name.startswith("message"):
            who, text = CAMILLE, rest or "On se retrouve à 19h ?"
            if name == "message-leo":
                who = LEO
            elif name == "message-de":
                key, _, text = rest.partition(" ")
                who = SENDERS.get(key, CAMILLE)
                text = text or "On se retrouve à 19h ?"
            elif name == "message-numero":
                who, text = SENDERS["numero"], rest or "Bonjour, c'est le garage. Votre voiture est prête."
            elif name == "message-code":
                who, text = SENDERS["code"], rest or CODE_TEXT
            elif name == "message-email":
                who, text = SENDERS["email"], rest or "Je t'ai envoyé les photos par e-mail, dis-moi si tu les as."
            elif name == "message-long":
                who, text = SENDERS["numero"], rest or LONG_TEXT
            elif name == "message-emoji":
                text = rest or "Trop bien 🎉🎉 on fête ça ce soir ? 🥂"
            elif name == "message-vide":
                text = ""
            elif name == "message-suite":
                loop = asyncio.get_running_loop()
                for i, body in enumerate(["Tu es où ?", "Je suis devant l'entrée", "Ah non c'est bon, je te vois 👋"]):
                    loop.call_later(0.3 * i, self.incoming, CAMILLE, body)
                return True
            self.incoming(who, text)
        elif name == "historique-reel":
            old = [("code", CODE_TEXT, 900), ("numero", LONG_TEXT, 4000), ("email", "", 9000),
                   ("banque", "Votre carte se terminant par 0000 a été utilisée pour CHF 42.50.", 20000)]
            for key, body, ago in old:
                self.messages.append(self._message(SENDERS[key], body, now - ago, read=True))
            self.broadcast(self.threads())
        elif name in ("appel", "appel-inconnu") and self.protocol is not None:
            # Moteur récent : annoncé par sa notification, avant le mains-libres.
            if name == "appel":
                self.ring_start(CAMILLE["address"], CAMILLE["name"])
            else:
                self.ring_start(UNKNOWN_CALLER, "")
        elif name in ("sobre", "complet", "protocole-ancien"):
            self.protocol = {"sobre": "lean", "complet": "full"}.get(name)
            if self.protocol is None:
                self.le_fields, self.le_pause = False, None
            self.forced = ""
            self.reset_demand()
            self.broadcast_state()
        elif name in ("sans-accord", "accord"):
            # Démarrage sans accord des notifications mémorisé : mode complet,
            # motif notifications_off, jusqu'à la première session prête.
            self.forced = "notifications_off" if name == "sans-accord" else ""
            self.reset_demand()
            self.broadcast_state()
        elif name in ("garde-le-lien", "rend-le-lien"):
            self.forced = "phone_keeps_link" if name == "garde-le-lien" else ""
            self.reset_demand()
            self.broadcast_state()
        elif name == "lien-inutilise":
            strikes, _, cause = rest.partition(" ")
            self.le_fields, self.le_pause = True, None
            self.paired, self.chosen, self.le_bond = True, True, True
            self.linked, self.shaky_services, self.le_wait = True, True, None
            self.broadcast(self.connection())
            self.sync_presence()

            def died():
                self.linked = False
                self.le_strikes = int(strikes or 1)
                self.le_wait = cause or "unsteady"
                self.broadcast(self.connection())
                self.sync_presence("out_of_range")
            asyncio.get_running_loop().call_later(0.9, died)
        elif name == "rappel":
            self.le_wait = "unsteady"
            self.broadcast(self.connection())
        elif name == "reveil":
            self.le_fields, self.le_pause, self.le_wait = True, None, "after_resume"
            self.calls, self.audio = [], "idle"
            self.linked = False
            self.broadcast(self.connection())
            self.sync_presence("computer_suspended")
        elif name in ("notifications-en-pause", "notifications-reprises"):
            self.le_fields = True
            self.le_pause = (rest or "phone_silent") if name == "notifications-en-pause" else None
            if self.lean():
                self.reset_demand()
            self.broadcast_state()
        elif name == "tolerance":
            self.tolerance = None if rest.strip() == "aucune" else (rest.strip() or "applied")
            self.broadcast_state()
        elif name == "son-silence":
            # #43 : le son voulu ici, rien ne joue ; le bail « audio » est rendu.
            self.release("audio")
        elif name in ("refuse-ouverture", "accepte-ouverture"):
            self.refuse_open = name == "refuse-ouverture"
        elif name == "mains-libres":
            self.hands_free_never = rest == "jamais"
        elif name == "composition":
            self.dial_mode = rest or "normale"
        elif name == "son-appel":
            self.sound_mode = rest or "normal"
        elif name in ("appel", "appel-inconnu"):
            known = name == "appel"
            self.calls = [{"path": CALL_PATH, "number": CAMILLE["address"] if known else UNKNOWN_CALLER,
                           "withheld": False,
                           "name": CAMILLE["name"] if known else "", "incoming_line": "", "state": "incoming",
                           "multiparty": False, "ringing": True, "outgoing": False, "connected": False}]
            self.broadcast(self.connection())
            self.broadcast(self.calls_event(with_names=False))
        elif name == "decroche":
            if self.calls:
                self.calls[0].update(state="active", ringing=False, connected=True, outgoing=False)
                self.broadcast(self.calls_event(with_names=False))
                self.ring_end(keep_lease=True)
        elif name == "raccroche" and self.ring and not self.calls:
            # Pendant la sonnerie, avant le mains-libres : l'appelant renonce.
            self.ring_end(missed=True)
        elif name == "raccroche":
            missed = self.ring is not None
            self.ring_end(missed=missed, keep_lease=True)
            if self.calls and not missed:
                self.history.insert(0, {"number": self.calls[0]["number"], "name": self.calls[0]["name"],
                                        "type": "received", "timestamp": now})
            self.calls, self.audio = [], "idle"
            self.audio_wanted, self.audio_failed, self.audio_reason = None, False, ""
            self.broadcast(self.calls_event(with_names=False))
            self.broadcast(self.connection())
            self.broadcast({"command": "bt_call_history", "calls": self.history})
            self.release("call")
        elif name == "nom":
            self.name = rest or "iPhone de Camille"
            self.broadcast(self.devices())
        elif name in ("batterie-faible", "batterie"):
            before = self.battery
            self.battery = 12 if name == "batterie-faible" else int(rest or 50)
            self.broadcast(self.connection())
            # Moteur récent : un événement par seuil franchi (20 %, 10 %).
            if self.daily and self.linked:
                for threshold in (10, 20):
                    if self.battery <= threshold < before:
                        self.broadcast({"command": "bt_battery_low", "level": self.battery,
                                        "threshold": threshold, "source": "le", "initial": False,
                                        "state": "critical" if threshold == 10 else "low"})
                        break
        elif name in ("quotidien-ancien", "quotidien-nouveau"):
            # Moteur d'avant ou d'après docs/moteur-quotidien.md.
            self.daily = name == "quotidien-nouveau"
            self.broadcast_state()
            if self.daily:
                self.broadcast(self.audio_state())
        elif name in ("appels-sans-son-ici", "appels-son-ici"):
            self.call_switchable = name == "appels-son-ici"
            self.broadcast(self.audio_state())
        elif name in ("son-coince", "son-refuse", "son-libre"):
            self.stuck_here = name == "son-coince"
            self.refuse_phone = name == "son-refuse"
        elif name == "eloigne":
            # « eloigne [motif [verrou]] » : l'iPhone part ; motif out_of_range (défaut),
            # switched_off, computer_suspended ; verrou off, spared, pending, locked, failed.
            reason, _, lock = rest.partition(" ")
            self.calls, self.audio = [], "idle"
            self.linked = False
            self.broadcast(self.status())
            self.broadcast(self.devices())
            self.broadcast(self.connection())
            self.sync_presence(reason or "out_of_range", lock or None)
        elif name == "notification-appli":
            # « notification-appli whatsapp|telegram|signal|teams|mail|calendrier|groupe [expéditeur] »
            app, _, who = rest.partition(" ")
            apps = {"whatsapp": ("net.whatsapp.WhatsApp", "WhatsApp"), "telegram": ("ph.telegra.Telegraph", "Telegram"),
                    "signal": ("org.whispersystems.signal", "Signal"), "groupe": ("net.whatsapp.WhatsApp", "WhatsApp"),
                    "mail": ("com.apple.mobilemail", "Mail"), "discord": ("com.hammerandchisel.discord", "Discord"),
                    "teams": ("com.microsoft.skype.teams", "Teams"), "calendrier": ("com.apple.mobilecal", "Calendar")}
            # « signal-vocal », « whatsapp-photo » : ce que l'appli écrit à la place du média (#49).
            app, _, media = app.partition("-")
            app_id, app_name = apps.get(app, apps["whatsapp"])
            self.next_uid += 1
            n = {"uid": self.next_uid, "app_id": app_id, "app_name": app_name,
                 "title": "Les voisins" if app == "groupe" else (who or CAMILLE["name"]),
                 "subtitle": (who or CAMILLE["name"]) if app == "groupe" else "",
                 "body": {"vocal": "🎤 Voice message", "photo": "📷 Photo", "video": "🎥 Video"}.get(
                     media, "On mange ensemble à midi ?"),
                 "category": 4, "timestamp": now, "silent": False,
                 "positive_action": False, "negative_action": True}
            self.notifications.append(n)
            self.broadcast(dict(n, command="bt_notification"))
        elif name == "appairage":
            self.start_pairing("essai-1", force=True)
        elif name == "appairage-partiel":
            self.paired, self.le_bond, self.chosen, self.linked = True, False, False, False
            self.half, self.auth_fail = False, False
            self.broadcast_state()
        elif name == "echecs-authentification":
            self.paired, self.le_bond, self.chosen, self.linked = True, True, True, False
            self.half, self.auth_fail = True, True
            self.broadcast_state()
            asyncio.get_running_loop().call_later(1.0, self.tick_auth_failures)
        elif name == "delai":
            what, _, value = rest.partition(" ")
            if what in self.delays:
                self.delays[what] = float(value or 1)
        elif name in ("contrat-ancien", "contrat-nouveau"):
            self.legacy = name == "contrat-ancien"
            self.broadcast_state()
        elif name in ("sans-le", "avec-le"):
            self.no_le = name == "sans-le"
        elif name in ("obex-injoignable", "obex-tenace", "obex-retabli", "bus-ferme"):
            self.obex_down = name != "obex-retabli"
            self.obex_stubborn = name == "obex-tenace"
            self.obex_error = "bus_closed" if name == "bus-ferme" else "other"
            self.broadcast(self.connection())
            if name == "bus-ferme":
                asyncio.get_running_loop().call_later(2.0, self.restart)
        elif name == "relance":
            self.restart()
        elif name in ("refuse-oubli", "accepte-oubli"):
            self.refuse_unpair = name == "refuse-oubli"
        elif name in ("bluetooth-eteint", "bluetooth-allume"):
            self.powered = name == "bluetooth-allume"
            self.linked = self.powered and self.paired and self.chosen
            self.broadcast_state()
        elif name == "liens-battants":
            # Appairage cassé d'un côté, vu en réel : les liens montent et
            # retombent sans qu'aucun service ne s'ouvre. « liens-battants 8 1.5 » :
            # huit bascules, une toutes les 1,5 s ; « liens-stables » arrête.
            count, _, period = rest.partition(" ")
            self.paired, self.chosen, self.le_bond = True, True, True
            self.half, self.auth_fail = False, False
            self.shaky, self.shaky_services = int(count or 8), True
            self.shaky_period = float(period or 1.5)
            self.tick_shaky()
        elif name == "liens-stables":
            self.shaky, self.shaky_services = 0, False
            self.linked = True
            self.broadcast_state()
        elif name == "expire-appairage":
            self.expire_pairing()
        elif name == "interrupteur":
            if rest in self.switches:
                self.switches[rest] = True
                self.broadcast(self.connection())
            else:
                print(f"interrupteur inconnu : {rest}", file=sys.stderr, flush=True)
        elif name == "rafale":
            apps = [("com.exemple.Courriel", "Courriel", 6), ("ch.exemple.Agenda", "Agenda", 5),
                    ("org.exemple.Infos", "Infos", 7), ("net.exemple.Discussion", "Discussion", 4)]
            for i in range(int(rest or 40)):
                app_id, app_name, category = apps[i % len(apps)]
                self.next_uid += 1
                n = {"uid": self.next_uid, "app_id": app_id, "app_name": app_name,
                     "title": f"Ancienne notification {i + 1}", "subtitle": "", "body": "Déjà sur l'iPhone",
                     "category": category, "timestamp": now - 3600 - i * 60, "silent": True,
                     "positive_action": False, "negative_action": True}
                self.notifications.append(n)
                self.broadcast(dict(n, command="bt_notification"))
        elif name == "pile":
            # Ce qui s'est accumulé sur l'iPhone pendant qu'il était loin : rien
            # n'est diffusé, « reconnexion » (ou la liste) le renverra, silent.
            stack = [("net.whatsapp.WhatsApp", "WhatsApp", "Camille Martin", "Tu rentres à quelle heure ?", 4),
                     ("net.whatsapp.WhatsApp", "WhatsApp", "Camille Martin", "J'ai pris le pain", 4),
                     ("com.apple.mobilemail", "Mail", "Régie du Lac", "Votre décompte de charges 2026", 6),
                     ("net.whatsapp.WhatsApp", "WhatsApp", "Léo", "Photo", 4),
                     ("ch.exemple.Agenda", "Calendar", "Dentiste", "Demain à 9 h 30", 5),
                     ("net.whatsapp.WhatsApp", "WhatsApp", "Les voisins", "Apéro samedi ?", 4),
                     ("com.apple.mobilemail", "Mail", "CFF", "Votre billet Genève – Lausanne", 6),
                     ("net.whatsapp.WhatsApp", "WhatsApp", "Léo", "Tu as vu ça ?", 4),
                     ("org.exemple.Infos", "News", "À la une", "Le Léman au plus haut depuis dix ans", 7),
                     ("org.whispersystems.signal", "Signal", "Marc", "Appelle-moi quand tu peux", 4)]
            for i, (app_id, app_name, title, body, category) in enumerate(stack[:int(rest or len(stack))]):
                self.next_uid += 1
                self.notifications.append(
                    {"uid": self.next_uid, "app_id": app_id, "app_name": app_name, "title": title, "subtitle": "",
                     "body": body, "category": category, "timestamp": now - 240 * (i + 1), "silent": True,
                     "positive_action": False, "negative_action": True})
        elif name in ("reconnexion", "renvoi-bruyant"):
            # « reconnexion » : le lien tombe et remonte, et le moteur renvoie toute
            # la pile de l'iPhone, marquée silent, comme à chaque reconnexion réelle.
            # « renvoi-bruyant » : la même pile renvoyée sans silent, sous de
            # nouveaux numéros (ce qu'un moteur fautif pourrait faire).
            if name == "reconnexion":
                self.linked = False
                self.broadcast_state()
                self.linked = True
                self.broadcast_state()
            for n in self.notifications:
                if name == "renvoi-bruyant":
                    self.next_uid += 1
                    n["uid"] = self.next_uid
                self.broadcast(dict(n, command="bt_notification", silent=name == "reconnexion"))
        elif name == "notification":
            self.next_uid += 1
            n = {"uid": self.next_uid, "app_id": "ch.exemple.Agenda", "app_name": "Agenda",
                 "title": rest or "Dentiste", "subtitle": "", "body": "Demain à 9 h 30",
                 "category": 5, "timestamp": now, "silent": False,
                 "positive_action": False, "negative_action": True}
            self.notifications.append(n)
            self.broadcast(dict(n, command="bt_notification"))
        elif name in ("appel-manque", "repondeur"):
            # Ce que l'iPhone publie lui-même, par l'appli Téléphone (ANCS :
            # catégorie 2, appel manqué ; catégorie 3, messagerie vocale).
            self.next_uid += 1
            missed = name == "appel-manque"
            n = {"uid": self.next_uid, "app_id": "com.apple.mobilephone", "app_name": "Phone",
                 "title": rest or ("Camille Martin" if missed else "Voicemail"), "subtitle": "",
                 "body": "Missed Call" if missed else "Camille Martin",
                 "category": 2 if missed else 3, "timestamp": now, "silent": False,
                 "positive_action": True, "negative_action": True}
            self.notifications.append(n)
            self.broadcast(dict(n, command="bt_notification"))
            if missed:
                self.history.insert(0, {"number": "+41790000001", "name": n["title"], "type": "missed",
                                        "timestamp": now})
                self.broadcast({"command": "bt_call_history", "calls": self.history})
        elif name == "retire-notification":
            if self.notifications:
                n = self.notifications.pop()
                self.broadcast({"command": "bt_notification_removed", "uid": n["uid"]})
        elif name == "refuse-envoi":
            self.refuse_send = True
        elif name == "quitter":
            return False
        else:
            print(f"scénario inconnu : {line}", file=sys.stderr, flush=True)
        return True


async def play(moteur, steps):
    # Les scénarios donnés en arguments attendent qu'un client soit abonné.
    while steps and not any(moteur.clients.values()):
        await asyncio.sleep(0.1)
    for step in steps:
        if step.startswith("attendre:"):
            await asyncio.sleep(float(step.split(":", 1)[1]))
        elif not moteur.scenario(step.replace(":", " ", 1)):
            return False
    return True


async def read_stdin(moteur):
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    try:
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    except (ValueError, OSError):
        await asyncio.Event().wait()      # entrée standard inutilisable : rester en service
    while True:
        line = await reader.readline()
        if not line:
            await asyncio.Event().wait()  # entrée fermée : rester en service
        text = line.decode(errors="replace").strip()
        if text.startswith("attendre:"):
            await asyncio.sleep(float(text.split(":", 1)[1]))
        elif text and not moteur.scenario(text):
            return


async def main():
    parser = argparse.ArgumentParser(description="Faux moteur pour l'extension (données fictives).")
    parser.add_argument("--socket", required=True, help="chemin du socket de test à créer")
    parser.add_argument("--etat", default="relie",
                        choices=["relie", "hors-portee", "permission", "a-relier", "sans-iphone"],
                        help="état de départ (défaut : relie)")
    parser.add_argument("--appels", action="store_true",
                        help="contrôle d'appel déjà activé (le vrai moteur démarre sans)")
    parser.add_argument("scenarios", nargs="*", help="scénarios à jouer au démarrage")
    args = parser.parse_args()

    path = os.path.abspath(args.socket)
    # Le socket du vrai moteur vit dans le dossier d'exécution de la session
    # (/run/user/<uid>) : jamais là. Le même nom dans un dossier d'exécution
    # d'essai est permis.
    real = os.path.realpath(path)
    if real.startswith("/run/user/"):
        sys.exit("refus : ce chemin est dans le dossier d'exécution de la session, celui du vrai moteur")
    if os.path.exists(path):
        os.unlink(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    moteur = FauxMoteur(args.etat)
    moteur.calls_enabled = args.appels
    server = await asyncio.start_unix_server(moteur.handle, path=path)
    print(f"PRET {path}", flush=True)
    try:
        if await play(moteur, args.scenarios):
            await read_stdin(moteur)
    finally:
        server.close()
        if os.path.exists(path):
            os.unlink(path)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
