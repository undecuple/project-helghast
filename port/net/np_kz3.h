/*
 * KZ3 port: NP sign-in and the ticket calls KZ3's online path needs (the RE notes section 1, row 3):
 *   sceNpManagerRequestTicket2 (NID 0x8297F1EC, called at 0x16D9E4 and 0x16C89C) builds an unsigned v2.1 ticket
 *   (np_ticket.h) and queues the manager callback with event 0xFF (GOT_TICKET), result = the ticket size (>= 0);
 *   sceNpManagerGetTicket (NID 0x0968AA36, called at 0x16E520 size query, 0x16E55C copy) hands it over.
 * Registered only when online with a valid online id (kz3_net_register). Guest pointers are bounds-checked.
 */
#ifndef KZ3_NP_KZ3_H
#define KZ3_NP_KZ3_H

#include <stdint.h>
#include "np_ticket.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP_KZ3_NID_REQUEST_TICKET2 0x8297F1ECu
#define NP_KZ3_NID_GET_TICKET      0x0968AA36u
#define NP_KZ3_EVENT_GOT_TICKET    0xFF
#define NP_KZ3_NID_GET_ACCOUNT_REGION 0xB1E0718Bu   /* called at 0x16D474, 0x16FE7C */
#define NP_KZ3_NID_EULA_CHECK      0x59D1629Au      /* cellSysutilNpEula, 5 args, called at 0x17022C */
#define NP_KZ3_NID_EULA_SHOW       0x6599500Du      /* 4 args, called at 0x17020C */
#define NP_KZ3_NID_EULA_ABORT      0x5EC05AD8u      /* called at 0x170368 */
#define NP_KZ3_EULA_ACCEPTED         1
#define NP_KZ3_EULA_ALREADY_ACCEPTED 2

/* Online id for NP (sceNpGetNpId & co. via np_psnr), the signed-in flag, the two handlers. */
void np_kz3_sign_in(const char* online_id);

/* The handlers' logic on guest addresses (tests call these directly). Return the s32 the title sees. */
int32_t np_kz3_request_ticket2(uint32_t npid_ea, uint32_t version_ea, uint32_t service_id_ea, uint32_t cookie_ea,
                               uint32_t cookie_size, uint32_t entitlement_ea, uint32_t consumed);
int32_t np_kz3_get_ticket(uint32_t buffer_ea, uint32_t size_ea);
int32_t np_kz3_get_account_region(uint32_t cc_ea, uint32_t lang_ea);

/* ticket source. Default: the built-in unsigned v2.1 ticket (for the player's own server). With a ticket file
 * (online.ini ticket_file), RequestTicket2/GetTicket hand over those bytes unchanged after np_ticket_check; returns
 * 0 or the check's negative reason (nothing stored then). Call before np_kz3_sign_in. */
int    np_kz3_set_ticket_file(const uint8_t* buf, size_t len, NpTicketInfo* info);
size_t np_kz3_ticket_source_file(void);   /* bytes of the file ticket, 0 = built-in source */

#ifdef __cplusplus
}
#endif

#endif /* KZ3_NP_KZ3_H */
