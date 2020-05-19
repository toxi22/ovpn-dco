// SPDX-License-Identifier: GPL-2.0-only
/*  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2020 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "aead.h"
#include "work.h"
#include "crypto.h"
#include "pktid.h"
#include "proto.h"

#include <crypto/aead.h>
#include <linux/skbuff.h>
#include <linux/printk.h>

const struct ovpn_crypto_ops ovpn_aead_ops;

static int ovpn_aead_encap_overhead(const struct ovpn_crypto_key_slot *ks)
{
	return  OVPN_OP_SIZE_V2 +			/* OP header size */
		4 +					/* Packet ID */
		crypto_aead_authsize(ks->u.ae.encrypt);	/* Auth Tag */
}

static int ovpn_aead_encrypt(struct ovpn_crypto_key_slot *ks,
			     struct sk_buff *skb)
{
	const unsigned int tag_size = crypto_aead_authsize(ks->u.ae.encrypt);
	const unsigned int head_size = ovpn_aead_encap_overhead(ks);
	struct scatterlist sg[MAX_SKB_FRAGS + 2];
	DECLARE_CRYPTO_WAIT(wait);
	struct aead_request *req;
	struct sk_buff *trailer;
	u8 iv[NONCE_SIZE];
	int nfrags, ret;
	u32 pktid, op;

	/* Sample AES-GCM head:
	 * 48000001 00000005 7e7046bd 444a7e28 cc6387b1 64a4d6c1 380275a...
	 * [ OP32 ] [seq # ] [             auth tag            ] [ payload ... ]
	 *          [4-byte
	 *          IV head]
	 */

	/* check that there's enough headroom in the skb for packet
	 * encapsulation, after adding network header and encryption overhead
	 */
	if (unlikely(skb_cow_head(skb, OVPN_HEAD_ROOM + head_size)))
		return -ENOBUFS;

	/* get number of skb frags and ensure that packet data is writable */
	nfrags = skb_cow_data(skb, 0, &trailer);
	if (unlikely(nfrags < 0))
		return nfrags;

	if (unlikely(nfrags > ARRAY_SIZE(sg)))
		return -ENOSPC;

	req = aead_request_alloc(ks->u.ae.encrypt, GFP_KERNEL);
	if (unlikely(!req))
		return -ENOMEM;

	/* sg table:
	 * 0: op, wire nonce (AD, len=OVPN_OP_SIZE_V2+NONCE_WIRE_SIZE),
	 * 1, 2, 3, ...: payload,
	 * n: auth_tag (len=tag_size)
	 */
	sg_init_table(sg, nfrags + 2);

	/* build scatterlist to encrypt packet payload */
	ret = skb_to_sgvec_nomark(skb, sg + 1, 0, skb->len);
	if (unlikely(nfrags != ret)) {
		ret = -EINVAL;
		goto free_req;
	}

	/* append auth_tag onto scatterlist */
	__skb_push(skb, tag_size);
	sg_set_buf(sg + nfrags + 1, skb->data, tag_size);

	/* Prepend packet ID.
	 * Nonce containing OpenVPN packet ID is both our IV (NONCE_SIZE)
	 * and tail of our additional data (NONCE_WIRE_SIZE)
	 */
	__skb_push(skb, NONCE_WIRE_SIZE);
	ret = ovpn_pktid_xmit_next(&ks->pid_xmit, &pktid);
	if (unlikely(ret < 0)) {
		if (ret != -1)
			goto free_req;
		//ovpn_notify_pktid_wrap_pc(ks->peer, ks->key_id);
	}

	/* place nonce at the beginning of the packet */
	get_random_bytes(iv, NONCE_SIZE);
	ovpn_pktid_aead_write(pktid, &ks->u.ae.nonce_tail_xmit, iv);
	memcpy(skb->data, iv, NONCE_WIRE_SIZE);

	/* add packet op as head of additional data */
	op = ovpn_op32_compose(OVPN_DATA_V2, ks->key_id, ks->remote_peer_id);
	__skb_push(skb, OVPN_OP_SIZE_V2);
	BUILD_BUG_ON(sizeof(op) != OVPN_OP_SIZE_V2);
	*((__force __be32 *)skb->data) = htonl(op);

	/* AEAD Additional data */
	sg_set_buf(sg, skb->data, OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE);

	/* setup async crypto operation */
	aead_request_set_tfm(req, ks->u.ae.encrypt);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG |
				       CRYPTO_TFM_REQ_MAY_SLEEP,
				  crypto_req_done, &wait);
	aead_request_set_crypt(req, sg, sg, skb->len - head_size, iv);
	aead_request_set_ad(req, OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE);

	/* encrypt it */
	ret = crypto_wait_req(crypto_aead_encrypt(req), &wait);

free_req:
	aead_request_free(req);
	return ret;
}

static int ovpn_aead_decrypt(struct ovpn_crypto_key_slot *ks,
			     struct sk_buff *skb, unsigned int op)
{
	const unsigned int tag_size = crypto_aead_authsize(ks->u.ae.decrypt);
	unsigned int payload_offset, opsize, ad_start;
	const u32 opcode = ovpn_opcode_extract(op);
	struct scatterlist sg[MAX_SKB_FRAGS + 2];
	int ret, payload_len, nfrags;
	u8 *sg_data, iv[NONCE_SIZE];
	DECLARE_CRYPTO_WAIT(wait);
	struct aead_request *req;
	struct sk_buff *trailer;
	unsigned int sg_len;
	__be32 *pid;

	if (likely(opcode == OVPN_DATA_V2)) {
		opsize = OVPN_OP_SIZE_V2;
	} else if (opcode == OVPN_DATA_V1) {
		opsize = OVPN_OP_SIZE_V1;
	} else {
		return -EINVAL;
	}

	payload_offset = opsize + NONCE_WIRE_SIZE + tag_size;
	payload_len = skb->len - payload_offset;

	/* sanity check on packet size, payload size must be >= 0 */
	if (unlikely(payload_len < 0 || !pskb_may_pull(skb, payload_offset)))
		return -EINVAL;

	/* get number of skb frags and ensure that packet data is writable */
	nfrags = skb_cow_data(skb, 0, &trailer);
	if (unlikely(nfrags < 0))
		return nfrags;

	if (unlikely(nfrags > ARRAY_SIZE(sg)))
		return -ENOSPC;

	req = aead_request_alloc(ks->u.ae.decrypt, GFP_KERNEL);
	if (unlikely(!req))
		return -ENOMEM;

	/* sg table:
	 * 0: op, wire nonce (AD, len=OVPN_OP_SIZE_V2+NONCE_WIRE_SIZE),
	 * 1, 2, 3, ...: payload,
	 * n: auth_tag (len=tag_size)
	 */
	sg_init_table(sg, nfrags + 2);

	/* packet op is head of additional data */
	sg_data = skb->data;
	sg_len = OVPN_OP_SIZE_V2 + NONCE_WIRE_SIZE;
	if (unlikely(opcode == OVPN_DATA_V1)) {
		sg_data = skb->data + OVPN_OP_SIZE_V1;
		sg_len = NONCE_WIRE_SIZE;
	}
	sg_set_buf(sg, sg_data, sg_len);

	/* build scatterlist to decrypt packet payload */
	ret = skb_to_sgvec_nomark(skb, sg + 1, payload_offset, payload_len);
	if (unlikely(nfrags != ret)) {
		ret = -EINVAL;
		goto free_req;
	}

	/* append auth_tag onto scatterlist */
	sg_set_buf(sg + nfrags + 1, skb->data + opsize + NONCE_WIRE_SIZE,
		   tag_size);

	/* copy nonce into IV buffer */
	memcpy(iv, skb->data + opsize, NONCE_WIRE_SIZE);
	memcpy(iv + NONCE_WIRE_SIZE, ks->u.ae.nonce_tail_recv.u8,
	       sizeof(struct ovpn_nonce_tail));

	/* setup async crypto operation */
	aead_request_set_tfm(req, ks->u.ae.decrypt);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG |
				       CRYPTO_TFM_REQ_MAY_SLEEP,
				  crypto_req_done, &wait);
	aead_request_set_crypt(req, sg, sg, payload_len + tag_size, iv);

	ad_start = NONCE_WIRE_SIZE;
	if (likely(opcode == OVPN_DATA_V2))
		ad_start += OVPN_OP_SIZE_V2;
	aead_request_set_ad(req, ad_start);

	/* decrypt it */
	ret = crypto_wait_req(crypto_aead_decrypt(req), &wait);
	if (ret < 0)
		goto free_req;

	/* PID sits after the op */
	pid = (__force __be32 *)(skb->data + opsize);
	ret = ovpn_pktid_recv(&ks->pid_recv, ntohl(*pid), 0);
	if (unlikely(ret < 0))
		goto free_req;

	/* point to encapsulated IP packet */
	__skb_pull(skb, payload_offset);

free_req:
	aead_request_free(req);
	return ret;
}

/* Initialize a struct crypto_aead object */
static struct crypto_aead *ovpn_aead_init(const char *title,
					  const char *alg_name,
					  const unsigned char *key,
					  unsigned int keylen)
{
	const unsigned int auth_tag_size = 16;
	struct crypto_aead *aead;
	int ret;

	aead = crypto_alloc_aead(alg_name, 0, 0);
	if (IS_ERR(aead)) {
		ret = PTR_ERR(aead);
		pr_err("%s crypto_alloc_aead failed, err=%d\n", title, ret);
		aead = NULL;
		goto error;
	}

	ret = crypto_aead_setkey(aead, key, keylen);
	if (ret) {
		pr_err("%s crypto_aead_setkey size=%u failed, err=%d\n", title,
		       keylen, ret);
		goto error;
	}

	ret = crypto_aead_setauthsize(aead, auth_tag_size);
	if (ret) {
		pr_err("%s crypto_aead_setauthsize failed, err=%d\n", title,
		       ret);
		goto error;
	}

	/* basic AEAD assumption */
	if (crypto_aead_ivsize(aead) != EXPECTED_IV_SIZE) {
		pr_err("%s IV size must be %d\n", title, EXPECTED_IV_SIZE);
		ret = -EINVAL;
		goto error;
	}

	pr_debug("********* Cipher %s (%s)\n", alg_name, title);
	pr_debug("*** IV size=%u\n", crypto_aead_ivsize(aead));
	pr_debug("*** req size=%u\n", crypto_aead_reqsize(aead));
	pr_debug("*** block size=%u\n", crypto_aead_blocksize(aead));
	pr_debug("*** auth size=%u\n", crypto_aead_authsize(aead));
	pr_debug("*** alignmask=0x%x\n", crypto_aead_alignmask(aead));

	return aead;

error:
	crypto_free_aead(aead);
	return ERR_PTR(ret);
}

static void ovpn_aead_crypto_key_slot_destroy(struct ovpn_crypto_key_slot *ks)
{
	if (!ks)
		return;

	crypto_free_aead(ks->u.ae.encrypt);
	crypto_free_aead(ks->u.ae.decrypt);
	kfree(ks);
}

static struct ovpn_crypto_key_slot *
ovpn_aead_crypto_key_slot_init(enum ovpn_cipher_alg alg,
			       const unsigned char *encrypt_key,
			       unsigned int encrypt_keylen,
			       const unsigned char *decrypt_key,
			       unsigned int decrypt_keylen,
			       const unsigned char *encrypt_nonce_tail,
			       unsigned int encrypt_nonce_tail_len,
			       const unsigned char *decrypt_nonce_tail,
			       unsigned int decrypt_nonce_tail_len,
			       u16 key_id)
{
	struct ovpn_crypto_key_slot *ks = NULL;
	const char *alg_name;
	int ret;

	/* validate crypto alg */
	switch (alg) {
	case OVPN_CIPHER_ALG_AES_GCM:
		alg_name = "gcm(aes)";
		break;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}

	/* build the key slot */
	ks = kmalloc(sizeof(*ks), GFP_KERNEL);
	if (!ks)
		return ERR_PTR(-ENOMEM);

	ks->ops = &ovpn_aead_ops;
	ks->u.ae.encrypt = NULL;
	ks->u.ae.decrypt = NULL;
	kref_init(&ks->refcount);
	ks->key_id = key_id;

	ks->u.ae.encrypt = ovpn_aead_init("encrypt", alg_name, encrypt_key,
					  encrypt_keylen);
	if (IS_ERR(ks->u.ae.encrypt)) {
		ret = PTR_ERR(ks->u.ae.encrypt);
		ks->u.ae.encrypt = NULL;
		goto destroy_ks;
	}

	ks->u.ae.decrypt = ovpn_aead_init("decrypt", alg_name, decrypt_key,
					  decrypt_keylen);
	if (IS_ERR(ks->u.ae.decrypt)) {
		ret = PTR_ERR(ks->u.ae.decrypt);
		ks->u.ae.decrypt = NULL;
		goto destroy_ks;
	}

	if (sizeof(struct ovpn_nonce_tail) != encrypt_nonce_tail_len ||
	    sizeof(struct ovpn_nonce_tail) != decrypt_nonce_tail_len) {
		ret = -EINVAL;
		goto destroy_ks;
	}

	memcpy(ks->u.ae.nonce_tail_xmit.u8, encrypt_nonce_tail,
	       sizeof(struct ovpn_nonce_tail));
	memcpy(ks->u.ae.nonce_tail_recv.u8, decrypt_nonce_tail,
	       sizeof(struct ovpn_nonce_tail));

	/* init packet ID generation/validation */
	ovpn_pktid_xmit_init(&ks->pid_xmit);
	ovpn_pktid_recv_init(&ks->pid_recv);

	return ks;

destroy_ks:
	ovpn_aead_crypto_key_slot_destroy(ks);
	return ERR_PTR(ret);
}

static struct ovpn_crypto_key_slot *
ovpn_aead_crypto_key_slot_new(const struct ovpn_key_config *kc)
{
	return ovpn_aead_crypto_key_slot_init(kc->cipher_alg,
					      kc->encrypt.cipher_key,
					      kc->encrypt.cipher_key_size,
					      kc->decrypt.cipher_key,
					      kc->decrypt.cipher_key_size,
					      kc->encrypt.nonce_tail,
					      kc->encrypt.nonce_tail_size,
					      kc->decrypt.nonce_tail,
					      kc->decrypt.nonce_tail_size,
					      kc->key_id);
}

const struct ovpn_crypto_ops ovpn_aead_ops = {
	.encrypt     = ovpn_aead_encrypt,
	.decrypt     = ovpn_aead_decrypt,
	.new         = ovpn_aead_crypto_key_slot_new,
	.destroy     = ovpn_aead_crypto_key_slot_destroy,
	.encap_overhead = ovpn_aead_encap_overhead,
	.use_hmac    = false,
};
