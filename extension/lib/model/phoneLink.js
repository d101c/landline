// SPDX-License-Identifier: GPL-2.0-or-later
// A phone link (tel:, callto:) opened anywhere on this computer (#50): a
// small confirmation drops from the top bar, "Call Camille Martin?", and
// "Call" goes the way of "Call…" (model/dial.js). Nothing is dialled
// without that click: a link may come from any web page. When the call
// cannot be placed from here, the same card says why.
//
// "Use Landline for phone links" is offered once, discreetly, in the menu
// at rest (#50), when no other app opens tel: links: the extension says so
// with setPhoneLinksOffer(), and the menu that shows the offer remembers it
// at once through onSetting('phone-links-offered').

import {digits, formatNumber, sender} from './format.js';
import {phoneLinkNumber} from '../telLinks.js';

export const withPhoneLink = Base => class extends Base {
    // From the extension, with the link as it was opened. The card opens
    // (the extension opens the menu); true when there is one to show.
    phoneLink(uri) {
        const number = phoneLinkNumber(uri);
        // Whatever was open gives way, except a call on its way.
        this._thread = null;
        this._threadMessages = null;
        this._compose = null;
        this._noteCard = null;
        if (this._dial && !this._dial.pending)
            this._dial = null;
        this._phoneAsk = {
            number, name: number ? this._phoneLinkName(number) : '', where: this._dialDefault(),
            // The calls there already: the card shows over theirs and says
            // why it cannot call. A call that comes after it takes over.
            calls: this._phoneAskCalls(),
            // Why "Call" did not go through, or ''.
            error: '',
        };
        this._cardOpen = true;
        this._changed();
        return true;
    }

    // What rings or is under way, as the phone link card sees it.
    _phoneAskCalls() {
        return [...this._calls.map(c => `call:${c.path}`), ...this._incoming ? [`ring:${this._incoming.uid}`] : []];
    }

    // After the calls changed: one the link did not see come takes over,
    // and the link is forgotten (#56). It never comes back after the call.
    _phoneAskCallsChanged() {
        const ask = this._phoneAsk;
        if (ask && this._phoneAskCalls().some(key => !ask.calls.includes(key)))
            this._phoneAsk = null;
    }

    // The name of the contact, when a conversation or a recent call has
    // this number; '' otherwise.
    _phoneLinkName(number) {
        const named = this._dialName(number);
        if (named && named !== formatNumber(number))
            return named;
        const wanted = digits(number).slice(-9);
        if (wanted.length < 6)
            return '';
        const call = this._history.find(c => c.name && digits(c.number).endsWith(wanted));
        return call?.name ?? '';
    }

    // Why the call cannot be placed from here, or ''.
    _phoneAskProblem() {
        const _ = this._;
        const ask = this._phoneAsk;
        if (!ask.number)
            return _('This link has no phone number that can be called.');
        if (this._engine !== 'connected')
            return _('Landline isn’t running, so the call can’t be placed from here.');
        if (this._link() !== 'linked')
            return _('Your iPhone isn’t connected, so the call can’t be placed from here.');
        if (!this._callsUsable())
            return _('Calls from this computer are turned off in Phone Settings.');
        if (this._activeCall() || this._ringingCall())
            return _('You’re already on a call.');
        if (this._dial?.pending)
            return _('Another call is being placed.');
        return ask.error;
    }

    _phoneAskCard() {
        const _ = this._;
        const ask = this._phoneAsk;
        const shown = ask.number ? formatNumber(ask.number) : '';
        const problem = this._phoneAskProblem();
        const who = ask.name ? sender(ask.name) : sender('', shown);
        return {
            kind: 'phonelink',
            // Another link is another card (the indicator keys cards on it).
            uid: `tel:${ask.number}`,
            // Translators: %s is a name or a phone number.
            name: ask.number ? _('Call %s?').replace('%s', ask.name || shown) : _('Phone link'),
            initials: who.initials,
            // The number under the name.
            subtitle: ask.name ? shown : ask.number ? _('Through your iPhone') : '',
            isError: false,
            // Why it cannot be called, in full under the header; '' when it can.
            why: problem,
            number: ask.number,
            canCall: problem === '',
            // null with an engine that cannot say where the sound goes.
            audio: problem === '' && this._dialAudioChoice() ? ask.where : null,
        };
    }

    // 'computer' or 'phone'.
    setPhoneLinkAudio(where) {
        if (!this._phoneAsk || !['computer', 'phone'].includes(where))
            return;
        this._phoneAsk.where = where;
        this._changed();
    }

    // "Call": the way of "Call…", which shows how it goes. Refused there,
    // the card stays and says so (#56).
    confirmPhoneLink() {
        const ask = this._phoneAsk;
        if (!ask || this._phoneAskProblem())
            return false;
        if (!this.dial(ask.number, ask.where, ask.name)) {
            ask.error = this._('The call could not be placed.');
            this._changed();
            return false;
        }
        this._phoneAsk = null;
        this._changed();
        return true;
    }

    // "Cancel": nothing is called.
    cancelPhoneLink() {
        if (!this._phoneAsk)
            return;
        this._phoneAsk = null;
        this._cardOpen = false;
        this._changed();
    }

    // --- the offer of the first run (#50) ---------------------------------------

    // From the extension: true while "Use Landline for phone links" is
    // off, has never been offered, and no app opens tel: or callto: links.
    setPhoneLinksOffer(wanted) {
        // Shown once: never again in this session either.
        wanted = wanted === true && !this._phoneLinksOffered;
        if (wanted === this._phoneLinksOffer)
            return;
        this._phoneLinksOffer = wanted;
        this._changed();
    }

    // The menu at rest just opened: the offer is in it, for as long as it
    // stays open, and never again.
    _phoneLinksOfferSeen() {
        if (!this._phoneLinksOffer || this._phoneLinksOffering || !this._canDial())
            return;
        this._phoneLinksOffer = false;
        this._phoneLinksOffered = true;
        this._phoneLinksOffering = true;
        this.onSetting?.('phone-links-offered', true);
    }

    // "Use Landline" (the extension writes the handler) or "Not Now".
    answerPhoneLinksOffer() {
        if (!this._phoneLinksOffering)
            return;
        this._phoneLinksOffering = false;
        this._changed();
    }
};
