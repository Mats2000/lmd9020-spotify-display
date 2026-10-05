#pragma once

// Root CAs for Spotify's hosts as BearSSL trust anchors (tools/make_trust_anchors.py).

#include "bearssl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const br_x509_trust_anchor TRUST_ANCHORS[];
extern const size_t TRUST_ANCHORS_NUM;

#ifdef __cplusplus
}
#endif
