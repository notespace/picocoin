/* Copyright 2012 exMULTI, Inc.
 * Copyright (c) 2009-2012 The Bitcoin developers
 * Distributed under the MIT/X11 software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */
#include "picocoin-config.h"

#include <string.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/ripemd.h>
#include <ccoin/key.h>

/* Generate a private key from just the secret parameter */
static int EC_KEY_regenerate_key(EC_KEY *eckey, BIGNUM *priv_key)
{
	int ok = 0;
	BN_CTX *ctx = NULL;
	EC_POINT *pub_key = NULL;

	if (!eckey) return 0;

	const EC_GROUP *group = EC_KEY_get0_group(eckey);

	if ((ctx = BN_CTX_new()) == NULL)
		goto err;

	pub_key = EC_POINT_new(group);

	if (pub_key == NULL)
		goto err;

	if (!EC_POINT_mul(group, pub_key, priv_key, NULL, NULL, ctx))
		goto err;

	EC_KEY_set_private_key(eckey,priv_key);
	EC_KEY_set_public_key(eckey,pub_key);

	ok = 1;

err:

	if (pub_key)
		EC_POINT_free(pub_key);
	if (ctx != NULL)
		BN_CTX_free(ctx);

	return(ok);
}

bool bp_key_init(struct bp_key *key)
{
	memset(key, 0, sizeof(*key));

	key->k = EC_KEY_new_by_curve_name(NID_secp256k1);
	if (!key->k)
		return false;

	return true;
}

void bp_key_free(struct bp_key *key)
{
	if (key->k) {
		EC_KEY_free(key->k);
		key->k = NULL;
	}
}

bool bp_key_generate(struct bp_key *key)
{
	if (!key->k)
		return false;

	if (!EC_KEY_generate_key(key->k))
		return false;
	if (!EC_KEY_check_key(key->k))
		return false;

	EC_KEY_set_conv_form(key->k, POINT_CONVERSION_COMPRESSED);

	return true;
}

bool bp_privkey_set(struct bp_key *key, const void *privkey_, size_t pk_len)
{
	const unsigned char *privkey = privkey_;
	if (!d2i_ECPrivateKey(&key->k, &privkey, pk_len))
		return false;
	if (!EC_KEY_check_key(key->k))
		return false;

	EC_KEY_set_conv_form(key->k, POINT_CONVERSION_COMPRESSED);

	return true;
}

bool bp_pubkey_set(struct bp_key *key, const void *pubkey_, size_t pk_len)
{
	const unsigned char *pubkey = pubkey_;
	if (!o2i_ECPublicKey(&key->k, &pubkey, pk_len))
		return false;
	if (pk_len == 33)
		EC_KEY_set_conv_form(key->k, POINT_CONVERSION_COMPRESSED);
	return true;
}

bool bp_key_secret_set(struct bp_key *key, const void *privkey_, size_t pk_len)
{
	bp_key_free(key);

	if (!privkey_ || pk_len != 32)
		return false;

	const unsigned char *privkey = privkey_;
	BIGNUM *bn = BN_bin2bn(privkey, 32, BN_new());
	if (!bn)
		return false;

	key->k = EC_KEY_new_by_curve_name(NID_secp256k1);
	if (!key->k)
		goto err_out;
	
	if (!EC_KEY_regenerate_key(key->k, bn))
		goto err_out;
	if (!EC_KEY_check_key(key->k))
		return false;

	EC_KEY_set_conv_form(key->k, POINT_CONVERSION_COMPRESSED);

	BN_clear_free(bn);
	return true;

err_out:
	bp_key_free(key);
	BN_clear_free(bn);
	return false;
}

bool bp_privkey_get(struct bp_key *key, void **privkey, size_t *pk_len)
{
	if (!EC_KEY_check_key(key->k))
		return false;

	size_t sz = i2d_ECPrivateKey(key->k, 0);
	unsigned char *orig_mem, *mem = malloc(sz);
	orig_mem = mem;
	i2d_ECPrivateKey(key->k, &mem);

	*privkey = orig_mem;
	*pk_len = sz;

	return true;
}

bool bp_pubkey_get(struct bp_key *key, void **pubkey, size_t *pk_len)
{
	if (!EC_KEY_check_key(key->k))
		return false;

	size_t sz = i2o_ECPublicKey(key->k, 0);
	unsigned char *orig_mem, *mem = malloc(sz);
	orig_mem = mem;
	i2o_ECPublicKey(key->k, &mem);

	*pubkey = orig_mem;
	*pk_len = sz;

	return true;
}


bool bp_pubkey_get_uncompressed(struct bp_key *key, void **pubkey, size_t *pk_len){
	bool ret;
	EC_KEY_set_conv_form(key->k, POINT_CONVERSION_UNCOMPRESSED);
	ret = bp_pubkey_get(key, pubkey, pk_len);
	EC_KEY_set_conv_form(key->k, POINT_CONVERSION_COMPRESSED);
}

bool bp_key_secret_get(void *p, size_t len, const struct bp_key *key)
{
	if (!p || len < 32 || !key)
		return false;

	/* zero buffer */
	memset(p, 0, len);

	/* get bignum secret */
	const BIGNUM *bn = EC_KEY_get0_private_key(key->k);
	if (!bn)
		return false;
	int nBytes = BN_num_bytes(bn);

	/* store secret at end of buffer */
	int n = BN_bn2bin(bn, p + (len - nBytes));
	if (n != nBytes)
		return false;
	
	return true;
}

bool bp_sign(struct bp_key *key, const void *data, size_t data_len,
	     void **sig_, size_t *sig_len_)
{
	size_t sig_sz = ECDSA_size(key->k);
	void *sig = calloc(1, sig_sz);
	unsigned int sig_sz_out = sig_sz;

	int src = ECDSA_sign(0, data, data_len, sig, &sig_sz_out, key->k);
	if (src != 1) {
		free(sig);
		return false;
	}

	*sig_ = sig;
	*sig_len_ = sig_sz_out;

	return true;
}

bool bp_verify(struct bp_key *key, const void *data, size_t data_len,
	       const void *sig, size_t sig_len)
{
	return ECDSA_verify(0, data, data_len, sig, sig_len, key->k) == 1;
}

