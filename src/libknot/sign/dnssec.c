#include "common/descriptor.h"
#include "common/errcode.h"
#include "sign/bnutils.h"
#include "sign/dnssec.h"
#include "sign/key.h"
#include <assert.h>
#include <openssl/dsa.h>
#include <openssl/opensslconf.h>
#ifndef OPENSSL_NO_ECDSA
#include <openssl/ecdsa.h>
#endif
#include <openssl/evp.h>
#include <openssl/rsa.h>

struct algorithm_functions;
typedef struct algorithm_functions algorithm_functions_t;

//! \brief Algorithm state data.
struct knot_dnssec_algorithm_context {
	const algorithm_functions_t *functions;	//!< Implementation specific.
	EVP_MD_CTX *digest_context;		//!< Digest computation context.
	EVP_PKEY *private_key;			//!< Private key.
};

/*!
 * \brief Algorithm implementation specific functions.
 */
struct algorithm_functions {
	//! \brief Callback: create private key from key parameters.
	int (*create_pkey)(const knot_key_params_t *, EVP_PKEY *);
	//! \brief Callback: get signature size in bytes.
	size_t (*sign_size)(knot_dnssec_key_t *);
	//! \brief Callback: cover supplied data with the signature.
	int (*sign_add)(knot_dnssec_key_t *, const uint8_t *, size_t);
	//! \brief Callback: finish the signing and write out the signature.
	int (*sign_write)(knot_dnssec_key_t *, uint8_t *);
};

/*- Algorithm independent ----------------------------------------------------*/

/*!
 * \brief Get size of the resulting signature.
 *
 * \param key	DNSSEC key.
 *
 * \return Signature size in bytes.
 */
static size_t any_sign_size(knot_dnssec_key_t *key)
{
	assert(key);

	return (size_t)EVP_PKEY_size(key->context->private_key);
}

/*!
 * \brief Add data to be covered by the signature.
 *
 * \param key		DNSSEC key.
 * \param data		Data to be signed.
 * \param data_size	Size of the data to be signed.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int any_sign_add(knot_dnssec_key_t *key,
                        const uint8_t *data, size_t data_size)
{
	assert(key);
	assert(data);

	if (!EVP_SignUpdate(key->context->digest_context, data, data_size))
		return KNOT_DNSSEC_ESIGN;

	return KNOT_EOK;
}

/*!
 * \brief Finish the signing and get the RAW signature.
 *
 * Caller should free the memory returned via signature parameter.
 *
 * \param key		  DNSSEC key.
 * \param signature	  Pointer to signature (output).
 * \param signature_size  Signature size (output).
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int any_sign_finish(knot_dnssec_key_t *key, uint8_t **signature,
			   size_t *signature_size)
{
	assert(key);
	assert(signature);
	assert(signature_size);

	size_t max_size = (size_t)EVP_PKEY_size(key->context->private_key);
	uint8_t *output = calloc(1, max_size);
	if (!output)
		return KNOT_ENOMEM;

	unsigned int actual_size;
	int result = EVP_SignFinal(key->context->digest_context, output,
				   &actual_size, key->context->private_key);
	if (!result) {
		free(output);
		return KNOT_DNSSEC_ESIGN;
	}

	assert(actual_size <= max_size);

	*signature = output;
	*signature_size = actual_size;

	return KNOT_EOK;
}

/*- RSA specific -------------------------------------------------------------*/

/*!
 * \brief Create RSA private key from key parameters.
 *
 * \param params	Key parameters.
 * \param key		Output private key.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int rsa_create_pkey(const knot_key_params_t *params, EVP_PKEY *key)
{
	assert(key);

	RSA *rsa = RSA_new();
	if (rsa == NULL)
		return KNOT_ENOMEM;

	rsa->n    = knot_b64_to_bignum(params->modulus);
	rsa->e    = knot_b64_to_bignum(params->public_exponent);
	rsa->d    = knot_b64_to_bignum(params->private_exponent);
	rsa->p    = knot_b64_to_bignum(params->prime_one);
	rsa->q    = knot_b64_to_bignum(params->prime_two);
	rsa->dmp1 = knot_b64_to_bignum(params->exponent_one);
	rsa->dmq1 = knot_b64_to_bignum(params->exponent_two);
	rsa->iqmp = knot_b64_to_bignum(params->coefficient);

	if (RSA_check_key(rsa) != 1) {
		RSA_free(rsa);
		return KNOT_DNSSEC_EINVALID_KEY;
	}

	if (!EVP_PKEY_assign_RSA(key, rsa)) {
		RSA_free(rsa);
		return KNOT_DNSSEC_EASSIGN_KEY;
	}

	return KNOT_EOK;
}

/*!
 * \brief Finish the signing and write out the RSA signature.
 *
 * \param key		DNSSEC key.
 * \param signature	Pointer to memory where the signature will be written.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int rsa_sign_write(knot_dnssec_key_t *key, uint8_t *signature)
{
	assert(key);
	assert(signature);

	int result;
	uint8_t *raw_signature;
	size_t raw_signature_size;

	result = any_sign_finish(key, &raw_signature, &raw_signature_size);
	if (result != KNOT_EOK) {
		return result;
	}

	if (raw_signature_size != key->context->functions->sign_size(key)) {
		free(raw_signature);
		return KNOT_DNSSEC_EUNEXPECTED_SIGNATURE_SIZE;
	}

	memcpy(signature, raw_signature, raw_signature_size);
	free(raw_signature);

	return KNOT_EOK;
}

/*- DSA specific -------------------------------------------------------------*/

/*!
 * \brief Create DSA private key from key parameters.
 * \see rsa_create_pkey
 */
static int dsa_create_pkey(const knot_key_params_t *params, EVP_PKEY *key)
{
	assert(key);

	DSA *dsa = DSA_new();
	if (dsa == NULL)
		return KNOT_ENOMEM;

	dsa->p        = knot_b64_to_bignum(params->prime);
	dsa->q        = knot_b64_to_bignum(params->subprime);
	dsa->g        = knot_b64_to_bignum(params->base);
	dsa->priv_key = knot_b64_to_bignum(params->private_value);
	dsa->pub_key  = knot_b64_to_bignum(params->public_value);

	if (!EVP_PKEY_assign_DSA(key, dsa)) {
		DSA_free(dsa);
		return KNOT_DNSSEC_EASSIGN_KEY;
	}

	return KNOT_EOK;
}

/*!
 * \brief Get size of the resulting signature for DSA algorithm.
 * \see any_sign_size
 */
static size_t dsa_sign_size(knot_dnssec_key_t *key)
{
	// RFC 2536 (section 3 - DSA SIG Resource Record)
	return 41;
}

/*!
 * \brief Finish the signing and write out the DSA signature.
 * \see rsa_sign_write
 */
static int dsa_sign_write(knot_dnssec_key_t *key, uint8_t *signature)
{
	assert(key);
	assert(signature);

	int result;
	uint8_t *raw_signature;
	size_t raw_signature_size;

	result = any_sign_finish(key, &raw_signature, &raw_signature_size);
	if (result != KNOT_EOK) {
		return result;
	}

	// decode signature, X.509 Dss-Sig-Value (RFC2459)

	DSA_SIG *decoded = DSA_SIG_new();
	if (!decoded) {
		free(raw_signature);
		return KNOT_ENOMEM;
	}

	const uint8_t *decode_scan = raw_signature;
	if (!d2i_DSA_SIG(&decoded, &decode_scan, (long)raw_signature_size)) {
		DSA_SIG_free(decoded);
		free(raw_signature);
		return KNOT_DNSSEC_EDECODE_RAW_SIGNATURE;
	}

	free(raw_signature);

	// convert to format defined by RFC 2536 (DSA keys and SIGs in DNS)

	// T (1 byte), R (20 bytes), S (20 bytes)
	uint8_t *signature_t = signature;
	uint8_t *signature_r = signature + 21 - BN_num_bytes(decoded->r);
	uint8_t *signature_s = signature + 41 - BN_num_bytes(decoded->s);

	*signature_t = 0x00; //! \todo How to compute T? (Only recommended.)
	BN_bn2bin(decoded->r, signature_r);
	BN_bn2bin(decoded->s, signature_s);

	DSA_SIG_free(decoded);

	return KNOT_EOK;
}

/*- EC specific --------------------------------------------------------------*/

#ifndef OPENSSL_NO_ECDSA

/*!
 * \brief Create ECDSA private key from key parameters.
 * \see rsa_create_pkey
 */
static int ecdsa_create_pkey(const knot_key_params_t *params, EVP_PKEY *key)
{
	assert(key);

	int curve;
	if (params->algorithm == KNOT_DNSSEC_ALG_ECDSAP256SHA256) {
		curve = NID_X9_62_prime256v1; // == secp256r1
	} else if (params->algorithm == KNOT_DNSSEC_ALG_ECDSAP384SHA384) {
		curve = NID_secp384r1;
	} else {
		return KNOT_DNSSEC_ENOTSUP;
	}

	EC_KEY *ec_key = EC_KEY_new_by_curve_name(curve);
	if (ec_key == NULL)
		return KNOT_ENOMEM;

	EC_KEY_set_private_key(ec_key, knot_b64_to_bignum(params->private_key));

	// EC_KEY_check_key() could be added, but fails without public key

	if (!EVP_PKEY_assign_EC_KEY(key, ec_key)) {
		EC_KEY_free(ec_key);
		return KNOT_DNSSEC_EASSIGN_KEY;
	}

	return KNOT_EOK;
}

/*!
 * \brief Get size of the resulting signature for ECDSA algorithm.
 * \see any_sign_size
 */
static size_t ecdsa_sign_size(knot_dnssec_key_t *key)
{
	assert(key);

	// RFC 6605 (section 4 - DNSKEY and RRSIG Resource Records for ECDSA)

	switch (key->algorithm) {
	case KNOT_DNSSEC_ALG_ECDSAP256SHA256:
		return 2 * 32;
	case KNOT_DNSSEC_ALG_ECDSAP384SHA384:
		return 2 * 48;
	default:
		assert(0);
		return 0;
	}
}

/*!
 * \brief Finish the signing and write out the ECDSA signature.
 * \see rsa_sign_write
 */
static int ecdsa_sign_write(knot_dnssec_key_t *key, uint8_t *signature)
{
	assert(key);
	assert(signature);

	int result;
	uint8_t *raw_signature;
	size_t raw_signature_size;

	result = any_sign_finish(key, &raw_signature, &raw_signature_size);
	if (result != KNOT_EOK) {
		return result;
	}

	// decode signature

	ECDSA_SIG *decoded = ECDSA_SIG_new();
	if (!decoded) {
		free(raw_signature);
		return KNOT_ENOMEM;
	}

	const uint8_t *decode_scan = raw_signature;
	if (!d2i_ECDSA_SIG(&decoded, &decode_scan, (long)raw_signature_size)) {
		ECDSA_SIG_free(decoded);
		free(raw_signature);
		return KNOT_DNSSEC_EDECODE_RAW_SIGNATURE;
	}

	free(raw_signature);

	// convert to format defined by RFC 6605 (EC DSA for DNSSEC)
	// R and S parameters are encoded in halves of the output signature

	uint8_t *signature_r;
	uint8_t *signature_s;
	size_t param_size = ecdsa_sign_size(key) / 2;

	signature_r = signature + param_size - BN_num_bytes(decoded->r);
	signature_s = signature + 2 * param_size - BN_num_bytes(decoded->s);

	BN_bn2bin(decoded->r, signature_r);
	BN_bn2bin(decoded->s, signature_s);

	ECDSA_SIG_free(decoded);

	return KNOT_EOK;
}

#endif

/*- Algorithm specifications -------------------------------------------------*/

static const algorithm_functions_t rsa_functions = {
	rsa_create_pkey,
	any_sign_size,
	any_sign_add,
	rsa_sign_write
};

static const algorithm_functions_t dsa_functions = {
	dsa_create_pkey,
	dsa_sign_size,
	any_sign_add,
	dsa_sign_write
};

#ifndef OPENSSL_NO_ECDSA
static const algorithm_functions_t ecdsa_functions = {
	ecdsa_create_pkey,
	ecdsa_sign_size,
	any_sign_add,
	ecdsa_sign_write
};
#endif

/*!
 * \brief Get implementation specific callbacks for a given algorithm.
 *
 * \param algorithm	Algorithm number.
 *
 * \return Pointer to structure with functions, NULL if not implemented.
 */
static const algorithm_functions_t *get_implementation(int algorithm)
{
	switch (algorithm) {
	case KNOT_DNSSEC_ALG_RSAMD5:
	case KNOT_DNSSEC_ALG_RSASHA1:
	case KNOT_DNSSEC_ALG_RSASHA1_NSEC3_SHA1:
	case KNOT_DNSSEC_ALG_RSASHA256:
	case KNOT_DNSSEC_ALG_RSASHA512:
		return &rsa_functions;
	case KNOT_DNSSEC_ALG_DSA:
	case KNOT_DNSSEC_ALG_DSA_NSEC3_SHA1:
		return &dsa_functions;
	case KNOT_DNSSEC_ALG_ECDSAP256SHA256:
	case KNOT_DNSSEC_ALG_ECDSAP384SHA384:
#ifndef OPENSSL_NO_ECDSA
		return &ecdsa_functions;
#endif
	default:
		return NULL;
	}
}

/*!
 * \brief Get message digest type for a given algorithm.
 *
 * \param algorithm	Algorithm number.
 *
 * \return Pointer to digest type specification, NULL if not implemented.
 */
static const EVP_MD *get_digest_type(knot_dnssec_algorithm_t algorithm)
{
	// EVP_<digest>() functions should not fail (return NULL)

	switch (algorithm) {
	case KNOT_DNSSEC_ALG_RSASHA1:
	case KNOT_DNSSEC_ALG_RSASHA1_NSEC3_SHA1:
	case KNOT_DNSSEC_ALG_DSA:
	case KNOT_DNSSEC_ALG_DSA_NSEC3_SHA1:
		return EVP_sha1();
	case KNOT_DNSSEC_ALG_RSAMD5:
		return EVP_md5();
	case KNOT_DNSSEC_ALG_RSASHA256:
	case KNOT_DNSSEC_ALG_ECDSAP256SHA256:
		return EVP_sha256();
	case KNOT_DNSSEC_ALG_ECDSAP384SHA384:
		return EVP_sha384();
	case KNOT_DNSSEC_ALG_RSASHA512:
		return EVP_sha512();
	default:
		return NULL;
	}
}

/*- Internal init/clean functions --------------------------------------------*/

/*!
 * \brief Create private key.
 *
 * \param params	Key parameters.
 * \param functions	Algorithm specific callbacks.
 * \param result_key	Output private key.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int create_pkey(const knot_key_params_t *params,
                       const algorithm_functions_t *functions,
                       EVP_PKEY **result_key)
{
        assert(result_key);

	EVP_PKEY *private_key = EVP_PKEY_new();
	if (!private_key)
		return KNOT_ENOMEM;

	int result = functions->create_pkey(params, private_key);
	if (result != KNOT_EOK) {
		EVP_PKEY_free(private_key);
		return result;
	}

	*result_key = private_key;
	return KNOT_EOK;
}

/*!
 * \brief Create message digest context.
 *
 * \param params	  Key parameters.
 * \param result_context  Output message digest context.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int create_digest_context(const knot_key_params_t *params,
				 EVP_MD_CTX **result_context)
{
	assert(result_context);

	const EVP_MD *digest_type = get_digest_type(params->algorithm);
	if (digest_type == NULL)
		return KNOT_DNSSEC_ENOTSUP;

	EVP_MD_CTX *context = EVP_MD_CTX_create();
	if (!context)
		return KNOT_ENOMEM;

	if (!EVP_SignInit_ex(context, digest_type, NULL)) {
		EVP_MD_CTX_destroy(context);
		return KNOT_DNSSEC_ECREATE_DIGEST_CONTEXT;
	}

	*result_context = context;
	return KNOT_EOK;
}

/*!
 * \brief Free algorithm context data.
 *
 * \param context	  Algorithm context.
 *
 * \return Error code, always KNOT_EOK.
 */
static int clean_algorithm_context(knot_dnssec_algorithm_context_t  *context)
{
	assert(context);

	if (context->private_key) {
		EVP_PKEY_free(context->private_key);
		context->private_key = NULL;
	}

	if (context->digest_context) {
		EVP_MD_CTX_destroy(context->digest_context);
		context->digest_context = NULL;
	}

	return KNOT_EOK;
}

/*!
 * \brief Initialize algorithm context.
 *
 * \param params	Key parameters.
 * \param context	Algorithm context to be initialized.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static int init_algorithm_context(const knot_key_params_t *params,
                                  knot_dnssec_algorithm_context_t *context)
{
	assert(params);
	assert(context);

	context->functions = get_implementation(params->algorithm);
	if (!context->functions)
		return KNOT_DNSSEC_ENOTSUP;

	int result = create_digest_context(params, &context->digest_context);
	if (result != KNOT_EOK) {
		clean_algorithm_context(context);
		return result;
	}

	result = create_pkey(params, context->functions, &context->private_key);
	if (result != KNOT_EOK) {
		clean_algorithm_context(context);
		return result;
	}

	return KNOT_EOK;
}

/*- Public init/clean functions ----------------------------------------------*/

/*!
 * \brief Fill DNSSEC key structure according to key parameters.
 */
int knot_dnssec_key_from_params(const knot_key_params_t *params,
				knot_dnssec_key_t *key)
{
	if (!key || !params)
		return KNOT_EINVAL;

	knot_dname_t *name = knot_dname_deep_copy(params->name);
	if (!name)
		return KNOT_ENOMEM;

	knot_dnssec_algorithm_context_t *context;
	context = calloc(1, sizeof(knot_dnssec_algorithm_context_t));
	if (!context) {
		knot_dname_release(name);
		return KNOT_ENOMEM;
	}

	int result = init_algorithm_context(params, context);
	if (result != KNOT_EOK) {
		knot_dname_release(name);
		free(context);
		return result;
	}

	key->name = name;
	key->keytag = params->keytag;
	key->algorithm = params->algorithm;
	key->context = context;

	return KNOT_EOK;
}

/*!
 * \brief Free DNSSEC key structure content.
 */
int knot_dnssec_key_free(knot_dnssec_key_t *key)
{
	if (!key)
		return KNOT_EINVAL;

	if (key->name)
		knot_dname_release(key->name);

	if (key->context) {
		clean_algorithm_context(key->context);
		free(key->context);
	}

	memset(key, '\0', sizeof(knot_dnssec_key_t));

	return KNOT_EOK;
}

/*- Public low level signing interface ---------------------------------------*/

/*!
 * \brief Get DNSSEC signature size.
 */
size_t knot_dnssec_sign_size(knot_dnssec_key_t *key)
{
	if (!key)
		return 0;

	return key->context->functions->sign_size(key);
}

/*!
 * \brief Add data into DNSSEC signature.
 */
int knot_dnssec_sign_add(knot_dnssec_key_t *key, const uint8_t *data,
			 size_t data_size)
{
	if (!key || !data)
		return KNOT_EINVAL;

	return key->context->functions->sign_add(key, data, data_size);
}

/**
 * \brief Finish DNSSEC signing and write out the signature.
 */
int knot_dnssec_sign_write(knot_dnssec_key_t *key, uint8_t *signature)
{
	if (!key || !signature)
		return KNOT_EINVAL;

	return key->context->functions->sign_write(key, signature);
}
