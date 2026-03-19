/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_PAL_H
#define BOAT_PAL_H

#include "boat.h"

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------
 * Tier 1: Mandatory platform primitives
 *--------------------------------------------------------------------------*/
void  *boat_malloc(size_t size);
void   boat_free(void *ptr);

BoatResult boat_storage_write(const char *name, const uint8_t *data, size_t len);
BoatResult boat_storage_read(const char *name, uint8_t *data, size_t cap, size_t *out_len);
BoatResult boat_storage_delete(const char *name);

uint64_t boat_time_ms(void);
void     boat_sleep_ms(uint32_t ms);

BoatResult boat_random(uint8_t *buf, size_t len);

/*--- Mutex ---*/
typedef struct BoatMutex BoatMutex;

BoatResult boat_mutex_init(BoatMutex **mutex);
BoatResult boat_mutex_lock(BoatMutex *mutex);
BoatResult boat_mutex_unlock(BoatMutex *mutex);
void       boat_mutex_destroy(BoatMutex *mutex);

/*----------------------------------------------------------------------------
 * Tier 2: HTTP service contract (function-pointer based)
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t *data;
    size_t   len;
} BoatHttpResponse;

typedef struct {
    /** POST request. Caller frees response via free_response(). */
    BoatResult (*post)(const char *url,
                       const char *content_type,
                       const uint8_t *body, size_t body_len,
                       const char *extra_headers,
                       BoatHttpResponse *response);

    /** GET request. Caller frees response via free_response(). */
    BoatResult (*get)(const char *url,
                      const char *extra_headers,
                      BoatHttpResponse *response);

    /** Free response data allocated by post/get. */
    void (*free_response)(BoatHttpResponse *response);
} BoatHttpOps;

void              boat_set_http_ops(const BoatHttpOps *ops);
const BoatHttpOps *boat_get_http_ops(void);

/*----------------------------------------------------------------------------
 * PAL port initialization (called by platform-specific code)
 *--------------------------------------------------------------------------*/
/** Linux default: registers curl-based HTTP ops. */
BoatResult boat_pal_linux_init(void);
const BoatHttpOps *boat_pal_linux_default_http_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_PAL_H */
