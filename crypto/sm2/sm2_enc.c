#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <strings.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include "sm2.h"

int SM2_CIPHERTEXT_VALUE_size(const EC_GROUP *ec_group,
	point_conversion_form_t point_form, size_t mlen,
	const EVP_MD *mac_md)
{
	int ret = 0;
	EC_POINT *point = EC_POINT_new(ec_group);
	BN_CTX *bn_ctx = BN_CTX_new();
	size_t len;

	if (!point || !bn_ctx) {
		goto end;
	}
	
	if (!(len = EC_POINT_point2oct(ec_group, point, point_form,
		NULL, 0, bn_ctx))) {
		goto end;
	}
	len += mlen + EVP_MD_size(mac_md);

	ret = len;
end:
	if (point) EC_POINT_free(point);
	if (bn_ctx) BN_CTX_free(bn_ctx);
 	return ret;
}

void SM2_CIPHERTEXT_VALUE_free(SM2_CIPHERTEXT_VALUE *cv)
{
	if (cv->ephem_point) EC_POINT_free(cv->ephem_point);
	if (cv->ciphertext) OPENSSL_free(cv->ciphertext);
	bzero(cv, sizeof(SM2_CIPHERTEXT_VALUE));
	OPENSSL_free(cv);
}

int SM2_CIPHERTEXT_VALUE_encode(const SM2_CIPHERTEXT_VALUE *cv,
	const EC_GROUP *ec_group, point_conversion_form_t point_form,
	unsigned char *buf, size_t *buflen)
{
	int ret = 0;
	BN_CTX *bn_ctx = BN_CTX_new();
	size_t ptlen, cvlen;

	if (!bn_ctx) {
		return 0;
	}

	if (!(ptlen = EC_POINT_point2oct(ec_group, cv->ephem_point,
		point_form, NULL, 0, bn_ctx))) {
		goto end;
	}
	cvlen = ptlen + cv->ciphertext_size + cv->mactag_size;

	if (!buf) {
		*buflen = cvlen;
		ret = 1;
		goto end;

	} else if (*buflen < cvlen) {
		goto end;
	}

	if (!(ptlen = EC_POINT_point2oct(ec_group, cv->ephem_point,
		point_form, buf, *buflen, bn_ctx))) {
		goto end;
	}
	buf += ptlen;
	memcpy(buf, cv->ciphertext, cv->ciphertext_size);
	buf += cv->ciphertext_size;
	memcpy(buf, cv->mactag, cv->mactag_size);

	*buflen = cvlen;
	ret = 1;
end:
	if (bn_ctx) BN_CTX_free(bn_ctx);
	return ret;
}

SM2_CIPHERTEXT_VALUE *SM2_CIPHERTEXT_VALUE_decode(const EC_GROUP *ec_group,
	point_conversion_form_t point_form, const EVP_MD *mac_md,
	const unsigned char *buf, size_t buflen)
{
	int ok = 0;
	SM2_CIPHERTEXT_VALUE *ret = NULL;
	BN_CTX *bn_ctx = NULL;
	int len = SM2_CIPHERTEXT_VALUE_size(ec_group, point_form, 0, mac_md);
	int ptlen = len - EVP_MD_size(mac_md);

	if (!(len = SM2_CIPHERTEXT_VALUE_size(ec_group, point_form, 0, mac_md))) {
		goto end;
	}
	if (buflen <= len) {
		goto end;
	}

	if (!(ret = OPENSSL_malloc(sizeof(SM2_CIPHERTEXT_VALUE)))) {
		goto end;
	}

	ret->ephem_point = EC_POINT_new(ec_group);
	ret->ciphertext_size = buflen - len;
	ret->ciphertext = OPENSSL_malloc(ret->ciphertext_size);
	if (!ret->ephem_point || !ret->ciphertext) {
		goto end;
	}
	if (!(bn_ctx = BN_CTX_new())) {
		goto end;
	}
	if (!EC_POINT_oct2point(ec_group, ret->ephem_point, buf, len, bn_ctx)) {
		goto end;
	}
	memcpy(ret->ciphertext, buf + ptlen, ret->ciphertext_size);
	ret->mactag_size = EVP_MD_size(mac_md);
	memcpy(ret->mactag, buf + buflen - ret->mactag_size, ret->mactag_size);

	ok = 1;

end:
	if (!ok && ret) {
		SM2_CIPHERTEXT_VALUE_free(ret);
		ret = NULL;
	}
	if (bn_ctx) BN_CTX_free(bn_ctx);

	return ret;
}

int SM2_CIPHERTEXT_VALUE_print(BIO *out, const SM2_CIPHERTEXT_VALUE *cv,
	int indent, unsigned long flags)
{
	OPENSSL_assert(0);
	return 0;
}

int SM2_encrypt(const EVP_MD *kdf_md, const EVP_MD *mac_md,
	point_conversion_form_t point_form, unsigned char *out, size_t *outlen,
	const unsigned char *in, size_t inlen, EC_KEY *ec_key)
{
	int ret = 0;
	const EC_GROUP *ec_group = EC_KEY_get0_group(ec_key);
	SM2_CIPHERTEXT_VALUE *cv = NULL;
	int len;

	if (!(len = SM2_CIPHERTEXT_VALUE_size(ec_group, point_form, inlen, mac_md))) {
		goto end;
	}

	if (!out) {
		*outlen = (size_t)len;
		return 1;

	} else if (*outlen < (size_t)len) {
		return 0;
	}

	if (!(cv = SM2_do_encrypt(kdf_md, mac_md, in, inlen, ec_key))) {
		goto end;
	}
	if (!SM2_CIPHERTEXT_VALUE_encode(cv, ec_group, point_form, out, outlen)) {
		goto end;
	}
	
	ret = 1;
end:
	if (cv) SM2_CIPHERTEXT_VALUE_free(cv);
	return ret;
}

SM2_CIPHERTEXT_VALUE *SM2_do_encrypt(const EVP_MD *kdf_md, const EVP_MD *mac_md,
	const unsigned char *in, size_t inlen, EC_KEY *ec_key)
{
	int ok = 0;
	SM2_CIPHERTEXT_VALUE *cv = NULL;
	const EC_GROUP *ec_group = EC_KEY_get0_group(ec_key);
	const EC_POINT *pub_key = EC_KEY_get0_public_key(ec_key);
	KDF_FUNC kdf = KDF_get_x9_63(kdf_md);
	EC_POINT *point = NULL;
	BIGNUM *n = NULL;
	BIGNUM *h = NULL;
	BIGNUM *k = NULL;
	BN_CTX *bn_ctx = NULL;
	EVP_MD_CTX *md_ctx = NULL;
	unsigned char buf[(OPENSSL_ECC_MAX_FIELD_BITS + 7)/4 + 1];
	int nbytes;
	size_t len;
	int i;

	if (!ec_group || !pub_key) {
		goto end;
	}
	if (!kdf) {
		goto end;
	}

	/* init ciphertext_value */
	if (!(cv = OPENSSL_malloc(sizeof(SM2_CIPHERTEXT_VALUE)))) {
		goto end;
	}
	bzero(cv, sizeof(SM2_CIPHERTEXT_VALUE));
	cv->ephem_point = EC_POINT_new(ec_group);
	cv->ciphertext = OPENSSL_malloc(inlen);
	cv->ciphertext_size = inlen;
	if (!cv->ephem_point || !cv->ciphertext) {
		goto end;
	}

	point = EC_POINT_new(ec_group);
	n = BN_new();
	h = BN_new();
	k = BN_new();
	bn_ctx = BN_CTX_new();
	md_ctx = EVP_MD_CTX_create();
	if (!point || !n || !h || !k || !bn_ctx || !md_ctx) {
		goto end;
	}

	/* init ec domain parameters */
	if (!EC_GROUP_get_order(ec_group, n, bn_ctx)) {
		goto end;
	}
	if (!EC_GROUP_get_cofactor(ec_group, h, bn_ctx)) {
		goto end;
	}
	nbytes = (EC_GROUP_get_degree(ec_group) + 7) / 8;
	OPENSSL_assert(nbytes == BN_num_bytes(n));

	/* check sm2 curve and md is 256 bits */
	OPENSSL_assert(nbytes == 32);
	OPENSSL_assert(EVP_MD_size(kdf_md) == 32);
	OPENSSL_assert(EVP_MD_size(mac_md) == 32);

	do
	{
		/* A1: rand k in [1, n-1] */
		do {
			BN_rand_range(k, n);
		} while (BN_is_zero(k));
	
		/* A2: C1 = [k]G = (x1, y1) */
		if (!EC_POINT_mul(ec_group, cv->ephem_point, k, NULL, NULL, bn_ctx)) {
			goto end;
		}
		
		/* A3: check [h]P_B != O */
		if (!EC_POINT_mul(ec_group, point, NULL, pub_key, h, bn_ctx)) {
			goto end;
		}
		if (EC_POINT_is_at_infinity(ec_group, point)) {
			goto end;
		}
		
		/* A4: compute ECDH [k]P_B = (x2, y2) */
		if (!EC_POINT_mul(ec_group, point, NULL, pub_key, k, bn_ctx)) {
			goto end;
		}
		if (!(len = EC_POINT_point2oct(ec_group, point,
			POINT_CONVERSION_UNCOMPRESSED, buf, sizeof(buf), bn_ctx))) {
			goto end;
		}
		OPENSSL_assert(len == nbytes * 2 + 1);

		/* A5: t = KDF(x2 || y2, klen) */	
		kdf(buf - 1, len - 1, cv->ciphertext, &cv->ciphertext_size);	
	
		for (i = 0; i < cv->ciphertext_size; i++) {
			if (cv->ciphertext[i]) {
				break;
			}
		}
		if (i == cv->ciphertext_size) {
			continue;
		}

		break;

	} while (1);


	/* A6: C2 = M xor t */
	for (i = 0; i < inlen; i++) {
		cv->ciphertext[i] ^= in[i];
	}

	/* A7: C3 = Hash(x2 || M || y2) */
	if (!EVP_DigestInit_ex(md_ctx, mac_md, NULL)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, buf + 1, nbytes)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, in, inlen)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, buf + 1 + nbytes, nbytes)) {
		goto end;
	}
	if (!EVP_DigestFinal_ex(md_ctx, cv->mactag, &cv->mactag_size)) {
		goto end;
	}

	ok = 1;

end:
	if (!ok && cv) {
		SM2_CIPHERTEXT_VALUE_free(cv);
		cv = NULL;
	}

	if (n) BN_free(n);
	if (h) BN_free(h);
	if (k) BN_free(k);
	if (bn_ctx) BN_CTX_free(bn_ctx);
	if (md_ctx) EVP_MD_CTX_destroy(md_ctx);

	return cv;
}

int SM2_decrypt(const EVP_MD *kdf_md, const EVP_MD *mac_md,
	point_conversion_form_t point_form, const unsigned char *in,
	size_t inlen, unsigned char *out, size_t *outlen, EC_KEY *ec_key)
{
	int ret = 0;
	const EC_GROUP *ec_group = EC_KEY_get0_group(ec_key);
	SM2_CIPHERTEXT_VALUE *cv = NULL;
	int len;

	if (!(len = SM2_CIPHERTEXT_VALUE_size(ec_group, point_form, 0, mac_md))) {
		goto end;
	}
	if (inlen <= len) {
		goto end;
	}

	if (!out) {
		*outlen = inlen - len;
		return 1;
	} else if (outlen < inlen - len) {
		return 0;
	}

	if (!(cv = SM2_CIPHERTEXT_VALUE_decode(ec_group, point_form, mac_md, in, inlen))) {
		goto end;
	}
	if (!SM2_do_decrypt(kdf_md, mac_md, cv, out, outlen, ec_key)) {
		goto end;
	}

	ret = 1;
end:
	if (cv) SM2_CIPHERTEXT_VALUE_free(cv);
	return ret;
}

int SM2_do_decrypt(const EVP_MD *kdf_md, const EVP_MD *mac_md,
	const SM2_CIPHERTEXT_VALUE *cv, unsigned char *out, size_t *outlen,
	EC_KEY *ec_key)
{
	int ret = 0;
	const EC_GROUP *ec_group = EC_KEY_get0_group(ec_key);
	const BIGNUM *pri_key = EC_KEY_get0_private_key(ec_key);
	KDF_FUNC kdf = KDF_get_x9_63(kdf_md);
	EC_POINT *point = NULL;
	BIGNUM *n = NULL;
	BIGNUM *h = NULL;
	BN_CTX *bn_ctx = NULL;
	EVP_MD_CTX *md_ctx = NULL;
	unsigned char buf[(OPENSSL_ECC_MAX_FIELD_BITS + 7)/4 + 1];
	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned int maclen;
	int nbytes;
	size_t size;
	int i;

	if (!ec_group || !pri_key) {
		goto end;
	}
	if (!kdf) {
		goto end;
	}

	if (!out) {
		*outlen = cv->ciphertext_size;
		return 1;
	}
	if (*outlen < cv->ciphertext_size) {
		goto end;
	}

	/* init vars */
	point = EC_POINT_new(ec_group);
	n = BN_new();
	h = BN_new();
	bn_ctx = BN_CTX_new();
	md_ctx = EVP_MD_CTX_create();
	if (!point || !n || !h || !bn_ctx || !md_ctx) {
		goto end;
	}
	
	/* init ec domain parameters */
	if (!EC_GROUP_get_order(ec_group, n, bn_ctx)) {
		goto end;
	}
	if (!EC_GROUP_get_cofactor(ec_group, h, bn_ctx)) {
		goto end;
	}
	nbytes = (EC_GROUP_get_degree(ec_group) + 7) / 8;
	OPENSSL_assert(nbytes == BN_num_bytes(n));

	/* check sm2 curve and md is 256 bits */
	OPENSSL_assert(nbytes == 32);
	OPENSSL_assert(EVP_MD_size(kdf_md) == 32);
	OPENSSL_assert(EVP_MD_size(mac_md) == 32);

	/* B2: check [h]C1 != O */
	if (!EC_POINT_mul(ec_group, point, NULL, cv->ephem_point, h, bn_ctx)) {
		goto end;
	}
	if (EC_POINT_is_at_infinity(ec_group, point)) {
		goto end;
	}

	/* B3: compute ECDH [d]C1 = (x2, y2) */	
	if (!EC_POINT_mul(ec_group, point, NULL, cv->ephem_point, pri_key, bn_ctx)) {
		goto end;
	}
	if (!(size = EC_POINT_point2oct(ec_group, point,
		POINT_CONVERSION_UNCOMPRESSED, buf, sizeof(buf), bn_ctx))) {
		goto end;
	}

	/* B4: compute t = KDF(x2 || y2, clen) */
	kdf(buf - 1, size - 1, out, outlen);


	/* B5: compute M = C2 xor t */
	for (i = 0; i < cv->ciphertext_size; i++) {
		out[i] ^= cv->ciphertext[i];
	}

	/* B6: check Hash(x2 || M || y2) == C3 */
	if (!EVP_DigestInit_ex(md_ctx, mac_md, NULL)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, buf + 1, nbytes)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, out, *outlen)) {
		goto end;
	}
	if (!EVP_DigestUpdate(md_ctx, buf + 1 + nbytes, nbytes)) {
		goto end;
	}
	if (!EVP_DigestFinal_ex(md_ctx, mac, &maclen)) {
		goto end;
	}
	if (cv->mactag_size != maclen ||
		memcmp(cv->mactag, mac, maclen)) {
		goto end;
	}

	ret = 1;
end:
	if (point) EC_POINT_free(point);
	if (n) BN_free(n);	
	if (h) BN_free(h);
	if (bn_ctx) BN_CTX_free(bn_ctx);
	if (md_ctx) EVP_MD_CTX_destroy(md_ctx);

	return ret;
}
