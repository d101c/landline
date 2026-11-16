// SPDX-License-Identifier: GPL-2.0-or-later
// What the iPhone itself writes on the screens the setup tour draws, in the
// language of the iPhone. No Shell or GLib import. The French ones were read
// on an iPhone (iOS 26); a language that is not here falls back to English.
// Section titles are written in capitals, as the iPhone shows them.

export const PHONE_LABELS = {
    en: {
        settings: 'Settings',
        bluetooth: 'Bluetooth',
        myDevices: 'My Devices',
        otherDevices: 'Other Devices',
        connected: 'Connected',
        notConnected: 'Not Connected',
        pairingRequest: 'Bluetooth Pairing Request',
        // %s is the name of this computer.
        request: ['“%s” would like to pair', 'with your iPhone. Confirm that this', 'code is shown on “%s”.'],
        pair: 'Pair',
        cancel: 'Cancel',
        // The page behind the ⓘ, top to bottom.
        showNotifications: 'Show Message Notifications',
        showFooter: 'Allow message notifications to be shown on this device.',
        syncContacts: 'Sync Contacts',
        contactsFooter: 'Allow this accessory to access your contacts for calls and messages.',
        groups: 'GROUPS',
        favorites: 'Phone Favorites',
        recents: 'Recents',
        allContacts: 'All Contacts',
        deviceType: 'Device Type',
        systemNotifications: 'SYSTEM NOTIFICATIONS',
        shareNotifications: 'Share System Notifications',
        forget: 'Forget This Device',
        allow: 'Allow',
        hotspot: 'Personal Hotspot',
        messages: 'Messages',
        now: 'now',
        date: 'Monday 16 November',
    },
    fr: {
        settings: 'Réglages',
        bluetooth: 'Bluetooth',
        myDevices: 'Mes appareils',
        otherDevices: 'Autres appareils',
        connected: 'Connecté',
        notConnected: 'Non connecté',
        pairingRequest: 'Demande de jumelage Bluetooth',
        request: ['« %s » souhaite se jumeler avec', 'votre iPhone. Confirmez que ce code', 'est affiché sur « %s ».'],
        pair: 'Jumeler',
        cancel: 'Annuler',
        showNotifications: 'Afficher les notifications de message',
        showFooter: 'Autorisez l’affichage des notifications de message sur cet appareil.',
        syncContacts: 'Synchroniser les contacts',
        contactsFooter: 'Autorisez cet accessoire à accéder à vos contacts pour les appels et les messages.',
        groups: 'GROUPES',
        favorites: 'Favoris du téléphone',
        recents: 'Historique',
        allContacts: 'Tous les contacts',
        deviceType: 'Type d’appareil',
        systemNotifications: 'NOTIFICATIONS SYSTÈME',
        shareNotifications: 'Partager les notifications système',
        forget: 'Oublier cet appareil',
        allow: 'Autoriser',
        hotspot: 'Partage de connexion',
        messages: 'Messages',
        now: 'maintenant',
        date: 'lundi 16 novembre',
    },
};

export const PHONE_LANGUAGES = Object.keys(PHONE_LABELS);

function known(names) {
    for (const name of names ?? []) {
        const code = String(name).toLowerCase().split(/[_.@-]/)[0];
        if (PHONE_LANGUAGES.includes(code))
            return code;
    }
    return null;
}

// The best guess at the language of the iPhone. wanted: the choice made in
// Phone Settings, '' for automatic. messages: the languages of the desktop
// (GLib.get_language_names()); formats: those of its dates and numbers.
// A desktop in English with French formats is taken for a French iPhone:
// English is where people leave a desktop, less often a phone.
export function phoneLanguage(wanted, messages, formats) {
    if (PHONE_LANGUAGES.includes(wanted))
        return wanted;
    const first = known(messages) ?? 'en';
    if (first !== 'en')
        return first;
    return known(formats) ?? 'en';
}

// A label as the English text of a step names it: in English, with what the
// iPhone really writes in brackets when the iPhone speaks another language.
export function bilingual(language, key) {
    const english = PHONE_LABELS.en[key];
    const local = (PHONE_LABELS[language] ?? PHONE_LABELS.en)[key];
    return local === english ? english : `${english} (${local})`;
}
