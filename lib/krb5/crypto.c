/*
 * Copyright (c) 1997 - 2008 Kungliga Tekniska Högskolan
 * (Royal Institute of Technology, Stockholm, Sweden).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "krb5_locl.h"

struct _krb5_key_usage {
    unsigned usage;
    struct _krb5_key_data key;
};


#ifndef HEIMDAL_SMALLER
#define DES3_OLD_ENCTYPE 1
#endif

static krb5_error_code _get_derived_key(krb5_context, krb5_crypto,
					unsigned, struct _krb5_key_data**);
static struct _krb5_key_data *_new_derived_key(krb5_crypto crypto, unsigned usage);

static void free_key_schedule(krb5_context,
			      struct _krb5_key_data *,
			      struct _krb5_encryption_type *);

/* 
 * Converts etype to a user readable string and sets as a side effect
 * the krb5_error_message containing this string. Returns
 * KRB5_PROG_ETYPE_NOSUPP in not the conversion of the etype failed in
 * which case the error code of the etype convesion is returned.
 */

static krb5_error_code
unsupported_enctype(krb5_context context, krb5_enctype etype)
{
    krb5_error_code ret;
    char *name;

    ret = krb5_enctype_to_string(context, etype, &name);
    if (ret)
	return ret;

    krb5_set_error_message(context, KRB5_PROG_ETYPE_NOSUPP,
			   N_("Encryption type %s not supported", ""),
			   name);
    free(name);
    return KRB5_PROG_ETYPE_NOSUPP;
}

/*
 *
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_keysize(krb5_context context,
		     krb5_enctype type,
		     size_t *keysize)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(type);
    if(et == NULL) {
        return unsupported_enctype (context, type);
    }
    *keysize = et->keytype->size;
    return 0;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_keybits(krb5_context context,
		     krb5_enctype type,
		     size_t *keybits)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(type);
    if(et == NULL) {
        return unsupported_enctype (context, type);
    }
    *keybits = et->keytype->bits;
    return 0;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_generate_random_keyblock(krb5_context context,
			      krb5_enctype type,
			      krb5_keyblock *key)
{
    krb5_error_code ret;
    struct _krb5_encryption_type *et = _krb5_find_enctype(type);
    if(et == NULL) {
        return unsupported_enctype (context, type);
    }
    ret = krb5_data_alloc(&key->keyvalue, et->keytype->size);
    if(ret)
	return ret;
    key->keytype = type;
    if(et->keytype->random_key)
	(*et->keytype->random_key)(context, key);
    else
	krb5_generate_random_block(key->keyvalue.data,
				   key->keyvalue.length);
    return 0;
}

static krb5_error_code
_key_schedule(krb5_context context,
	      struct _krb5_key_data *key)
{
    krb5_error_code ret;
    struct _krb5_encryption_type *et;
    struct _krb5_key_type *kt;

    if (key->schedule != NULL)
	return 0;

    et = _krb5_find_enctype(key->key->keytype);

    if (et == NULL) {
        return unsupported_enctype (context,
                               key->key->keytype);
    }

    kt = et->keytype;

    if(kt->schedule == NULL)
	return 0;
    ALLOC(key->schedule, 1);
    if (key->schedule == NULL)
	return krb5_enomem(context);
    ret = krb5_data_alloc(key->schedule, kt->schedule_size);
    if(ret) {
	free(key->schedule);
	key->schedule = NULL;
	return ret;
    }
    (*kt->schedule)(context, kt, key);
    return 0;
}

/************************************************************
 *                                                          *
 ************************************************************/

static krb5_error_code
EVP_unkeyed_checksum(krb5_context context,
		     krb5_crypto crypto,
		     struct _krb5_key_data *key,
		     unsigned usage,
		     const struct krb5_crypto_iov *iov,
		     int niov,
		     Checksum *C,
		     const EVP_MD *md)
{
    if (_krb5_evp_digest_iov(crypto,
			     iov, niov,
			     C->checksum.data, NULL,
			     md, NULL) != 1)
	krb5_abortx(context, "unkeyed checksum failed");

    return 0;
}

#define EVP_SHA_CHECKSUM(name)				    \
							    \
    static krb5_error_code				    \
    SHA ## name ##_checksum(krb5_context context,	    \
		       krb5_crypto crypto,		    \
		       struct _krb5_key_data *key,	    \
		       unsigned usage,			    \
		       const struct krb5_crypto_iov *iov,   \
		       int niov,			    \
		       Checksum *C)			    \
    {							    \
	return EVP_unkeyed_checksum(context, crypto, key,   \
				    usage, iov, niov,	    \
				    C, EVP_sha##name());    \
    }

EVP_SHA_CHECKSUM(1)
EVP_SHA_CHECKSUM(256)
EVP_SHA_CHECKSUM(384)
EVP_SHA_CHECKSUM(512)

/* HMAC according to RFC2104 */
KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
_krb5_internal_hmac_iov(krb5_context context,
			krb5_crypto crypto,
			struct _krb5_checksum_type *cm,
			unsigned usage,
			const struct krb5_crypto_iov *iov,
			int niov,
			struct _krb5_key_data *keyblock,
			Checksum *result)
{
    unsigned char *ipad, *opad;
    unsigned char *key;
    struct krb5_crypto_iov *working;
    size_t key_len;
    size_t i;

    ipad = malloc(cm->blocksize);
    if (ipad == NULL)
	return ENOMEM;

    opad = malloc(cm->blocksize + cm->checksumsize);
    if (opad == NULL) {
	free(ipad);
	return ENOMEM;
    }

    working = calloc(niov + 1, sizeof(struct krb5_crypto_iov));
    if (working == NULL) {
	free(ipad);
	free(opad);
	return ENOMEM;
    }

    memset(ipad, 0x36, cm->blocksize);
    memset(opad, 0x5c, cm->blocksize);

    if(keyblock->key->keyvalue.length > cm->blocksize){
	working[0].data = keyblock->key->keyvalue;
	working[0].flags = KRB5_CRYPTO_TYPE_DATA;
	(*cm->checksum)(context,
			crypto,
			keyblock,
			usage,
			working,
			1,
			result);
	key = result->checksum.data;
	key_len = result->checksum.length;
    } else {
	key = keyblock->key->keyvalue.data;
	key_len = keyblock->key->keyvalue.length;
    }
    for(i = 0; i < key_len; i++){
	ipad[i] ^= key[i];
	opad[i] ^= key[i];
    }

    working[0].data.data = ipad;
    working[0].data.length = cm->blocksize;
    working[0].flags = KRB5_CRYPTO_TYPE_DATA;
    for (i = 0; i < niov; i++)
	working[i + 1] = iov[i];

    (*cm->checksum)(context, crypto, keyblock, usage, working, niov + 1, result);
    memcpy(opad + cm->blocksize, result->checksum.data,
	   result->checksum.length);

    working[0].data.data = opad;
    working[0].data.length = cm->blocksize + cm->checksumsize;
    working[0].flags = KRB5_CRYPTO_TYPE_DATA;
    (*cm->checksum)(context, crypto, keyblock, usage, working, 1, result);
    memset(ipad, 0, cm->blocksize);
    free(ipad);
    memset(opad, 0, cm->blocksize + cm->checksumsize);
    free(opad);
    free(working);

    return 0;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
_krb5_internal_hmac(krb5_context context,
		    krb5_crypto crypto,
		    struct _krb5_checksum_type *cm,
		    const void *data,
		    size_t len,
		    unsigned usage,
		    struct _krb5_key_data *keyblock,
		    Checksum *result)
{
    struct krb5_crypto_iov iov[1];

    iov[0].data.data = (void *) data;
    iov[0].data.length = len;
    iov[0].flags = KRB5_CRYPTO_TYPE_DATA;
    return _krb5_internal_hmac_iov(context, crypto, cm, usage, iov, 1,
				   keyblock, result);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_hmac(krb5_context context,
	  krb5_cksumtype cktype,
	  const void *data,
	  size_t len,
	  unsigned usage,
	  krb5_keyblock *key,
	  Checksum *result)
{
    struct _krb5_checksum_type *c = _krb5_find_checksum(cktype);
    struct _krb5_key_data kd;

    krb5_error_code ret;

    if (c == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				cktype);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    kd.key = key;
    kd.schedule = NULL;

    ret = _krb5_internal_hmac(context, NULL, c, data, len, usage, &kd, result);

    if (kd.schedule)
	krb5_free_data(context, kd.schedule);

    return ret;
}

krb5_error_code
_krb5_SP_HMAC_SHA1_checksum(krb5_context context,
			    krb5_crypto crypto,
			    struct _krb5_key_data *key,
			    unsigned usage,
			    const struct krb5_crypto_iov *iov,
			    int niov,
			    Checksum *result)
{
    krb5_error_code ret;
    unsigned char hmac[EVP_MAX_MD_SIZE];
    unsigned int hmaclen = sizeof(hmac);

    ret = _krb5_evp_hmac_iov(context, crypto, key, iov, niov, hmac, &hmaclen,
                             EVP_sha1(), NULL);
    if (ret)
        return ret;

    heim_assert(result->checksum.length <= hmaclen,
                "SHA1 checksum too short");
    memcpy(result->checksum.data, hmac, result->checksum.length);

    return 0;
}

krb5_error_code
_krb5_SP_HMAC_SHA1_verify(krb5_context context,
                          krb5_crypto crypto,
                          struct _krb5_key_data *key,
                          unsigned usage,
                          const struct krb5_crypto_iov *iov,
                          int niov,
                          Checksum *verify)
{
    krb5_error_code ret;
    unsigned char hmac[EVP_MAX_MD_SIZE];
    unsigned int hmaclen = sizeof(hmac);
    krb5_data data;

    ret = _krb5_evp_hmac_iov(context, crypto, key, iov, niov, hmac, &hmaclen,
                             EVP_sha1(), NULL);
    if (ret)
        return ret;

    data.data = hmac;
    data.length = min(hmaclen, verify->checksum.length);

    if(krb5_data_ct_cmp(&data, &verify->checksum) != 0)
        return KRB5KRB_AP_ERR_BAD_INTEGRITY;

    return 0;
}

#define SHA_CHECKSUM(name, blocksize, outputsize)	    \
    struct _krb5_checksum_type _krb5_checksum_sha##name = { \
	CKSUMTYPE_SHA##name,				    \
	"sha" #name,					    \
	blocksize,					    \
	outputsize,					    \
	F_CPROOF,					    \
	SHA##name##_checksum,				    \
	NULL						    \
    };

SHA_CHECKSUM(1,   64,  20);
SHA_CHECKSUM(256, 64,  32);
SHA_CHECKSUM(384, 128, 48);
SHA_CHECKSUM(512, 128, 64);

KRB5_LIB_FUNCTION struct _krb5_checksum_type * KRB5_LIB_CALL
_krb5_find_checksum(krb5_cksumtype type)
{
    int i;
    for(i = 0; i < _krb5_num_checksums; i++)
	if(_krb5_checksum_types[i]->type == type)
	    return _krb5_checksum_types[i];
    return NULL;
}

static krb5_error_code
get_checksum_key(krb5_context context,
		 krb5_crypto crypto,
		 unsigned usage,  /* not krb5_key_usage */
		 struct _krb5_checksum_type *ct,
		 struct _krb5_key_data **key)
{
    krb5_error_code ret = 0;
    struct _krb5_checksum_type *kct = NULL;

    if (crypto == NULL) {
	krb5_set_error_message(context, KRB5_BAD_ENCTYPE,
			       N_("Checksum type %s is keyed but no "
			          "crypto context (key) was passed in", ""),
			       ct->name);
	return KRB5_BAD_ENCTYPE;
    }
    kct = crypto->et->keyed_checksum;
    if (kct == NULL || kct->type != ct->type) {
	krb5_set_error_message(context, KRB5_BAD_ENCTYPE,
			       N_("Checksum type %s is keyed, but "
			          "the key type %s passed didnt have that checksum "
			          "type as the keyed type", ""),
			       ct->name, crypto->et->name);
	return KRB5_BAD_ENCTYPE;
    }

    if(ct->flags & F_DERIVED)
	ret = _get_derived_key(context, crypto, usage, key);
    else if(ct->flags & F_VARIANT) {
	size_t i;

	*key = _new_derived_key(crypto, 0xff/* KRB5_KU_RFC1510_VARIANT */);
	if (*key == NULL)
	    return krb5_enomem(context);
	ret = krb5_copy_keyblock(context, crypto->key.key, &(*key)->key);
	if(ret)
	    return ret;
	for(i = 0; i < (*key)->key->keyvalue.length; i++)
	    ((unsigned char*)(*key)->key->keyvalue.data)[i] ^= 0xF0;
    } else {
	*key = &crypto->key;
    }
    if(ret == 0)
	ret = _key_schedule(context, *key);
    return ret;
}

static krb5_error_code
create_checksum_iov(krb5_context context,
		    struct _krb5_checksum_type *ct,
		    krb5_crypto crypto,
		    unsigned usage,
		    struct krb5_crypto_iov *iov,
		    int niov,
		    krb5_flags flags,
		    Checksum *result)
{
    krb5_error_code ret;
    struct _krb5_key_data *dkey;

    if (ct->flags & F_DISABLED) {
	krb5_clear_error_message (context);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    if (ct->flags & F_KEYED) {
	ret = get_checksum_key(context, crypto, usage, ct, &dkey);
	if (ret)
	    return ret;
    } else if ((flags & KRB5_CRYPTO_FLAG_ALLOW_UNKEYED_CHECKSUM) == 0) {
	return EINVAL;
    } else
	dkey = NULL;

    result->cksumtype = ct->type;

    return (*ct->checksum)(context, crypto, dkey, usage, iov, niov, result);
}

static krb5_error_code
create_checksum (krb5_context context,
		 struct _krb5_checksum_type *ct,
		 krb5_crypto crypto,
		 unsigned usage,
		 void *data,
		 size_t len,
		 krb5_flags flags,
		 Checksum *result)
{
    int ret;
    struct krb5_crypto_iov iov[1];

    ret = krb5_data_alloc(&result->checksum, ct->checksumsize);
    if (ret)
	return ret;

    iov[0].data.data = data;
    iov[0].data.length = len;
    iov[0].flags = KRB5_CRYPTO_TYPE_DATA;

    return create_checksum_iov(context, ct, crypto, usage, iov, 1, flags, result);
}

static int
arcfour_checksum_p(struct _krb5_checksum_type *ct, krb5_crypto crypto)
{
    return (ct->type == CKSUMTYPE_HMAC_MD5) &&
	(crypto->key.key->keytype == KEYTYPE_ARCFOUR);
}

static inline krb5_flags
crypto_flags(krb5_crypto crypto)
{
    /* If caller didn't specify a key, unkeyed checksums are the only option */
    if (crypto == NULL)
	return KRB5_CRYPTO_FLAG_ALLOW_UNKEYED_CHECKSUM;
    else
	return crypto->flags;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_create_checksum(krb5_context context,
		     krb5_crypto crypto,
		     krb5_key_usage usage,
		     int type,
		     void *data,
		     size_t len,
		     Checksum *result)
{
    struct _krb5_checksum_type *ct = NULL;
    unsigned keyusage;

    /* type 0 -> pick from crypto */
    if (type) {
	ct = _krb5_find_checksum(type);
    } else if (crypto) {
	ct = crypto->et->keyed_checksum;
	if (ct == NULL)
	    ct = crypto->et->checksum;
    }

    if(ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				type);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    if (arcfour_checksum_p(ct, crypto)) {
	keyusage = usage;
	_krb5_usage2arcfour(context, &keyusage);
    } else
	keyusage = CHECKSUM_USAGE(usage);

    return create_checksum(context, ct, crypto, keyusage, data, len,
			   crypto_flags(crypto), result);
}

static krb5_error_code
verify_checksum_iov(krb5_context context,
		    krb5_crypto crypto,
		    unsigned usage, /* not krb5_key_usage */
		    struct krb5_crypto_iov *iov,
		    int niov,
		    krb5_flags flags,
		    Checksum *cksum)
{
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    Checksum c;
    struct _krb5_checksum_type *ct;

    ct = _krb5_find_checksum(cksum->cksumtype);
    if (ct == NULL || (ct->flags & F_DISABLED)) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				cksum->cksumtype);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    if(ct->checksumsize != cksum->checksum.length) {
	krb5_clear_error_message (context);
	krb5_set_error_message(context, KRB5KRB_AP_ERR_BAD_INTEGRITY,
			       N_("Decrypt integrity check failed for checksum type %s, "
				  "length was %u, expected %u", ""),
			       ct->name, (unsigned)cksum->checksum.length,
			       (unsigned)ct->checksumsize);

	return KRB5KRB_AP_ERR_BAD_INTEGRITY; /* XXX */
    }
    if (ct->flags & F_KEYED) {
	ret = get_checksum_key(context, crypto, usage, ct, &dkey);
	if (ret)
	    return ret;
    } else if ((flags & KRB5_CRYPTO_FLAG_ALLOW_UNKEYED_CHECKSUM) == 0) {
	krb5_clear_error_message (context);
	krb5_set_error_message(context, KRB5KRB_AP_ERR_BAD_INTEGRITY,
			       N_("Unkeyed checksum type %s provided where keyed "
				  "checksum was expected", ""), ct->name);

	return KRB5KRB_AP_ERR_INAPP_CKSUM;
    } else
	dkey = NULL;

    /*
     * If checksum have a verify function, lets use that instead of
     * calling ->checksum and then compare result.
     */

    if(ct->verify) {
	ret = (*ct->verify)(context, crypto, dkey, usage, iov, niov, cksum);
	if (ret)
	    krb5_set_error_message(context, ret,
				   N_("Decrypt integrity check failed for checksum "
				      "type %s, key type %s", ""),
				   ct->name, (crypto != NULL)? crypto->et->name : "(none)");
	return ret;
    }

    ret = krb5_data_alloc (&c.checksum, ct->checksumsize);
    if (ret)
	return ret;

    ret = (*ct->checksum)(context, crypto, dkey, usage, iov, niov, &c);
    if (ret) {
	krb5_data_free(&c.checksum);
	return ret;
    }

    if(krb5_data_ct_cmp(&c.checksum, &cksum->checksum) != 0) {
	ret = KRB5KRB_AP_ERR_BAD_INTEGRITY;
	krb5_set_error_message(context, ret,
			       N_("Decrypt integrity check failed for checksum "
				  "type %s, key type %s", ""),
			       ct->name, crypto ? crypto->et->name : "(unkeyed)");
    } else {
	ret = 0;
    }
    krb5_data_free (&c.checksum);
    return ret;
}

static krb5_error_code
verify_checksum(krb5_context context,
		krb5_crypto crypto,
		unsigned usage, /* not krb5_key_usage */
		void *data,
		size_t len,
		krb5_flags flags,
		Checksum *cksum)
{
    struct krb5_crypto_iov iov[1];

    iov[0].data.data = data;
    iov[0].data.length = len;
    iov[0].flags = KRB5_CRYPTO_TYPE_DATA;

    return verify_checksum_iov(context, crypto, usage, iov, 1, flags, cksum);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_verify_checksum(krb5_context context,
		     krb5_crypto crypto,
		     krb5_key_usage usage,
		     void *data,
		     size_t len,
		     Checksum *cksum)
{
    struct _krb5_checksum_type *ct;
    unsigned keyusage;

    ct = _krb5_find_checksum(cksum->cksumtype);
    if(ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				cksum->cksumtype);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    if (arcfour_checksum_p(ct, crypto)) {
	keyusage = usage;
	_krb5_usage2arcfour(context, &keyusage);
    } else
	keyusage = CHECKSUM_USAGE(usage);

    return verify_checksum(context, crypto, keyusage,
			   data, len, crypto_flags(crypto), cksum);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_get_checksum_type(krb5_context context,
                              krb5_crypto crypto,
			      krb5_cksumtype *type)
{
    struct _krb5_checksum_type *ct = NULL;

    if (crypto != NULL) {
        ct = crypto->et->keyed_checksum;
        if (ct == NULL)
            ct = crypto->et->checksum;
    }

    if (ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type not found", ""));
        return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    *type = ct->type;

    return 0;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_checksumsize(krb5_context context,
		  krb5_cksumtype type,
		  size_t *size)
{
    struct _krb5_checksum_type *ct = _krb5_find_checksum(type);
    if(ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				type);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    *size = ct->checksumsize;
    return 0;
}

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_checksum_is_keyed(krb5_context context,
		       krb5_cksumtype type)
{
    struct _krb5_checksum_type *ct = _krb5_find_checksum(type);
    if(ct == NULL) {
	if (context)
	    krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				    N_("checksum type %d not supported", ""),
				    type);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    return ct->flags & F_KEYED;
}

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_checksum_is_collision_proof(krb5_context context,
				 krb5_cksumtype type)
{
    struct _krb5_checksum_type *ct = _krb5_find_checksum(type);
    if(ct == NULL) {
	if (context)
	    krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				    N_("checksum type %d not supported", ""),
				    type);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    return ct->flags & F_CPROOF;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_checksum_disable(krb5_context context,
		      krb5_cksumtype type)
{
    struct _krb5_checksum_type *ct = _krb5_find_checksum(type);
    if(ct == NULL) {
	if (context)
	    krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				    N_("checksum type %d not supported", ""),
				    type);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    ct->flags |= F_DISABLED;
    return 0;
}

/************************************************************
 *                                                          *
 ************************************************************/

KRB5_LIB_FUNCTION struct _krb5_encryption_type * KRB5_LIB_CALL
_krb5_find_enctype(krb5_enctype type)
{
    int i;
    for(i = 0; i < _krb5_num_etypes; i++)
	if(_krb5_etypes[i]->type == type)
	    return _krb5_etypes[i];
    return NULL;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_to_string(krb5_context context,
		       krb5_enctype etype,
		       char **string)
{
    struct _krb5_encryption_type *e;
    e = _krb5_find_enctype(etype);
    if(e == NULL) {
	krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
				N_("encryption type %d not supported", ""),
				etype);
	*string = NULL;
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    *string = strdup(e->name);
    if (*string == NULL)
	return krb5_enomem(context);
    return 0;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_string_to_enctype(krb5_context context,
		       const char *string,
		       krb5_enctype *etype)
{
    int i;
    for(i = 0; i < _krb5_num_etypes; i++) {
	if(strcasecmp(_krb5_etypes[i]->name, string) == 0){
	    *etype = _krb5_etypes[i]->type;
	    return 0;
	}
	if(_krb5_etypes[i]->alias != NULL &&
	   strcasecmp(_krb5_etypes[i]->alias, string) == 0){
	    *etype = _krb5_etypes[i]->type;
	    return 0;
	}
    }
    krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
			    N_("encryption type %s not supported", ""),
			    string);
    return KRB5_PROG_ETYPE_NOSUPP;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_to_keytype(krb5_context context,
			krb5_enctype etype,
			krb5_keytype *keytype)
{
    struct _krb5_encryption_type *e = _krb5_find_enctype(etype);
    if(e == NULL) {
        return unsupported_enctype (context, etype);
    }
    *keytype = e->keytype->type; /* XXX */
    return 0;
}

/**
 * Check if a enctype is valid, return 0 if it is.
 *
 * @param context Kerberos context
 * @param etype enctype to check if its valid or not
 *
 * @return Return an error code for an failure or 0 on success (enctype valid).
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_valid(krb5_context context,
		   krb5_enctype etype)
{
    struct _krb5_encryption_type *e = _krb5_find_enctype(etype);
    if(e && (e->flags & F_DISABLED) == 0)
	return 0;
    if (context == NULL)
	return KRB5_PROG_ETYPE_NOSUPP;
    if(e == NULL) {
        return unsupported_enctype (context, etype);
    }
    /* Must be (e->flags & F_DISABLED) */
    krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
			    N_("encryption type %s is disabled", ""),
			    e->name);
    return KRB5_PROG_ETYPE_NOSUPP;
}

/**
 * Return the coresponding encryption type for a checksum type.
 *
 * @param context Kerberos context
 * @param ctype The checksum type to get the result enctype for
 * @param etype The returned encryption, when the matching etype is
 * not found, etype is set to ETYPE_NULL.
 *
 * @return Return an error code for an failure or 0 on success.
 * @ingroup krb5_crypto
 */


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_cksumtype_to_enctype(krb5_context context,
			  krb5_cksumtype ctype,
			  krb5_enctype *etype)
{
    int i;

    *etype = ETYPE_NULL;

    for(i = 0; i < _krb5_num_etypes; i++) {
	if(_krb5_etypes[i]->keyed_checksum &&
	   _krb5_etypes[i]->keyed_checksum->type == ctype)
	    {
		*etype = _krb5_etypes[i]->type;
		return 0;
	    }
    }

    krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
			    N_("checksum type %d not supported", ""),
			    (int)ctype);
    return KRB5_PROG_SUMTYPE_NOSUPP;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_cksumtype_valid(krb5_context context,
		     krb5_cksumtype ctype)
{
    struct _krb5_checksum_type *c = _krb5_find_checksum(ctype);
    if (c == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				ctype);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    if (c->flags & F_DISABLED) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %s is disabled", ""),
				c->name);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }
    return 0;
}

static krb5_boolean
derived_crypto(krb5_context context,
	       krb5_crypto crypto)
{
    return (crypto->et->flags & F_DERIVED) != 0;
}

#define CHECKSUMSIZE(C) ((C)->checksumsize)
#define CHECKSUMTYPE(C) ((C)->type)

static krb5_error_code
encrypt_internal_derived(krb5_context context,
			 krb5_crypto crypto,
			 unsigned usage,
			 const void *data,
			 size_t len,
			 krb5_data *result,
			 void *ivec)
{
    size_t sz, block_sz, checksum_sz, total_sz;
    Checksum cksum;
    unsigned char *p, *q;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    const struct _krb5_encryption_type *et = crypto->et;

    checksum_sz = CHECKSUMSIZE(et->keyed_checksum);

    sz = et->confoundersize + len;
    block_sz = (sz + et->padsize - 1) &~ (et->padsize - 1); /* pad */
    total_sz = block_sz + checksum_sz;
    p = calloc(1, total_sz);
    if (p == NULL)
	return krb5_enomem(context);

    q = p;
    krb5_generate_random_block(q, et->confoundersize); /* XXX */
    q += et->confoundersize;
    memcpy(q, data, len);

    ret = create_checksum(context,
			  et->keyed_checksum,
			  crypto,
			  INTEGRITY_USAGE(usage),
			  p,
			  block_sz,
			  0,
			  &cksum);
    if(ret == 0 && cksum.checksum.length != checksum_sz) {
	free_Checksum (&cksum);
	krb5_clear_error_message (context);
	ret = KRB5_CRYPTO_INTERNAL;
    }
    if(ret)
	goto fail;
    memcpy(p + block_sz, cksum.checksum.data, cksum.checksum.length);
    free_Checksum (&cksum);
    ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
    if(ret)
	goto fail;
    ret = _key_schedule(context, dkey);
    if(ret)
	goto fail;
    ret = (*et->encrypt)(context, dkey, p, block_sz, 1, usage, ivec);
    if (ret)
	goto fail;
    result->data = p;
    result->length = total_sz;
    return 0;
 fail:
    memset(p, 0, total_sz);
    free(p);
    return ret;
}

static krb5_error_code
encrypt_internal_enc_then_cksum(krb5_context context,
				krb5_crypto crypto,
				unsigned usage,
				const void *data,
				size_t len,
				krb5_data *result,
				void *ivec)
{
    size_t sz, block_sz, checksum_sz, total_sz;
    Checksum cksum;
    unsigned char *p, *q, *ivc = NULL;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    const struct _krb5_encryption_type *et = crypto->et;

    checksum_sz = CHECKSUMSIZE(et->keyed_checksum);

    sz = et->confoundersize + len;
    block_sz = (sz + et->padsize - 1) &~ (et->padsize - 1); /* pad */
    total_sz = block_sz + checksum_sz;
    p = calloc(1, total_sz);
    if (p == NULL)
	return krb5_enomem(context);

    q = p;
    krb5_generate_random_block(q, et->confoundersize); /* XXX */
    q += et->confoundersize;
    memcpy(q, data, len);

    ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
    if(ret)
	goto fail;
    ret = _key_schedule(context, dkey);
    if(ret)
	goto fail;

    /* XXX EVP style update API would avoid needing to allocate here */
    ivc = malloc(et->blocksize + block_sz);
    if (ivc == NULL) {
	ret = krb5_enomem(context);
	goto fail;
    }
    if (ivec)
	memcpy(ivc, ivec, et->blocksize);
    else
	memset(ivc, 0, et->blocksize);

    ret = (*et->encrypt)(context, dkey, p, block_sz, 1, usage, ivec);
    if (ret)
	goto fail;
    memcpy(&ivc[et->blocksize], p, block_sz);

    ret = create_checksum(context,
			  et->keyed_checksum,
			  crypto,
			  INTEGRITY_USAGE(usage),
			  ivc,
			  et->blocksize + block_sz,
			  0,
			  &cksum);
    if(ret == 0 && cksum.checksum.length != checksum_sz) {
	free_Checksum (&cksum);
	krb5_clear_error_message (context);
	ret = KRB5_CRYPTO_INTERNAL;
    }
    if(ret)
	goto fail;
    memcpy(p + block_sz, cksum.checksum.data, cksum.checksum.length);
    free_Checksum (&cksum);
    result->data = p;
    result->length = total_sz;
    free(ivc);
    return 0;
 fail:
    memset_s(p, total_sz, 0, total_sz);
    free(p);
    free(ivc);
    return ret;
}

static krb5_error_code
encrypt_internal(krb5_context context,
		 krb5_crypto crypto,
		 const void *data,
		 size_t len,
		 krb5_data *result,
		 void *ivec)
{
    size_t sz, block_sz, checksum_sz;
    Checksum cksum;
    unsigned char *p, *q;
    krb5_error_code ret;
    const struct _krb5_encryption_type *et = crypto->et;

    checksum_sz = CHECKSUMSIZE(et->checksum);

    sz = et->confoundersize + checksum_sz + len;
    block_sz = (sz + et->padsize - 1) &~ (et->padsize - 1); /* pad */
    p = calloc(1, block_sz);
    if (p == NULL)
	return krb5_enomem(context);

    q = p;
    krb5_generate_random_block(q, et->confoundersize); /* XXX */
    q += et->confoundersize;
    memset(q, 0, checksum_sz);
    q += checksum_sz;
    memcpy(q, data, len);

    ret = create_checksum(context,
			  et->checksum,
			  crypto,
			  0,
			  p,
			  block_sz,
			  KRB5_CRYPTO_FLAG_ALLOW_UNKEYED_CHECKSUM,
			  &cksum);
    if(ret == 0 && cksum.checksum.length != checksum_sz) {
	krb5_clear_error_message (context);
	free_Checksum(&cksum);
	ret = KRB5_CRYPTO_INTERNAL;
    }
    if(ret)
	goto fail;
    memcpy(p + et->confoundersize, cksum.checksum.data, cksum.checksum.length);
    free_Checksum(&cksum);
    ret = _key_schedule(context, &crypto->key);
    if(ret)
	goto fail;
    ret = (*et->encrypt)(context, &crypto->key, p, block_sz, 1, 0, ivec);
    if (ret) {
	memset(p, 0, block_sz);
	free(p);
	return ret;
    }
    result->data = p;
    result->length = block_sz;
    return 0;
 fail:
    memset(p, 0, block_sz);
    free(p);
    return ret;
}

static krb5_error_code
encrypt_internal_special(krb5_context context,
			 krb5_crypto crypto,
			 int usage,
			 const void *data,
			 size_t len,
			 krb5_data *result,
			 void *ivec)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t cksum_sz = CHECKSUMSIZE(et->checksum);
    size_t sz = len + cksum_sz + et->confoundersize;
    char *tmp, *p;
    krb5_error_code ret;

    tmp = malloc (sz);
    if (tmp == NULL)
	return krb5_enomem(context);
    p = tmp;
    memset (p, 0, cksum_sz);
    p += cksum_sz;
    krb5_generate_random_block(p, et->confoundersize);
    p += et->confoundersize;
    memcpy (p, data, len);
    ret = (*et->encrypt)(context, &crypto->key, tmp, sz, TRUE, usage, ivec);
    if (ret) {
	memset(tmp, 0, sz);
	free(tmp);
	return ret;
    }
    result->data   = tmp;
    result->length = sz;
    return 0;
}

static krb5_error_code
decrypt_internal_derived(krb5_context context,
			 krb5_crypto crypto,
			 unsigned usage,
			 void *data,
			 size_t len,
			 krb5_data *result,
			 void *ivec)
{
    size_t checksum_sz;
    Checksum cksum;
    unsigned char *p;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    struct _krb5_encryption_type *et = crypto->et;
    unsigned long l;

    checksum_sz = CHECKSUMSIZE(et->keyed_checksum);
    if (len < checksum_sz + et->confoundersize) {
	krb5_set_error_message(context, KRB5_BAD_MSIZE,
			       N_("Encrypted data shorter then "
				  "checksum + confunder", ""));
	return KRB5_BAD_MSIZE;
    }

    if (((len - checksum_sz) % et->padsize) != 0) {
	krb5_clear_error_message(context);
	return KRB5_BAD_MSIZE;
    }

    p = malloc(len);
    if (len != 0 && p == NULL)
	return krb5_enomem(context);
    memcpy(p, data, len);

    len -= checksum_sz;

    ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
    if(ret) {
	free(p);
	return ret;
    }
    ret = _key_schedule(context, dkey);
    if(ret) {
	free(p);
	return ret;
    }
    ret = (*et->encrypt)(context, dkey, p, len, 0, usage, ivec);
    if (ret) {
	free(p);
	return ret;
    }

    cksum.checksum.data   = p + len;
    cksum.checksum.length = checksum_sz;
    cksum.cksumtype       = CHECKSUMTYPE(et->keyed_checksum);

    ret = verify_checksum(context,
			  crypto,
			  INTEGRITY_USAGE(usage),
			  p,
			  len,
			  0,
			  &cksum);
    if(ret) {
	free(p);
	return ret;
    }
    l = len - et->confoundersize;
    memmove(p, p + et->confoundersize, l);
    result->data = p;
    result->length = l;
    return 0;
}

static krb5_error_code
decrypt_internal_enc_then_cksum(krb5_context context,
				krb5_crypto crypto,
				unsigned usage,
				void *data,
				size_t len,
				krb5_data *result,
				void *ivec)
{
    size_t checksum_sz;
    Checksum cksum;
    unsigned char *p;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    struct _krb5_encryption_type *et = crypto->et;
    unsigned long l;

    checksum_sz = CHECKSUMSIZE(et->keyed_checksum);
    if (len < checksum_sz + et->confoundersize) {
	krb5_set_error_message(context, KRB5_BAD_MSIZE,
			       N_("Encrypted data shorter then "
				  "checksum + confunder", ""));
	return KRB5_BAD_MSIZE;
    }

    if (((len - checksum_sz) % et->padsize) != 0) {
	krb5_clear_error_message(context);
	return KRB5_BAD_MSIZE;
    }

    len -= checksum_sz;

    p = malloc(et->blocksize + len);
    if (p == NULL)
	return krb5_enomem(context);

    if (ivec)
	memcpy(p, ivec, et->blocksize);
    else
	memset(p, 0, et->blocksize);
    memcpy(&p[et->blocksize], data, len);

    cksum.checksum.data   = (unsigned char *)data + len;
    cksum.checksum.length = checksum_sz;
    cksum.cksumtype       = CHECKSUMTYPE(et->keyed_checksum);

    ret = verify_checksum(context,
			  crypto,
			  INTEGRITY_USAGE(usage),
			  p,
			  et->blocksize + len,
			  0,
			  &cksum);
    if(ret) {
	free(p);
	return ret;
    }

    ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
    if(ret) {
	free(p);
	return ret;
    }
    ret = _key_schedule(context, dkey);
    if(ret) {
	free(p);
	return ret;
    }
    ret = (*et->encrypt)(context, dkey, &p[et->blocksize], len, 0, usage, ivec);
    if (ret) {
	free(p);
	return ret;
    }

    l = len - et->confoundersize;
    memmove(p, p + et->blocksize + et->confoundersize, l);
    result->data = p;
    result->length = l;
    return 0;
}

static krb5_error_code
decrypt_internal(krb5_context context,
		 krb5_crypto crypto,
		 void *data,
		 size_t len,
		 krb5_data *result,
		 void *ivec)
{
    krb5_error_code ret;
    unsigned char *p;
    Checksum cksum;
    size_t checksum_sz, l;
    struct _krb5_encryption_type *et = crypto->et;

    if ((len % et->padsize) != 0) {
	krb5_clear_error_message(context);
	return KRB5_BAD_MSIZE;
    }
    checksum_sz = CHECKSUMSIZE(et->checksum);
    if (len < checksum_sz + et->confoundersize) {
	krb5_set_error_message(context, KRB5_BAD_MSIZE,
			       N_("Encrypted data shorter then "
				  "checksum + confunder", ""));
	return KRB5_BAD_MSIZE;
    }

    p = malloc(len);
    if (len != 0 && p == NULL)
	return krb5_enomem(context);
    memcpy(p, data, len);

    ret = _key_schedule(context, &crypto->key);
    if(ret) {
	free(p);
	return ret;
    }
    ret = (*et->encrypt)(context, &crypto->key, p, len, 0, 0, ivec);
    if (ret) {
	free(p);
	return ret;
    }
    ret = krb5_data_copy(&cksum.checksum, p + et->confoundersize, checksum_sz);
    if(ret) {
 	free(p);
 	return ret;
    }
    memset(p + et->confoundersize, 0, checksum_sz);
    cksum.cksumtype = CHECKSUMTYPE(et->checksum);
    ret = verify_checksum(context, NULL, 0, p, len,
			  KRB5_CRYPTO_FLAG_ALLOW_UNKEYED_CHECKSUM, &cksum);
    free_Checksum(&cksum);
    if(ret) {
	free(p);
	return ret;
    }
    l = len - et->confoundersize - checksum_sz;
    memmove(p, p + et->confoundersize + checksum_sz, l);
    result->data = p;
    result->length = l;
    return 0;
}

static krb5_error_code
decrypt_internal_special(krb5_context context,
			 krb5_crypto crypto,
			 int usage,
			 void *data,
			 size_t len,
			 krb5_data *result,
			 void *ivec)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t cksum_sz = CHECKSUMSIZE(et->checksum);
    size_t sz = len - cksum_sz - et->confoundersize;
    unsigned char *p;
    krb5_error_code ret;

    if ((len % et->padsize) != 0) {
	krb5_clear_error_message(context);
	return KRB5_BAD_MSIZE;
    }
    if (len < cksum_sz + et->confoundersize) {
	krb5_set_error_message(context, KRB5_BAD_MSIZE,
			       N_("Encrypted data shorter then "
				  "checksum + confunder", ""));
	return KRB5_BAD_MSIZE;
    }

    p = malloc (len);
    if (p == NULL)
	return krb5_enomem(context);
    memcpy(p, data, len);

    ret = (*et->encrypt)(context, &crypto->key, p, len, FALSE, usage, ivec);
    if (ret) {
	free(p);
	return ret;
    }

    memmove (p, p + cksum_sz + et->confoundersize, sz);
    result->data = p;
    result->length = sz;
    return 0;
}

static krb5_crypto_iov *
iov_find(krb5_crypto_iov *data, size_t num_data, unsigned type)
{
    size_t i;
    for (i = 0; i < num_data; i++)
	if (data[i].flags == type)
	    return &data[i];
    return NULL;
}

static size_t
iov_enc_data_len(krb5_crypto_iov *data, int num_data)
{
    size_t i, len;

    for (len = 0, i = 0; i < num_data; i++) {
	if (data[i].flags != KRB5_CRYPTO_TYPE_DATA)
	    continue;
	len += data[i].data.length;
    }

    return len;
}

static size_t
iov_sign_data_len(krb5_crypto_iov *data, int num_data)
{
    size_t i, len;

    for (len = 0, i = 0; i < num_data; i++) {
	/* Can't use should_sign, because we must only count data, not
	 * header/trailer */
	if (data[i].flags == KRB5_CRYPTO_TYPE_DATA ||
	    data[i].flags == KRB5_CRYPTO_TYPE_SIGN_ONLY)
	    len += data[i].data.length;
    }

    return len;
}

static krb5_error_code
iov_coalesce(krb5_context context,
	     krb5_data *prefix,
	     krb5_crypto_iov *data,
	     int num_data,
	     krb5_boolean inc_sign_data,
	     krb5_data *out)
{
    unsigned char *p, *q;
    krb5_crypto_iov *hiv, *piv;
    size_t len;
    unsigned int i;

    hiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_HEADER);

    piv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_PADDING);

    len = 0;
    if (prefix)
	len += prefix->length;
    len += hiv->data.length;
    if (inc_sign_data)
	len += iov_sign_data_len(data, num_data);
    else
	len += iov_enc_data_len(data, num_data);
    if (piv)
	len += piv->data.length;

    p = q = malloc(len);
    if (p == NULL)
	return krb5_enomem(context);

    if (prefix) {
	memcpy(q, prefix->data, prefix->length);
	q += prefix->length;
    }
    memcpy(q, hiv->data.data, hiv->data.length);
    q += hiv->data.length;
    for (i = 0; i < num_data; i++) {
	if (data[i].flags == KRB5_CRYPTO_TYPE_DATA ||
	    (inc_sign_data && data[i].flags == KRB5_CRYPTO_TYPE_SIGN_ONLY)) {
	    memcpy(q, data[i].data.data, data[i].data.length);
	    q += data[i].data.length;
	}
    }
    if (piv)
	memset(q, 0, piv->data.length);

    out->length = len;
    out->data = p;

    return 0;
}

static krb5_error_code
iov_uncoalesce(krb5_context context,
	       krb5_data *enc_data,
	       krb5_crypto_iov *data,
	       int num_data)
{
    unsigned char *q = enc_data->data;
    krb5_crypto_iov *hiv, *piv;
    unsigned int i;

    hiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_HEADER);

    piv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_PADDING);

    memcpy(hiv->data.data, q, hiv->data.length);
    q += hiv->data.length;

    for (i = 0; i < num_data; i++) {
	if (data[i].flags != KRB5_CRYPTO_TYPE_DATA)
	    continue;
	memcpy(data[i].data.data, q, data[i].data.length);
	q += data[i].data.length;
    }
    if (piv)
	memcpy(piv->data.data, q, piv->data.length);

    return 0;
}

static krb5_error_code
iov_pad_validate(const struct _krb5_encryption_type *et,
		 krb5_crypto_iov *data,
		 int num_data,
		 krb5_crypto_iov **ppiv)
{
    krb5_crypto_iov *piv;
    size_t sz, headersz, block_sz, pad_sz, len;

    len = iov_enc_data_len(data, num_data);

    headersz = et->confoundersize;

    sz = headersz + len;
    block_sz = (sz + et->padsize - 1) &~ (et->padsize - 1); /* pad */

    pad_sz = block_sz - sz;

    piv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_PADDING);
    /* its ok to have no TYPE_PADDING if there is no padding */
    if (piv == NULL && pad_sz != 0)
	return KRB5_BAD_MSIZE;
    if (piv) {
	if (piv->data.length < pad_sz)
	    return KRB5_BAD_MSIZE;
	piv->data.length = pad_sz;
	if (pad_sz)
	    memset(piv->data.data, 0, pad_sz);
	else
	    piv = NULL;
    }

    *ppiv = piv;
    return 0;
}

/**
 * Inline encrypt a kerberos message
 *
 * @param context Kerberos context
 * @param crypto Kerberos crypto context
 * @param usage Key usage for this buffer
 * @param data array of buffers to process
 * @param num_data length of array
 * @param ivec initial cbc/cts vector
 *
 * @return Return an error code or 0.
 * @ingroup krb5_crypto
 *
 * Kerberos encrypted data look like this:
 *
 * 1. KRB5_CRYPTO_TYPE_HEADER
 * 2. array [1,...] KRB5_CRYPTO_TYPE_DATA and array [0,...]
 *    KRB5_CRYPTO_TYPE_SIGN_ONLY in any order, however the receiver
 *    have to aware of the order. KRB5_CRYPTO_TYPE_SIGN_ONLY is
 *    commonly used headers and trailers.
 * 3. KRB5_CRYPTO_TYPE_PADDING, at least on padsize long if padsize > 1
 * 4. KRB5_CRYPTO_TYPE_TRAILER
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_encrypt_iov_ivec(krb5_context context,
		      krb5_crypto crypto,
		      unsigned usage,
		      krb5_crypto_iov *data,
		      int num_data,
		      void *ivec)
{
    size_t headersz, trailersz;
    Checksum cksum;
    krb5_data enc_data, sign_data;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    const struct _krb5_encryption_type *et = crypto->et;
    krb5_crypto_iov *tiv, *piv, *hiv;

    if (num_data < 0) {
        krb5_clear_error_message(context);
	return KRB5_CRYPTO_INTERNAL;
    }

    if(!derived_crypto(context, crypto)) {
	krb5_clear_error_message(context);
	return KRB5_CRYPTO_INTERNAL;
    }

    krb5_data_zero(&enc_data);
    krb5_data_zero(&sign_data);

    headersz = et->confoundersize;
    trailersz = CHECKSUMSIZE(et->keyed_checksum);

    /* header */
    hiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_HEADER);
    if (hiv == NULL || hiv->data.length != headersz)
	return KRB5_BAD_MSIZE;
    krb5_generate_random_block(hiv->data.data, hiv->data.length);

    /* padding */
    ret = iov_pad_validate(et, data, num_data, &piv);
    if(ret)
	goto cleanup;

    /* trailer */
    tiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_TRAILER);
    if (tiv == NULL || tiv->data.length != trailersz) {
	ret = KRB5_BAD_MSIZE;
	goto cleanup;
    }

    if (et->flags & F_ENC_THEN_CKSUM) {
	unsigned char old_ivec[EVP_MAX_IV_LENGTH];
	krb5_data ivec_data;

	heim_assert(et->blocksize <= sizeof(old_ivec),
		    "blocksize too big for ivec buffer");

	ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
	if(ret)
	    goto cleanup;

	ret = _key_schedule(context, dkey);
	if(ret)
	    goto cleanup;

	if (ivec)
	    memcpy(old_ivec, ivec, et->blocksize);
	else
	    memset(old_ivec, 0, et->blocksize);

	if (et->encrypt_iov != NULL) {
	    ret = (*et->encrypt_iov)(context, dkey, data, num_data, 1, usage,
				     ivec);
	    if (ret)
		goto cleanup;
	} else {
	    ret = iov_coalesce(context, NULL, data, num_data, FALSE, &enc_data);
	    if (ret)
		goto cleanup;

	    ret = (*et->encrypt)(context, dkey, enc_data.data, enc_data.length,
				 1, usage, ivec);
	    if (ret)
		goto cleanup;

	    ret = iov_uncoalesce(context, &enc_data, data, num_data);
	    if (ret)
		goto cleanup;
        }

	ivec_data.length = et->blocksize;
	ivec_data.data = old_ivec;

	ret = iov_coalesce(context, &ivec_data, data, num_data, TRUE, &sign_data);
	if(ret)
	    goto cleanup;

        ret = create_checksum(context,
                              et->keyed_checksum,
                              crypto,
                              INTEGRITY_USAGE(usage),
                              sign_data.data,
                              sign_data.length,
                              0,
                              &cksum);

        if(ret == 0 && cksum.checksum.length != trailersz) {
            free_Checksum (&cksum);
            krb5_clear_error_message (context);
            ret = KRB5_CRYPTO_INTERNAL;
        }
        if (ret)
            goto cleanup;

        /* save cksum at end */
        memcpy(tiv->data.data, cksum.checksum.data, cksum.checksum.length);
        free_Checksum (&cksum);

    } else {
        cksum.checksum = tiv->data;
        ret = create_checksum_iov(context,
                              et->keyed_checksum,
                              crypto,
                              INTEGRITY_USAGE(usage),
                              data,
                              num_data,
                              0,
                              &cksum);
        if (ret)
            goto cleanup;

        /* create_checksum may realloc the derived key space, so any keys
         * obtained before it was called may no longer be valid */
	ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
	if(ret)
	    goto cleanup;

	ret = _key_schedule(context, dkey);
	if(ret)
	    goto cleanup;

        if (et->encrypt_iov != NULL) {
            ret = (*et->encrypt_iov)(context, dkey, data, num_data, 1, usage,
                                     ivec);
            if (ret)
                goto cleanup;
        } else {
            ret = iov_coalesce(context, NULL, data, num_data, FALSE, &enc_data);
            if (ret)
                goto cleanup;

            ret = (*et->encrypt)(context, dkey, enc_data.data, enc_data.length,
                                 1, usage, ivec);
            if (ret)
                goto cleanup;

            ret = iov_uncoalesce(context, &enc_data, data, num_data);
            if (ret)
                goto cleanup;
        }
    }

cleanup:
    if (enc_data.data) {
	memset_s(enc_data.data, enc_data.length, 0, enc_data.length);
	krb5_data_free(&enc_data);
    }
    if (sign_data.data) {
	memset_s(sign_data.data, sign_data.length, 0, sign_data.length);
	krb5_data_free(&sign_data);
    }
    return ret;
}

/**
 * Inline decrypt a Kerberos message.
 *
 * @param context Kerberos context
 * @param crypto Kerberos crypto context
 * @param usage Key usage for this buffer
 * @param data array of buffers to process
 * @param num_data length of array
 * @param ivec initial cbc/cts vector
 *
 * @return Return an error code or 0.
 * @ingroup krb5_crypto
 *
 * 1. KRB5_CRYPTO_TYPE_HEADER
 * 2. one KRB5_CRYPTO_TYPE_DATA and array [0,...] of KRB5_CRYPTO_TYPE_SIGN_ONLY in
 *  any order, however the receiver have to aware of the
 *  order. KRB5_CRYPTO_TYPE_SIGN_ONLY is commonly used unencrypoted
 *  protocol headers and trailers. The output data will be of same
 *  size as the input data or shorter.
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_decrypt_iov_ivec(krb5_context context,
		      krb5_crypto crypto,
		      unsigned usage,
		      krb5_crypto_iov *data,
		      unsigned int num_data,
		      void *ivec)
{
    Checksum cksum;
    krb5_data enc_data, sign_data;
    krb5_error_code ret;
    struct _krb5_key_data *dkey;
    struct _krb5_encryption_type *et = crypto->et;
    krb5_crypto_iov *tiv, *hiv;

    if(!derived_crypto(context, crypto)) {
	krb5_clear_error_message(context);
	return KRB5_CRYPTO_INTERNAL;
    }

    /* header */
    hiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_HEADER);
    if (hiv == NULL || hiv->data.length != et->confoundersize)
	return KRB5_BAD_MSIZE;

    /* trailer */
    tiv = iov_find(data, num_data, KRB5_CRYPTO_TYPE_TRAILER);
    if (tiv->data.length != CHECKSUMSIZE(et->keyed_checksum))
	return KRB5_BAD_MSIZE;

    /* padding */
    if ((iov_enc_data_len(data, num_data) % et->padsize) != 0) {
	krb5_clear_error_message(context);
	return KRB5_BAD_MSIZE;
    }

    krb5_data_zero(&enc_data);
    krb5_data_zero(&sign_data);

    if (!(et->flags & F_ENC_THEN_CKSUM)) {
	ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
	if(ret)
	    goto cleanup;

	ret = _key_schedule(context, dkey);
	if(ret)
	    goto cleanup;

	if (et->encrypt_iov != NULL) {
	    ret = (*et->encrypt_iov)(context, dkey, data, num_data,
				     0, usage, ivec);
	    if(ret)
		goto cleanup;
	} else {
	    ret = iov_coalesce(context, NULL, data, num_data, FALSE, &enc_data);
	    if(ret)
		goto cleanup;

	    ret = (*et->encrypt)(context, dkey, enc_data.data, enc_data.length,
			         0, usage, ivec);
	    if(ret)
	        goto cleanup;

	    ret = iov_uncoalesce(context, &enc_data, data, num_data);
	    if(ret)
		goto cleanup;
	}

	cksum.checksum.data   = tiv->data.data;
	cksum.checksum.length = tiv->data.length;
	cksum.cksumtype       = CHECKSUMTYPE(et->keyed_checksum);

	ret = verify_checksum_iov(context, crypto, INTEGRITY_USAGE(usage),
	                          data, num_data, 0, &cksum);
	if(ret)
	    goto cleanup;
    } else {
	krb5_data ivec_data;
	static unsigned char zero_ivec[EVP_MAX_IV_LENGTH];

	heim_assert(et->blocksize <= sizeof(zero_ivec),
		    "blocksize too big for ivec buffer");

	ivec_data.length = et->blocksize;
	ivec_data.data = ivec ? ivec : zero_ivec;

	ret = iov_coalesce(context, &ivec_data, data, num_data, TRUE, &sign_data);
	if(ret)
	    goto cleanup;

	cksum.checksum.data   = tiv->data.data;
	cksum.checksum.length = tiv->data.length;
	cksum.cksumtype       = CHECKSUMTYPE(et->keyed_checksum);

	ret = verify_checksum(context,
			      crypto,
			      INTEGRITY_USAGE(usage),
			      sign_data.data,
			      sign_data.length,
			      0,
			      &cksum);
	if(ret)
	    goto cleanup;

	ret = iov_coalesce(context, NULL, data, num_data, FALSE, &enc_data);
	if(ret)
	    goto cleanup;

	ret = _get_derived_key(context, crypto, ENCRYPTION_USAGE(usage), &dkey);
	if(ret)
	    goto cleanup;

	ret = _key_schedule(context, dkey);
	if(ret)
	    goto cleanup;

	ret = (*et->encrypt)(context, dkey, enc_data.data, enc_data.length,
			     0, usage, ivec);
	if(ret)
	    goto cleanup;

	ret = iov_uncoalesce(context, &enc_data, data, num_data);
	if(ret)
	    goto cleanup;
    }

cleanup:
    if (enc_data.data) {
	memset_s(enc_data.data, enc_data.length, 0, enc_data.length);
	krb5_data_free(&enc_data);
    }
    if (sign_data.data) {
	memset_s(sign_data.data, sign_data.length, 0, sign_data.length);
	krb5_data_free(&sign_data);
    }
    return ret;
}

/**
 * Create a Kerberos message checksum.
 *
 * @param context Kerberos context
 * @param crypto Kerberos crypto context
 * @param usage Key usage for this buffer
 * @param data array of buffers to process
 * @param num_data length of array
 * @param type output data
 *
 * @return Return an error code or 0.
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_create_checksum_iov(krb5_context context,
			 krb5_crypto crypto,
			 unsigned usage,
			 krb5_crypto_iov *data,
			 unsigned int num_data,
			 krb5_cksumtype *type)
{
    Checksum cksum;
    krb5_crypto_iov *civ;
    struct _krb5_checksum_type *ct;
    unsigned keyusage;
    krb5_error_code ret;

    civ = iov_find(data, num_data, KRB5_CRYPTO_TYPE_CHECKSUM);
    if (civ == NULL)
	return KRB5_BAD_MSIZE;

    ct = crypto->et->keyed_checksum;
    if (ct == NULL)
	ct = crypto->et->checksum;

    if(ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type not found", ""));
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    if (arcfour_checksum_p(ct, crypto)) {
	keyusage = usage;
	_krb5_usage2arcfour(context, &keyusage);
    } else
	keyusage = CHECKSUM_USAGE(usage);

    if (ct->checksumsize > civ->data.length) {
	krb5_set_error_message(context, KRB5_BAD_MSIZE,
			       N_("Checksum larger then input buffer", ""));
	return KRB5_BAD_MSIZE;
    }

    cksum.checksum = civ->data;
    ret = create_checksum_iov(context, ct, crypto, keyusage,
			      data, num_data, crypto_flags(crypto), &cksum);

    if (ret == 0 && type)
	*type = cksum.cksumtype;

    return ret;
}

/**
 * Verify a Kerberos message checksum.
 *
 * @param context Kerberos context
 * @param crypto Kerberos crypto context
 * @param usage Key usage for this buffer
 * @param data array of buffers to process
 * @param num_data length of array
 * @param type return checksum type if not NULL
 *
 * @return Return an error code or 0.
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_verify_checksum_iov(krb5_context context,
			 krb5_crypto crypto,
			 unsigned usage,
			 krb5_crypto_iov *data,
			 unsigned int num_data,
			 krb5_cksumtype *type)
{
    struct _krb5_encryption_type *et = crypto->et;
    struct _krb5_checksum_type *ct;
    Checksum cksum;
    krb5_crypto_iov *civ;
    krb5_error_code ret;
    unsigned keyusage;

    civ = iov_find(data, num_data, KRB5_CRYPTO_TYPE_CHECKSUM);
    if (civ == NULL)
	return KRB5_BAD_MSIZE;

    cksum.cksumtype = CHECKSUMTYPE(et->keyed_checksum);
    cksum.checksum.length = civ->data.length;
    cksum.checksum.data = civ->data.data;

    ct = _krb5_find_checksum(cksum.cksumtype);
    if(ct == NULL) {
	krb5_set_error_message (context, KRB5_PROG_SUMTYPE_NOSUPP,
				N_("checksum type %d not supported", ""),
				cksum.cksumtype);
	return KRB5_PROG_SUMTYPE_NOSUPP;
    }

    if (arcfour_checksum_p(ct, crypto)) {
	keyusage = usage;
	_krb5_usage2arcfour(context, &keyusage);
    } else
	keyusage = CHECKSUM_USAGE(usage);

    ret = verify_checksum_iov(context, crypto, keyusage, data, num_data,
			      crypto_flags(crypto), &cksum);

    if (ret == 0 && type)
	*type = cksum.cksumtype;

    return ret;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_length(krb5_context context,
		   krb5_crypto crypto,
		   int type,
		   size_t *len)
{
    if (!derived_crypto(context, crypto)) {
	krb5_set_error_message(context, EINVAL, "not a derived crypto");
	return EINVAL;
    }

    switch(type) {
    case KRB5_CRYPTO_TYPE_EMPTY:
	*len = 0;
	return 0;
    case KRB5_CRYPTO_TYPE_HEADER:
	*len = crypto->et->blocksize;
	return 0;
    case KRB5_CRYPTO_TYPE_DATA:
    case KRB5_CRYPTO_TYPE_SIGN_ONLY:
	/* len must already been filled in */
	return 0;
    case KRB5_CRYPTO_TYPE_PADDING:
	if (crypto->et->padsize > 1)
	    *len = crypto->et->padsize;
	else
	    *len = 0;
	return 0;
    case KRB5_CRYPTO_TYPE_TRAILER:
        if (crypto->et->keyed_checksum)
            *len = CHECKSUMSIZE(crypto->et->keyed_checksum);
        else
            *len = 0;
	return 0;
    case KRB5_CRYPTO_TYPE_CHECKSUM:
	if (crypto->et->keyed_checksum)
	    *len = CHECKSUMSIZE(crypto->et->keyed_checksum);
	else
	    *len = CHECKSUMSIZE(crypto->et->checksum);
	return 0;
    }
    krb5_set_error_message(context, EINVAL,
			   "%d not a supported type", type);
    return EINVAL;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_length_iov(krb5_context context,
		       krb5_crypto crypto,
		       krb5_crypto_iov *data,
		       unsigned int num_data)
{
    krb5_error_code ret;
    size_t i;

    for (i = 0; i < num_data; i++) {
	ret = krb5_crypto_length(context, crypto,
				 data[i].flags,
				 &data[i].data.length);
	if (ret)
	    return ret;
    }
    return 0;
}


KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_encrypt_ivec(krb5_context context,
		  krb5_crypto crypto,
		  unsigned usage,
		  const void *data,
		  size_t len,
		  krb5_data *result,
		  void *ivec)
{
    krb5_error_code ret;

    switch (crypto->et->flags & F_CRYPTO_MASK) {
    case F_RFC3961_ENC:
	ret = encrypt_internal_derived(context, crypto, usage,
				       data, len, result, ivec);
	break;
    case F_SPECIAL:
	ret = encrypt_internal_special (context, crypto, usage,
					data, len, result, ivec);
	break;
    case F_ENC_THEN_CKSUM:
	ret = encrypt_internal_enc_then_cksum(context, crypto, usage,
					      data, len, result, ivec);
	break;
    default:
	ret = encrypt_internal(context, crypto, data, len, result, ivec);
	break;
    }

    return ret;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_encrypt(krb5_context context,
	     krb5_crypto crypto,
	     unsigned usage,
	     const void *data,
	     size_t len,
	     krb5_data *result)
{
    return krb5_encrypt_ivec(context, crypto, usage, data, len, result, NULL);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_encrypt_EncryptedData(krb5_context context,
			   krb5_crypto crypto,
			   unsigned usage,
			   void *data,
			   size_t len,
			   int kvno,
			   EncryptedData *result)
{
    result->etype = CRYPTO_ETYPE(crypto);
    if(kvno){
	ALLOC(result->kvno, 1);
	*result->kvno = kvno;
    }else
	result->kvno = NULL;
    return krb5_encrypt(context, crypto, usage, data, len, &result->cipher);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_decrypt_ivec(krb5_context context,
		  krb5_crypto crypto,
		  unsigned usage,
		  void *data,
		  size_t len,
		  krb5_data *result,
		  void *ivec)
{
    krb5_error_code ret;

    switch (crypto->et->flags & F_CRYPTO_MASK) {
    case F_RFC3961_ENC:
	ret = decrypt_internal_derived(context, crypto, usage,
				data, len, result, ivec);
	break;
    case F_SPECIAL:
	ret = decrypt_internal_special(context, crypto, usage,
				       data, len, result, ivec);
	break;
    case F_ENC_THEN_CKSUM:
	ret = decrypt_internal_enc_then_cksum(context, crypto, usage,
					      data, len, result, ivec);
	break;
    default:
	ret = decrypt_internal(context, crypto, data, len, result, ivec);
	break;
    }

    return ret;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_decrypt(krb5_context context,
	     krb5_crypto crypto,
	     unsigned usage,
	     void *data,
	     size_t len,
	     krb5_data *result)
{
    return krb5_decrypt_ivec (context, crypto, usage, data, len, result,
			      NULL);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_decrypt_EncryptedData(krb5_context context,
			   krb5_crypto crypto,
			   unsigned usage,
			   const EncryptedData *e,
			   krb5_data *result)
{
    return krb5_decrypt(context, crypto, usage,
			e->cipher.data, e->cipher.length, result);
}

/************************************************************
 *                                                          *
 ************************************************************/

static krb5_error_code
derive_key_rfc3961(krb5_context context,
		   struct _krb5_encryption_type *et,
		   struct _krb5_key_data *key,
		   const void *constant,
		   size_t len)
{

    unsigned char *k = NULL;
    unsigned int nblocks = 0, i;
    krb5_error_code ret = 0;
    struct _krb5_key_type *kt = et->keytype;

    if(et->blocksize * 8 < kt->bits || len != et->blocksize) {
	nblocks = (kt->bits + et->blocksize * 8 - 1) / (et->blocksize * 8);
	k = malloc(nblocks * et->blocksize);
	if(k == NULL) {
	    ret = krb5_enomem(context);
	    goto out;
	}
	ret = _krb5_n_fold(constant, len, k, et->blocksize);
	if (ret) {
	    krb5_enomem(context);
	    goto out;
	}

	for(i = 0; i < nblocks; i++) {
	    if(i > 0)
		memcpy(k + i * et->blocksize,
		       k + (i - 1) * et->blocksize,
		       et->blocksize);
	    ret = (*et->encrypt)(context, key, k + i * et->blocksize,
				 et->blocksize, 1, 0, NULL);
	    if (ret) {
		    krb5_set_error_message(context, ret, N_("encrypt failed", ""));
		    goto out;
	    }
	}
    } else {
	/* this case is probably broken, but won't be run anyway */
	void *c = malloc(len);
	size_t res_len = (kt->bits + 7) / 8;

	if(len != 0 && c == NULL) {
	    ret = krb5_enomem(context);
	    goto out;
	}
	memcpy(c, constant, len);
	ret = (*et->encrypt)(context, key, c, len, 1, 0, NULL);
	if (ret) {
		free(c);
		krb5_set_error_message(context, ret, N_("encrypt failed", ""));
		goto out;
	}
	k = malloc(res_len);
	if(res_len != 0 && k == NULL) {
	    free(c);
	    ret = krb5_enomem(context);
	    goto out;
	}
	ret = _krb5_n_fold(c, len, k, res_len);
	free(c);
	if (ret) {
	    krb5_enomem(context);
	    goto out;
	}
    }

    if (kt->type == KRB5_ENCTYPE_OLD_DES3_CBC_SHA1)
	_krb5_DES3_random_to_key(context, key->key, k, nblocks * et->blocksize);
    else
	memcpy(key->key->keyvalue.data, k, key->key->keyvalue.length);

 out:
    if (k) {
	memset_s(k, nblocks * et->blocksize, 0, nblocks * et->blocksize);
	free(k);
    }
    return ret;
}

static krb5_error_code
derive_key_sp800_hmac(krb5_context context,
		      struct _krb5_encryption_type *et,
		      struct _krb5_key_data *key,
		      const void *constant,
		      size_t len)
{
    krb5_error_code ret;
    struct _krb5_key_type *kt = et->keytype;
    krb5_data label;
    const EVP_MD *md = NULL;
    const unsigned char *c = constant;
    size_t key_len;
    krb5_data K1;

    ret = _krb5_aes_sha2_md_for_enctype(context, kt->type, &md);
    if (ret)
	return ret;

    /*
     * PRF usage: not handled here (output cannot be longer)
     * Integrity usage: truncated hash (half length)
     * Encryption usage: base key length
     */
    if (len == 5 && (c[4] == 0x99 || c[4] == 0x55))
	key_len = EVP_MD_size(md) / 2;
    else
	key_len = kt->size;

    ret = krb5_data_alloc(&K1, key_len);
    if (ret)
	return ret;

    label.data = (void *)constant;
    label.length = len;

    ret = _krb5_SP800_108_HMAC_KDF(context, &key->key->keyvalue,
				   &label, NULL, md, &K1);
    if (ret == 0) {
	if (key->key->keyvalue.length > key_len)
	    key->key->keyvalue.length = key_len;
	memcpy(key->key->keyvalue.data, K1.data, key_len);
    }

    memset_s(K1.data, K1.length, 0, K1.length);
    krb5_data_free(&K1);

    return ret;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
_krb5_derive_key(krb5_context context,
		 struct _krb5_encryption_type *et,
		 struct _krb5_key_data *key,
		 const void *constant,
		 size_t len)
{
    krb5_error_code ret;

    ret = _key_schedule(context, key);
    if(ret)
	return ret;

    switch (et->flags & F_KDF_MASK) {
    case F_RFC3961_KDF:
	ret = derive_key_rfc3961(context, et, key, constant, len);
	break;
    case F_SP800_108_HMAC_KDF:
	ret = derive_key_sp800_hmac(context, et, key, constant, len);
	break;
    default:
	ret = KRB5_CRYPTO_INTERNAL;
	krb5_set_error_message(context, ret,
			       N_("derive_key() called with unknown keytype (%u)", ""),
			       et->keytype->type);
	break;
    }

    if (key->schedule) {
	free_key_schedule(context, key, et);
	key->schedule = NULL;
    }

    return ret;
}

static struct _krb5_key_data *
_new_derived_key(krb5_crypto crypto, unsigned usage)
{
    struct _krb5_key_usage *d = crypto->key_usage;
    d = realloc(d, (crypto->num_key_usage + 1) * sizeof(*d));
    if(d == NULL)
	return NULL;
    crypto->key_usage = d;
    d += crypto->num_key_usage++;
    memset(d, 0, sizeof(*d));
    d->usage = usage;
    return &d->key;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_derive_key(krb5_context context,
		const krb5_keyblock *key,
		krb5_enctype etype,
		const void *constant,
		size_t constant_len,
		krb5_keyblock **derived_key)
{
    krb5_error_code ret;
    struct _krb5_encryption_type *et;
    struct _krb5_key_data d;

    *derived_key = NULL;

    et = _krb5_find_enctype (etype);
    if (et == NULL) {
        return unsupported_enctype (context, etype);
    }

    ret = krb5_copy_keyblock(context, key, &d.key);
    if (ret)
	return ret;

    d.schedule = NULL;
    ret = _krb5_derive_key(context, et, &d, constant, constant_len);
    if (ret == 0)
	ret = krb5_copy_keyblock(context, d.key, derived_key);
    _krb5_free_key_data(context, &d, et);
    return ret;
}

static krb5_error_code
_get_derived_key(krb5_context context,
		 krb5_crypto crypto,
		 unsigned usage,
		 struct _krb5_key_data **key)
{
    int i;
    struct _krb5_key_data *d;
    unsigned char constant[5];

    *key = NULL;
    for(i = 0; i < crypto->num_key_usage; i++)
	if(crypto->key_usage[i].usage == usage) {
	    *key = &crypto->key_usage[i].key;
	    return 0;
	}
    d = _new_derived_key(crypto, usage);
    if (d == NULL)
	return krb5_enomem(context);
    *key = d;
    krb5_copy_keyblock(context, crypto->key.key, &d->key);
    _krb5_put_int(constant, usage, sizeof(constant));
    return _krb5_derive_key(context, crypto->et, d, constant, sizeof(constant));
}

/**
 * Create a crypto context used for all encryption and signature
 * operation. The encryption type to use is taken from the key, but
 * can be overridden with the enctype parameter.  This can be useful
 * for encryptions types which is compatiable (DES for example).
 *
 * To free the crypto context, use krb5_crypto_destroy().
 *
 * @param context Kerberos context
 * @param key the key block information with all key data
 * @param etype the encryption type
 * @param crypto the resulting crypto context
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_init(krb5_context context,
		 const krb5_keyblock *key,
		 krb5_enctype etype,
		 krb5_crypto *crypto)
{
    krb5_error_code ret;
    ALLOC(*crypto, 1);
    if (*crypto == NULL)
	return krb5_enomem(context);
    if(etype == ETYPE_NULL)
	etype = key->keytype;
    (*crypto)->et = _krb5_find_enctype(etype);
    if((*crypto)->et == NULL || ((*crypto)->et->flags & F_DISABLED)) {
	free(*crypto);
	*crypto = NULL;
	return unsupported_enctype(context, etype);
    }
    if((*crypto)->et->keytype->size != key->keyvalue.length) {
	free(*crypto);
	*crypto = NULL;
	krb5_set_error_message (context, KRB5_BAD_KEYSIZE,
				"encryption key has bad length");
	return KRB5_BAD_KEYSIZE;
    }
    ret = krb5_copy_keyblock(context, key, &(*crypto)->key.key);
    if(ret) {
	free(*crypto);
	*crypto = NULL;
	return ret;
    }
    (*crypto)->key.schedule = NULL;
    (*crypto)->num_key_usage = 0;
    (*crypto)->key_usage = NULL;
    (*crypto)->flags = 0;
    return 0;
}

static void
free_key_schedule(krb5_context context,
		  struct _krb5_key_data *key,
		  struct _krb5_encryption_type *et)
{
    if (et->keytype->cleanup)
	(*et->keytype->cleanup)(context, key);
    memset(key->schedule->data, 0, key->schedule->length);
    krb5_free_data(context, key->schedule);
}

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
_krb5_free_key_data(krb5_context context, struct _krb5_key_data *key,
	      struct _krb5_encryption_type *et)
{
    krb5_free_keyblock(context, key->key);
    if(key->schedule) {
	free_key_schedule(context, key, et);
	key->schedule = NULL;
    }
}

static void
free_key_usage(krb5_context context, struct _krb5_key_usage *ku,
	       struct _krb5_encryption_type *et)
{
    _krb5_free_key_data(context, &ku->key, et);
}

/**
 * Free a crypto context created by krb5_crypto_init().
 *
 * @param context Kerberos context
 * @param crypto crypto context to free
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_destroy(krb5_context context,
		    krb5_crypto crypto)
{
    int i;

    for(i = 0; i < crypto->num_key_usage; i++)
	free_key_usage(context, &crypto->key_usage[i], crypto->et);
    free(crypto->key_usage);
    _krb5_free_key_data(context, &crypto->key, crypto->et);

    if (crypto->mdctx)
	EVP_MD_CTX_destroy(crypto->mdctx);

    if (crypto->hmacctx)
	HMAC_CTX_free(crypto->hmacctx);

    free (crypto);
    return 0;
}

/**
 * Return the blocksize used algorithm referenced by the crypto context
 *
 * @param context Kerberos context
 * @param crypto crypto context to query
 * @param blocksize the resulting blocksize
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_getblocksize(krb5_context context,
			 krb5_crypto crypto,
			 size_t *blocksize)
{
    *blocksize = crypto->et->blocksize;
    return 0;
}

/**
 * Return the encryption type used by the crypto context
 *
 * @param context Kerberos context
 * @param crypto crypto context to query
 * @param enctype the resulting encryption type
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_getenctype(krb5_context context,
		       krb5_crypto crypto,
		       krb5_enctype *enctype)
{
    *enctype = crypto->et->type;
    return 0;
}

/**
 * Return the padding size used by the crypto context
 *
 * @param context Kerberos context
 * @param crypto crypto context to query
 * @param padsize the return padding size
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_getpadsize(krb5_context context,
                       krb5_crypto crypto,
                       size_t *padsize)
{
    *padsize = crypto->et->padsize;
    return 0;
}

/**
 * Return the confounder size used by the crypto context
 *
 * @param context Kerberos context
 * @param crypto crypto context to query
 * @param confoundersize the returned confounder size
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_getconfoundersize(krb5_context context,
                              krb5_crypto crypto,
                              size_t *confoundersize)
{
    *confoundersize = crypto->et->confoundersize;
    return 0;
}


/**
 * Disable encryption type
 *
 * @param context Kerberos 5 context
 * @param enctype encryption type to disable
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_disable(krb5_context context,
		     krb5_enctype enctype)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(enctype);
    if(et == NULL) {
	if (context)
	    krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
				    N_("encryption type %d not supported", ""),
				    enctype);
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    et->flags |= F_DISABLED;
    return 0;
}

/**
 * Enable encryption type
 *
 * @param context Kerberos 5 context
 * @param enctype encryption type to enable
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_enctype_enable(krb5_context context,
		    krb5_enctype enctype)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(enctype);
    if(et == NULL) {
	if (context)
	    krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
				    N_("encryption type %d not supported", ""),
				    enctype);
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    et->flags &= ~F_DISABLED;
    return 0;
}

/**
 * Enable or disable all weak encryption types
 *
 * @param context Kerberos 5 context
 * @param enable true to enable, false to disable
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_allow_weak_crypto(krb5_context context,
		       krb5_boolean enable)
{
    int i;

    for(i = 0; i < _krb5_num_etypes; i++)
	if(_krb5_etypes[i]->flags & F_WEAK) {
	    if(enable)
		_krb5_etypes[i]->flags &= ~F_DISABLED;
	    else
		_krb5_etypes[i]->flags |= F_DISABLED;
	}
    return 0;
}

/**
 * Returns is the encryption is strong or weak
 *
 * @param context Kerberos 5 context
 * @param enctype encryption type to probe
 *
 * @return Returns true if encryption type is weak or is not supported.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_is_enctype_weak(krb5_context context, krb5_enctype enctype)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(enctype);
    if(et == NULL || (et->flags & F_WEAK))
	return TRUE;
    return FALSE;
}

/**
 * Returns whether the encryption type is new or old
 *
 * @param context Kerberos 5 context
 * @param enctype encryption type to probe
 *
 * @return Returns true if encryption type is old or is not supported.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_is_enctype_old(krb5_context context, krb5_enctype enctype)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(enctype);
    if (!et  || (et->flags & F_OLD))
	return TRUE;
    return FALSE;
}

/**
 * Returns whether the encryption type should use randomly generated salts
 *
 * @param context Kerberos 5 context
 * @param enctype encryption type to probe
 *
 * @return Returns true if generated salts should have random component
 *
 * @ingroup krb5_crypto
 */
KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
_krb5_enctype_requires_random_salt(krb5_context context,
				   krb5_enctype enctype)
{
    struct _krb5_encryption_type *et;

    et = _krb5_find_enctype (enctype);

    return et && (et->flags & F_SP800_108_HMAC_KDF);
}

static size_t
wrapped_length (krb5_context context,
		krb5_crypto  crypto,
		size_t       data_len)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t padsize = et->padsize;
    size_t checksumsize = CHECKSUMSIZE(et->checksum);
    size_t res;

    res =  et->confoundersize + checksumsize + data_len;
    res =  (res + padsize - 1) / padsize * padsize;
    return res;
}

static size_t
wrapped_length_dervied (krb5_context context,
			krb5_crypto  crypto,
			size_t       data_len)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t padsize = et->padsize;
    size_t res;

    res =  et->confoundersize + data_len;
    res =  (res + padsize - 1) / padsize * padsize;
    if (et->keyed_checksum)
	res += et->keyed_checksum->checksumsize;
    else
	res += et->checksum->checksumsize;
    return res;
}

/*
 * Return the size of an encrypted packet of length `data_len'
 */

KRB5_LIB_FUNCTION size_t KRB5_LIB_CALL
krb5_get_wrapped_length (krb5_context context,
			 krb5_crypto  crypto,
			 size_t       data_len)
{
    if (derived_crypto (context, crypto))
	return wrapped_length_dervied (context, crypto, data_len);
    else
	return wrapped_length (context, crypto, data_len);
}

/*
 * Return the size of an encrypted packet of length `data_len'
 */

static size_t
crypto_overhead (krb5_context context,
		 krb5_crypto  crypto)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t res;

    res = CHECKSUMSIZE(et->checksum);
    res += et->confoundersize;
    if (et->padsize > 1)
	res += et->padsize;
    return res;
}

static size_t
crypto_overhead_dervied (krb5_context context,
			 krb5_crypto  crypto)
{
    struct _krb5_encryption_type *et = crypto->et;
    size_t res;

    if (et->keyed_checksum)
	res = CHECKSUMSIZE(et->keyed_checksum);
    else
	res = CHECKSUMSIZE(et->checksum);
    res += et->confoundersize;
    if (et->padsize > 1)
	res += et->padsize;
    return res;
}

KRB5_LIB_FUNCTION size_t KRB5_LIB_CALL
krb5_crypto_overhead (krb5_context context, krb5_crypto crypto)
{
    if (derived_crypto (context, crypto))
	return crypto_overhead_dervied (context, crypto);
    else
	return crypto_overhead (context, crypto);
}

/**
 * Converts the random bytestring to a protocol key according to
 * Kerberos crypto frame work. It may be assumed that all the bits of
 * the input string are equally random, even though the entropy
 * present in the random source may be limited.
 *
 * @param context Kerberos 5 context
 * @param type the enctype resulting key will be of
 * @param data input random data to convert to a key
 * @param size size of input random data, at least krb5_enctype_keysize() long
 * @param key key, output key, free with krb5_free_keyblock_contents()
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_random_to_key(krb5_context context,
		   krb5_enctype type,
		   const void *data,
		   size_t size,
		   krb5_keyblock *key)
{
    krb5_error_code ret;
    struct _krb5_encryption_type *et = _krb5_find_enctype(type);
    if(et == NULL) {
	krb5_set_error_message(context, KRB5_PROG_ETYPE_NOSUPP,
			       N_("encryption type %d not supported", ""),
			       type);
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    if ((et->keytype->bits + 7) / 8 > size) {
	krb5_set_error_message(context, KRB5_PROG_ETYPE_NOSUPP,
			       N_("encryption key %s needs %d bytes "
				  "of random to make an encryption key "
				  "out of it", ""),
			       et->name, (int)et->keytype->size);
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    ret = krb5_data_alloc(&key->keyvalue, et->keytype->size);
    if(ret)
	return ret;
    key->keytype = type;
    if (et->keytype->random_to_key)
 	(*et->keytype->random_to_key)(context, key, data, size);
    else
	memcpy(key->keyvalue.data, data, et->keytype->size);

    return 0;
}



KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_prf_length(krb5_context context,
		       krb5_enctype type,
		       size_t *length)
{
    struct _krb5_encryption_type *et = _krb5_find_enctype(type);

    if(et == NULL || et->prf_length == 0) {
	krb5_set_error_message(context, KRB5_PROG_ETYPE_NOSUPP,
			       N_("encryption type %d not supported", ""),
			       type);
	return KRB5_PROG_ETYPE_NOSUPP;
    }

    *length = et->prf_length;
    return 0;
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_prf(krb5_context context,
		const krb5_crypto crypto,
		const krb5_data *input,
		krb5_data *output)
{
    struct _krb5_encryption_type *et = crypto->et;

    krb5_data_zero(output);

    if(et->prf == NULL) {
	krb5_set_error_message(context, KRB5_PROG_ETYPE_NOSUPP,
			       "kerberos prf for %s not supported",
			       et->name);
	return KRB5_PROG_ETYPE_NOSUPP;
    }

    return (*et->prf)(context, crypto, input, output);
}

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_prfplus(krb5_context context,
		    const krb5_crypto crypto,
		    const krb5_data *input,
		    size_t length,
		    krb5_data *output)
{
    krb5_error_code ret;
    krb5_data input2;
    unsigned char i = 1;
    unsigned char *p;

    krb5_data_zero(&input2);
    krb5_data_zero(output);

    krb5_clear_error_message(context);

    ret = krb5_data_alloc(output, length);
    if (ret) goto out;
    ret = krb5_data_alloc(&input2, input->length + 1);
    if (ret) goto out;

    krb5_clear_error_message(context);

    memcpy(((unsigned char *)input2.data) + 1, input->data, input->length);

    p = output->data;

    while (length) {
	krb5_data block;

	((unsigned char *)input2.data)[0] = i++;

	ret = krb5_crypto_prf(context, crypto, &input2, &block);
	if (ret)
	    goto out;

	if (block.length < length) {
	    memcpy(p, block.data, block.length);
	    length -= block.length;
	} else {
	    memcpy(p, block.data, length);
	    length = 0;
	}
	p += block.length;
	krb5_data_free(&block);
    }

 out:
    krb5_data_free(&input2);
    if (ret)
	krb5_data_free(output);
    return ret;
}

/**
 * The FX-CF2 key derivation function, used in FAST and preauth framework.
 *
 * @param context Kerberos 5 context
 * @param crypto1 first key to combine
 * @param crypto2 second key to combine
 * @param pepper1 factor to combine with first key to garante uniqueness
 * @param pepper2 factor to combine with second key to garante uniqueness
 * @param enctype the encryption type of the resulting key
 * @param res allocated key, free with krb5_free_keyblock_contents()
 *
 * @return Return an error code or 0.
 *
 * @ingroup krb5_crypto
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_crypto_fx_cf2(krb5_context context,
		   const krb5_crypto crypto1,
		   const krb5_crypto crypto2,
		   krb5_data *pepper1,
		   krb5_data *pepper2,
		   krb5_enctype enctype,
		   krb5_keyblock *res)
{
    krb5_error_code ret;
    krb5_data os1, os2;
    size_t i, keysize;

    memset(res, 0, sizeof(*res));
    krb5_data_zero(&os1);
    krb5_data_zero(&os2);

    ret = krb5_enctype_keybits(context, enctype, &keysize);
    if (ret)
	return ret;
    keysize = (keysize + 7) / 8;

    ret = krb5_crypto_prfplus(context, crypto1, pepper1, keysize, &os1);
    if (ret)
	goto out;
    ret = krb5_crypto_prfplus(context, crypto2, pepper2, keysize, &os2);
    if (ret)
	goto out;

    res->keytype = enctype;
    {
	unsigned char *p1 = os1.data, *p2 = os2.data;
	for (i = 0; i < keysize; i++)
	    p1[i] ^= p2[i];
    }
    ret = krb5_random_to_key(context, enctype, os1.data, keysize, res);
 out:
    krb5_data_free(&os1);
    krb5_data_free(&os2);

    return ret;
}

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
_krb5_crypto_set_flags(krb5_context context,
		       krb5_crypto crypto,
		       krb5_flags flags)
{
    crypto->flags |= flags;
}

#ifndef HEIMDAL_SMALLER

/**
 * Deprecated: keytypes doesn't exists, they are really enctypes.
 *
 * @ingroup krb5_deprecated
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_keytype_to_enctypes (krb5_context context,
			  krb5_keytype keytype,
			  unsigned *len,
			  krb5_enctype **val)
    KRB5_DEPRECATED_FUNCTION("Use X instead")
{
    int i;
    unsigned n = 0;
    krb5_enctype *ret;

    for (i = _krb5_num_etypes - 1; i >= 0; --i) {
	if (_krb5_etypes[i]->keytype->type == keytype
	    && !(_krb5_etypes[i]->flags & F_PSEUDO)
	    && krb5_enctype_valid(context, _krb5_etypes[i]->type) == 0)
	    ++n;
    }
    if (n == 0) {
	krb5_set_error_message(context, KRB5_PROG_KEYTYPE_NOSUPP,
			       "Keytype have no mapping");
	return KRB5_PROG_KEYTYPE_NOSUPP;
    }

    ret = malloc(n * sizeof(*ret));
    if (ret == NULL && n != 0)
	return krb5_enomem(context);
    n = 0;
    for (i = _krb5_num_etypes - 1; i >= 0; --i) {
	if (_krb5_etypes[i]->keytype->type == keytype
	    && !(_krb5_etypes[i]->flags & F_PSEUDO)
	    && krb5_enctype_valid(context, _krb5_etypes[i]->type) == 0)
	    ret[n++] = _krb5_etypes[i]->type;
    }
    *len = n;
    *val = ret;
    return 0;
}

/**
 * Deprecated: keytypes doesn't exists, they are really enctypes.
 *
 * @ingroup krb5_deprecated
 */

/* if two enctypes have compatible keys */
KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_enctypes_compatible_keys(krb5_context context,
			      krb5_enctype etype1,
			      krb5_enctype etype2)
    KRB5_DEPRECATED_FUNCTION("Use X instead")
{
    struct _krb5_encryption_type *e1 = _krb5_find_enctype(etype1);
    struct _krb5_encryption_type *e2 = _krb5_find_enctype(etype2);
    return e1 != NULL && e2 != NULL && e1->keytype == e2->keytype;
}

#endif /* HEIMDAL_SMALLER */
