/*
 * Copyright 2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "license_service.h"
#include "mbedtls/certs.h"
#include "mbedtls/config.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "safe_str_lib.h"

ovsa_status_t ovsa_server_get_string_length(const char* in_buff, size_t* in_buff_len) {
    ovsa_status_t ret = OVSA_OK;
    size_t total_len = 0, buff_len = 0;

    if (in_buff == NULL) {
        OVSA_DBG(DBG_E, "Error: Getting string length failed with invalid parameter\n");
        ret = OVSA_INVALID_PARAMETER;
        return ret;
    }

    buff_len = strnlen_s(in_buff, RSIZE_MAX_STR);
    if (buff_len < RSIZE_MAX_STR) {
        *in_buff_len = buff_len;
    } else {
        while (buff_len == RSIZE_MAX_STR) {
            total_len += RSIZE_MAX_STR;

            buff_len = strnlen_s((in_buff + total_len), RSIZE_MAX_STR);
            if (buff_len < RSIZE_MAX_STR) {
                total_len += buff_len;
                break;
            }
        }
        *in_buff_len = total_len;
    }

    return ret;
}
ovsa_status_t ovsa_server_safe_malloc(size_t size, char** aloc_buf) {
    ovsa_status_t ret = OVSA_OK;

    *aloc_buf = (char*)malloc(size * sizeof(char));
    if (*aloc_buf == NULL) {
        ret = OVSA_MEMORY_ALLOC_FAIL;
        OVSA_DBG(DBG_E, "Error: Buffer allocation failed with code %d\n", ret);
        goto out;
    }
    memset_s(*aloc_buf, (size) * sizeof(char), 0);
out:
    return ret;
}

void ovsa_server_safe_free(char** ptr) {
    if (*ptr != NULL) {
        free(*ptr);
        *ptr = NULL;
    }

    return;
}

void ovsa_server_safe_free_url_list(ovsa_license_serv_url_list_t** lhead) {
    ovsa_license_serv_url_list_t* head = NULL;
    ovsa_license_serv_url_list_t* cur  = NULL;
    head                               = *lhead;
    while (head != NULL) {
        cur = head->next;
        ovsa_server_safe_free((char**)&head);
        head = cur;
    }
    *lhead = NULL;
}

void ovsa_server_safe_free_tcb_list(ovsa_tcb_sig_list_t** listhead) {
    ovsa_tcb_sig_list_t* head = NULL;
    ovsa_tcb_sig_list_t* cur  = NULL;
    head                      = *listhead;
    while (head != NULL) {
        cur = head->next;
        ovsa_server_safe_free(&head->tcb_signature);
        ovsa_server_safe_free((char**)&head);
        head = cur;
    }
    *listhead = NULL;
}

void ovsa_hexdump_mem(const void* data, size_t size) {
    uint8_t* ptr = (uint8_t*)data;
    for (size_t i = 0; i < size; i++) OVSA_DBG(DBG_D, "%02x", ptr[i]);
}

ovsa_status_t ovsa_append_payload_len_to_blob(const char* input_buf, char** json_payload) {
    ovsa_status_t ret         = OVSA_OK;
    uint64_t json_payload_len = 0;
    unsigned char payload_len[PAYLOAD_LENGTH + 1];
    size_t size = 0;

    memset_s(payload_len, sizeof(payload_len), 0);
    ret = ovsa_server_get_string_length(input_buf, &json_payload_len);
    if (ret < OVSA_OK) {
        OVSA_DBG(DBG_E, "Error: Could not get length of input_buf string %d\n", ret);
        return ret;
    }
    snprintf(payload_len, (PAYLOAD_LENGTH + 1), "%08ld", json_payload_len);
    size = strnlen_s(payload_len, RSIZE_MAX_STR) + 1;
    strcpy_s(*json_payload, size, payload_len);
    strcat_s(*json_payload, RSIZE_MAX_STR, input_buf);

    return ret;
}

ovsa_status_t ovsa_read_file_content(const char* filename, char** filecontent, size_t* filesize) {
    ovsa_status_t ret = OVSA_OK;
    size_t file_size  = 0;
    FILE* fptr        = NULL;

    OVSA_DBG(DBG_D, "OVSA:Entering %s\n", __func__);

    if (filename == NULL || filesize == NULL) {
        OVSA_DBG(DBG_E, "Error: Invalid parameter while reading Quote info\n");
        ret = OVSA_INVALID_PARAMETER;
        goto out;
    }

    fptr = fopen(filename, "rb");
    if (fptr == NULL) {
        ret = OVSA_FILEOPEN_FAIL;
        OVSA_DBG(DBG_E, "Error: Opening file %s failed with code %d\n", filename, ret);
        goto out;
    }

    file_size = ovsa_server_crypto_get_file_size(fptr);
    if (file_size == 0) {
        OVSA_DBG(DBG_E, "Error: Getting file size for %s failed\n", filename);
        ret = OVSA_FILEIO_FAIL;
        fclose(fptr);
        goto out;
    }

    ret = ovsa_server_safe_malloc((sizeof(char) * file_size), filecontent);
    if ((ret < OVSA_OK) || (*filecontent == NULL)) {
        OVSA_DBG(DBG_E, "Error: PCR quote buffer allocation failed %d\n", ret);
        fclose(fptr);
        goto out;
    }

    if (!fread(*filecontent, 1, file_size - 1, fptr)) {
        OVSA_DBG(DBG_E, "Error: Reading pcr quote failed %d\n", ret);
        ret = OVSA_FILEIO_FAIL;
        fclose(fptr);
        goto out;
    }
    fclose(fptr);
    *filesize = file_size;
out:
    OVSA_DBG(DBG_D, "OVSA:%s Exit\n", __func__);
    return ret;
}
int ovsa_server_crypto_get_file_size(FILE* fp) {
    size_t file_size = 0;
    int ret          = 0;

    if (fp == NULL) {
        BIO_printf(g_bio_err, "Error: Getting file size failed with invalid parameter\n");
        return OVSA_INVALID_PARAMETER;
    }

    if (!(fseek(fp, 0L, SEEK_END) == 0)) {
        BIO_printf(g_bio_err,
                   "Error: Getting file size failed in setting the fp to "
                   "end of the file\n");
        goto end;
    }

    file_size = ftell(fp);
    if (file_size == 0) {
        BIO_printf(g_bio_err,
                   "Error: Getting file size failed in giving the current "
                   "position of the fp\n");
        goto end;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        BIO_printf(g_bio_err,
                   "Error: Getting file size failed in setting the fp to "
                   "beginning of the file\n");
        goto end;
    }

    ret = file_size + NULL_TERMINATOR;
end:
    if (!ret) {
        ERR_print_errors(g_bio_err);
    }
    return ret;
}