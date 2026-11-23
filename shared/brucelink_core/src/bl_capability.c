/*
 * BruceLink shared core — capability registry.
 * SPDX-License-Identifier: MIT
 */
#include "bl_capability.h"

#include <string.h>

const bl_capability_desc_t* bl_caps_find(const bl_capability_registry_t* reg, uint16_t id) {
    if(reg == NULL) return NULL;
    for(size_t i = 0; i < reg->count; i++) {
        if(reg->items[i].id == id) return &reg->items[i];
    }
    return NULL;
}

int bl_caps_serialize(const bl_capability_registry_t* reg, uint8_t* out, size_t cap) {
    if(reg == NULL || out == NULL) return BL_E_ARG;
    if(reg->count > 255) return BL_E_TOO_LARGE;
    size_t w = 0;
    if(cap < 1) return BL_E_OVERFLOW;
    out[w++] = (uint8_t)reg->count;
    for(size_t i = 0; i < reg->count; i++) {
        const bl_capability_desc_t* d = &reg->items[i];
        size_t nlen = d->name ? strlen(d->name) : 0;
        if(nlen > 255) return BL_E_TOO_LARGE;
        if(w + 6 + nlen > cap) return BL_E_OVERFLOW;
        out[w++] = (uint8_t)(d->id & 0xFF);
        out[w++] = (uint8_t)(d->id >> 8);
        out[w++] = d->op_class;
        out[w++] = d->flags;
        out[w++] = d->min_proto;
        out[w++] = (uint8_t)nlen;
        if(nlen) memcpy(out + w, d->name, nlen);
        w += nlen;
    }
    return (int)w;
}

int bl_caps_validate(const uint8_t* buf, size_t len) {
    if(buf == NULL) return BL_E_ARG;
    if(len < 1) return BL_E_SHORT;
    uint8_t count = buf[0];
    size_t off = 1;
    for(uint8_t i = 0; i < count; i++) {
        if(off + 6 > len) return BL_E_FORMAT;
        uint8_t nlen = buf[off + 5];
        off += 6;
        if(off + nlen > len) return BL_E_FORMAT;
        off += nlen;
    }
    if(off != len) return BL_E_FORMAT;
    return (int)count;
}
