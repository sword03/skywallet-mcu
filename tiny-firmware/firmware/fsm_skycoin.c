/*
 * This file is part of the Skycoin project, https://skycoin.net/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 * Copyright (C) 2018-2019 Skycoin Project
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

#include <libopencm3/stm32/flash.h>

#include <inttypes.h>
#include <stdio.h>

#include "skycoin-crypto/check_digest.h"
#include "skycoin-crypto/skycoin_crypto.h"
#include "skycoin-crypto/tools/base58.h"
#include "skycoin-crypto/tools/bip32.h"
#include "skycoin-crypto/tools/bip39.h"
#include "skycoin-crypto/tools/bip44.h"
#include "tiny-firmware/firmware/droplet.h"
#include "tiny-firmware/firmware/entropy.h"
#include "tiny-firmware/firmware/fsm.h"
#include "tiny-firmware/firmware/fsm_impl.h"
#include "tiny-firmware/firmware/fsm_skycoin.h"
#include "tiny-firmware/firmware/fsm_skycoin_impl.h"
#include "tiny-firmware/firmware/gettext.h"
#include "tiny-firmware/firmware/layout2.h"
#include "tiny-firmware/firmware/messages.h"
#include "tiny-firmware/firmware/protect.h"
#include "tiny-firmware/firmware/recovery.h"
#include "tiny-firmware/firmware/reset.h"
#include "tiny-firmware/firmware/skyparams.h"
#include "tiny-firmware/firmware/skywallet.h"
#include "tiny-firmware/firmware/storage.h"
#include "tiny-firmware/memory.h"
#include "tiny-firmware/oled.h"
#include "tiny-firmware/rng.h"
#include "tiny-firmware/usb.h"
#include "tiny-firmware/util.h"

extern uint8_t msg_resp[MSG_OUT_SIZE] __attribute__((aligned));
extern const uint32_t bip44_purpose;

void fsm_msgSkycoinCheckMessageSignature(SkycoinCheckMessageSignature* msg)
{
    GET_MSG_POINTER(Success, successResp);
    GET_MSG_POINTER(Failure, failureResp);
    uint16_t msg_id = MessageType_MessageType_Failure;
    void* msg_ptr = failureResp;
    switch (msgSkycoinCheckMessageSignatureImpl(msg, successResp, failureResp)) {
    case ErrOk:
        msg_id = MessageType_MessageType_Success;
        msg_ptr = successResp;
        layoutRawMessage("Verification success");
        break;
    case ErrAddressGeneration:
    case ErrInvalidSignature:
        failureResp->code = FailureType_Failure_InvalidSignature;
        layoutRawMessage("Wrong signature");
        break;
    default:
        strncpy(failureResp->message, _("Firmware error."), sizeof(failureResp->message));
        layoutHome();
        break;
    }
    msg_write(msg_id, msg_ptr);
}

void fsm_msgSkycoinSignMessage(SkycoinSignMessage* msg)
{
    CHECK_MNEMONIC
    CHECK_PIN_UNCACHED
    RESP_INIT(ResponseSkycoinSignMessage);

    MessageType msgtype = MessageType_MessageType_SkycoinSignMessage;
    ResponseSkycoinAddress respAddr;
    uint8_t seckey[32] = {0};
    uint8_t pubkey[33] = {0};
    if (msg->has_bip44_addr) {
        const char* mnemo = storage_getFullSeed();
        uint8_t seed[512 / 8] = {0};
        mnemonic_to_seed(mnemo, "", seed, NULL);
        size_t addr_size = sizeof(respAddr.addresses[0]);
        int ret = hdnode_address_for_branch(
            seed, sizeof(seed), bip44_purpose, msg->bip44_addr.coin_type,
            msg->bip44_addr.account, msg->bip44_addr.change,
            msg->bip44_addr.address_start_index, respAddr.addresses[0],
            &addr_size);
        if (ret != 1) {
            fsm_sendResponseFromErrCode(
                ErrFailed, NULL, _("Unable to get address"), &msgtype);
            layoutHome();
            return;
        }
    } else {
        ErrCode_t err = fsm_getKeyPairAtIndex(1, pubkey, seckey, &respAddr,
            msg->address_n);
        if (err != ErrOk) {
            fsm_sendResponseFromErrCode(
                err, NULL, _("Unable to get keys pair"), &msgtype);
            layoutHome();
            return;
        }
    }
    layoutDialogSwipe(&bmp_icon_question, _("Cancel"), _("Confirm"), NULL,
        _("Do you really want to"), _("sign message using"),
        _("this address?"), respAddr.addresses[0], NULL, NULL);
    CHECK_BUTTON_PROTECT

    ErrCode_t err = msgSkycoinSignMessageImpl(msg, resp);
    if (err == ErrOk) {
        msg_write(MessageType_MessageType_ResponseSkycoinSignMessage, resp);
        layoutRawMessage("Signature success");
    } else {
        char* failMsg = NULL;
        if (err == ErrMnemonicRequired) {
            failMsg = _("Mnemonic not set");
        }
        fsm_sendResponseFromErrCode(err, NULL, failMsg, &msgtype);
        layoutHome();
    }
}

void fsm_msgSkycoinAddress(SkycoinAddress* msg)
{
    MessageType msgtype = MessageType_MessageType_SkycoinAddress;
    RESP_INIT(ResponseSkycoinAddress);
    char* failMsg = NULL;
    ErrCode_t err = msgSkycoinAddressImpl(msg, resp);
    switch (err) {
    case ErrUserConfirmation:
        layoutAddress(resp->addresses[0]);
        if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
            err = ErrActionCancelled;
            break;
        }
        // fall through
    case ErrOk:
        msg_write(MessageType_MessageType_ResponseSkycoinAddress, resp);
        layoutHome();
        return;
    case ErrPinRequired:
        failMsg = _("Expected pin");
        break;
    case ErrTooManyAddresses:
        failMsg = _("Asking for too much addresses");
        break;
    case ErrMnemonicRequired:
        failMsg = _("Mnemonic required");
        break;
    case ErrAddressGeneration:
        failMsg = _("Key pair generation failed");
        break;
    default:
        break;
    }
    fsm_sendResponseFromErrCode(err, NULL, failMsg, &msgtype);
    layoutHome();
}

void fsm_msgTransactionSign(TransactionSign* msg)
{
    CHECK_PIN
    CHECK_MNEMONIC
    CHECK_INPUTS(msg)
    CHECK_OUTPUTS(msg)

    MessageType msgtype = MessageType_MessageType_TransactionSign;
    RESP_INIT(ResponseTransactionSign);
    ErrCode_t err = msgTransactionSignImpl(msg, &requestConfirmTransaction, resp);
    char* failMsg = NULL;
    switch (err) {
    case ErrOk:
        msg_write(MessageType_MessageType_ResponseTransactionSign, resp);
        break;
    case ErrAddressGeneration:
        failMsg = _("Wrong return address");
        // fall through
    default:
        fsm_sendResponseFromErrCode(err, NULL, failMsg, &msgtype);
        break;
    }
    layoutHome();
}
