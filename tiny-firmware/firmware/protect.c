/*
 * This file is part of the Skycoin project, https://skycoin.net
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 * Copyright (C) 2019 Skycoin Project
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tiny-firmware/firmware/protect.h"
#include "skycoin-crypto/tools/memzero.h"
#include "tiny-firmware/buttons.h"
#include "tiny-firmware/firmware/fsm.h"
#include "tiny-firmware/firmware/gettext.h"
#include "tiny-firmware/firmware/layout2.h"
#include "tiny-firmware/firmware/messages.h"
#include "tiny-firmware/firmware/pinmatrix.h"
#include "tiny-firmware/firmware/storage.h"
#include "tiny-firmware/memory.h"
#include "tiny-firmware/oled.h"
#include "tiny-firmware/usb.h"
#include "tiny-firmware/util.h"

#define MAX_WRONG_PINS 15

bool protectAbortedByInitialize = false;

bool protectButton(ButtonRequestType type, bool confirm_only)
{
    ButtonRequest resp;
    bool result = false;
    bool acked = false;
#if DEBUG_LINK
    bool debug_decided = false;
#endif

    memset(&resp, 0, sizeof(ButtonRequest));
    resp.has_code = true;
    resp.code = type;
    usbTiny(1);
    buttonUpdate(); // Clear button state
    msg_write(MessageType_MessageType_ButtonRequest, &resp);

    for (;;) {
        usbPoll();

        // check for ButtonAck
        if (msg_tiny_id == MessageType_MessageType_ButtonAck) {
            msg_tiny_id = 0xFFFF;
            acked = true;
        }

        // button acked - check buttons
        if (acked) {
            usbSleep(5);
            buttonUpdate();
            if (button.YesUp) {
                result = true;
                break;
            }
            if (!confirm_only && button.NoUp) {
                result = false;
                break;
            }
        }

        // check for Cancel / Initialize
        if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
            if (msg_tiny_id == MessageType_MessageType_Initialize) {
                protectAbortedByInitialize = true;
            }
            msg_tiny_id = 0xFFFF;
            result = false;
            break;
        }

#if DEBUG_LINK
        // check DebugLink
        if (msg_tiny_id == MessageType_MessageType_DebugLinkDecision) {
            msg_tiny_id = 0xFFFF;
            DebugLinkDecision* dld = (DebugLinkDecision*)msg_tiny;
            result = dld->yes_no;
            debug_decided = true;
        }

        if (acked && debug_decided) {
            break;
        }

        if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
            msg_tiny_id = 0xFFFF;
            fsm_msgDebugLinkGetState((DebugLinkGetState*)msg_tiny);
        }
#endif
    }

    usbTiny(0);

    simulateButtonPress = false;

    return result;
}

const char* requestPin(PinMatrixRequestType type, const char* text)
{
    PinMatrixRequest resp;
    memset(&resp, 0, sizeof(PinMatrixRequest));
    resp.has_type = true;
    resp.type = type;
    usbTiny(1);
    msg_write(MessageType_MessageType_PinMatrixRequest, &resp);
    pinmatrix_start(text);
    for (;;) {
        usbPoll();
        if (msg_tiny_id == MessageType_MessageType_PinMatrixAck) {
            msg_tiny_id = 0xFFFF;
            PinMatrixAck* pma = (PinMatrixAck*)msg_tiny;
            pinmatrix_done(pma->pin); // convert via pinmatrix
            usbTiny(0);
            return pma->pin;
        }
        if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
            pinmatrix_done(0);
            if (msg_tiny_id == MessageType_MessageType_Initialize) {
                protectAbortedByInitialize = true;
            }
            msg_tiny_id = 0xFFFF;
            usbTiny(0);
            return 0;
        }
#if DEBUG_LINK
        if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
            msg_tiny_id = 0xFFFF;
            fsm_msgDebugLinkGetState((DebugLinkGetState*)msg_tiny);
        }
#endif
    }
}

static void protectCheckMaxTry(uint32_t wait)
{
    if (wait < (1 << MAX_WRONG_PINS))
        return;

    storage_wipe();
    layoutDialog(&bmp_icon_error, NULL, NULL, NULL, _("Too many wrong PIN"), _("attempts. Storage has"), _("been wiped."), NULL, _("Please unplug"), _("the device."));
    for (;;) {
    } // loop forever
}

bool protectPin(bool use_cached)
{
    if (!storage_hasPin() || (use_cached && session_isPinCached())) {
        return true;
    }
    uint32_t fails = storage_getPinFailsOffset();
    uint32_t wait = storage_getPinWait(fails);
    protectCheckMaxTry(wait);
    usbTiny(1);
    while (wait > 0) {
        // convert wait to secstr string
        char secstrbuf[20];
        strlcpy(secstrbuf, _("________0 seconds"), sizeof(secstrbuf));
        char* secstr = secstrbuf + 9;
        uint32_t secs = wait;
        while (secs > 0 && secstr >= secstrbuf) {
            secstr--;
            *secstr = (secs % 10) + '0';
            secs /= 10;
        }
        if (wait == 1) {
            secstrbuf[16] = 0;
        }
        layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("Wrong PIN entered"), NULL, _("Please wait"), secstr, _("to continue ..."), NULL);
        // wait one second
        usbSleep(1000);
        if (msg_tiny_id == MessageType_MessageType_Initialize) {
            protectAbortedByInitialize = true;
            msg_tiny_id = 0xFFFF;
            usbTiny(0);
            fsm_sendFailure(FailureType_Failure_PinCancelled, NULL, 0);
            return false;
        }
        wait--;
    }
    usbTiny(0);
    const char* pin;
    pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_Current, _("Please enter current PIN:"));
    if (!pin) {
        fsm_sendFailure(FailureType_Failure_PinCancelled, NULL, 0);
        return false;
    }
    if (!storage_increasePinFails(fails)) {
        fsm_sendFailure(FailureType_Failure_PinInvalid, NULL, 0);
        return false;
    }
    if (storage_containsPin(pin)) {
        session_cachePin();
        storage_resetPinFails(fails);
        return true;
    } else {
        protectCheckMaxTry(storage_getPinWait(fails));
        fsm_sendFailure(FailureType_Failure_PinInvalid, NULL, 0);
        return false;
    }
}

bool protectChangePin()
{
    return protectChangePinEx(NULL);
}

bool protectChangePinEx(const char* (*funcRequestPin)(PinMatrixRequestType, const char*))
{
    static CONFIDENTIAL char pin_compare[17];
    if (funcRequestPin == NULL) {
        funcRequestPin = requestPin;
    }

    const char* pin = funcRequestPin(PinMatrixRequestType_PinMatrixRequestType_NewFirst, _("Please enter new PIN:"));

    if (!pin) {
        return false;
    }

    strlcpy(pin_compare, pin, sizeof(pin_compare));

    pin = funcRequestPin(PinMatrixRequestType_PinMatrixRequestType_NewSecond, _("Please re-enter new PIN:"));

    const bool result = pin && *pin && (strncmp(pin_compare, pin, sizeof(pin_compare)) == 0);

    if (result) {
        storage_setPin(pin_compare);
        storage_update();
    }

    memzero(pin_compare, sizeof(pin_compare));

    return result;
}

bool protectPassphrase(void)
{
    if (!storage_hasPassphraseProtection() || session_isPassphraseCached()) {
        return true;
    }

    PassphraseRequest resp;
    memset(&resp, 0, sizeof(PassphraseRequest));
    usbTiny(1);
    msg_write(MessageType_MessageType_PassphraseRequest, &resp);

    layoutDialogSwipe(&bmp_icon_info, NULL, NULL, NULL, _("Please enter your"), _("passphrase using"), _("the computer's"), _("keyboard."), NULL, NULL);

    bool result;
    for (;;) {
        usbPoll();
        // TODO: correctly process PassphraseAck with state field set (mismatch => Failure)
        if (msg_tiny_id == MessageType_MessageType_PassphraseAck) {
            msg_tiny_id = 0xFFFF;
            PassphraseAck* ppa = (PassphraseAck*)msg_tiny;
            session_cachePassphrase(ppa->has_passphrase ? ppa->passphrase : "");
            result = true;
            break;
        }
        if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
            if (msg_tiny_id == MessageType_MessageType_Initialize) {
                protectAbortedByInitialize = true;
            }
            msg_tiny_id = 0xFFFF;
            result = false;
            break;
        }
    }
    usbTiny(0);
    layoutHome();
    return result;
}
