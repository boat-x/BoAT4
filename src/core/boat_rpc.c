/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/*----------------------------------------------------------------------------
 * Generic JSON-RPC client
 *--------------------------------------------------------------------------*/
typedef struct {
    char url[256];
    int  req_id;
} BoatRpcCtx;

BoatResult boat_rpc_ctx_init(BoatRpcCtx *ctx, const char *url)
{
    if (!ctx || !url) return BOAT_ERROR_ARG_NULL;
    memset(ctx, 0, sizeof(BoatRpcCtx));
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->req_id = 1;
    return BOAT_SUCCESS;
}

void boat_rpc_ctx_free(BoatRpcCtx *ctx)
{
    if (ctx) memset(ctx, 0, sizeof(BoatRpcCtx));
}

BoatResult boat_rpc_call(BoatRpcCtx *ctx, const char *method, const char *params_json,
                         char **result_json)
{
    if (!ctx || !method || !result_json) return BOAT_ERROR_ARG_NULL;

    const BoatHttpOps *http = boat_get_http_ops();
    if (!http || !http->post) return BOAT_ERROR_RPC_FAIL;

    /* Build JSON-RPC request */
    char req_buf[2048];
    int n = snprintf(req_buf, sizeof(req_buf),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
                     method,
                     params_json ? params_json : "[]",
                     ctx->req_id++);
    if (n < 0 || (size_t)n >= sizeof(req_buf)) return BOAT_ERROR_MEM_OVERFLOW;

    /* POST */
    BoatHttpResponse response = {0};
    BoatResult r = http->post(ctx->url, "application/json",
                              (const uint8_t *)req_buf, (size_t)n,
                              NULL, &response);
    if (r != BOAT_SUCCESS) return BOAT_ERROR_RPC_FAIL;

    /* Parse JSON response */
    cJSON *root = cJSON_Parse((const char *)response.data);
    if (!root) {
        http->free_response(&response);
        return BOAT_ERROR_RPC_PARSE;
    }

    /* Check for error */
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error && cJSON_IsObject(error)) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        if (msg && cJSON_IsString(msg)) {
            BOAT_LOG(BOAT_LOG_NORMAL, "RPC error: %s", msg->valuestring);
        }
        /* Print simulation logs if available */
        cJSON *data = cJSON_GetObjectItem(error, "data");
        if (data && cJSON_IsObject(data)) {
            cJSON *logs = cJSON_GetObjectItem(data, "logs");
            if (logs && cJSON_IsArray(logs)) {
                cJSON *log_item;
                cJSON_ArrayForEach(log_item, logs) {
                    if (cJSON_IsString(log_item)) {
                        BOAT_LOG(BOAT_LOG_NORMAL, "  log: %s", log_item->valuestring);
                    }
                }
            }
        }
        cJSON_Delete(root);
        http->free_response(&response);
        return BOAT_ERROR_RPC_SERVER;
    }

    /* Extract "result" */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result) {
        cJSON_Delete(root);
        http->free_response(&response);
        return BOAT_ERROR_RPC_PARSE;
    }

    /* Return result as string */
    if (cJSON_IsString(result)) {
        size_t slen = strlen(result->valuestring);
        *result_json = (char *)boat_malloc(slen + 1);
        if (!*result_json) {
            cJSON_Delete(root);
            http->free_response(&response);
            return BOAT_ERROR_MEM_ALLOC;
        }
        memcpy(*result_json, result->valuestring, slen + 1);
    } else {
        char *printed = cJSON_PrintUnformatted(result);
        if (!printed) {
            cJSON_Delete(root);
            http->free_response(&response);
            return BOAT_ERROR_MEM_ALLOC;
        }
        size_t plen = strlen(printed);
        *result_json = (char *)boat_malloc(plen + 1);
        if (!*result_json) {
            cJSON_free(printed);
            cJSON_Delete(root);
            http->free_response(&response);
            return BOAT_ERROR_MEM_ALLOC;
        }
        memcpy(*result_json, printed, plen + 1);
        cJSON_free(printed);
    }

    cJSON_Delete(root);
    http->free_response(&response);
    return BOAT_SUCCESS;
}
