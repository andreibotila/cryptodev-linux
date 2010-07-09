/*
 * Driver for /dev/crypto device (aka CryptoDev)
 *
 * Copyright (c) 2004 Michal Ludvig <mludvig@logix.net.nz>, SuSE Labs
 * Copyright (c) 2009,2010 Nikos Mavrogiannopoulos <nmav@gnutls.org>
 *
 * This file is part of linux cryptodev.
 *
 * cryptodev is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * cryptodev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Device /dev/crypto provides an interface for 
 * accessing kernel CryptoAPI algorithms (ciphers,
 * hashes) from userspace programs.
 *
 * /dev/crypto interface was originally introduced in
 * OpenBSD and this module attempts to keep the API.
 *
 */

#include <linux/crypto.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/random.h>
#include "cryptodev.h"
#include <asm/uaccess.h>
#include <asm/ioctl.h>
#include <linux/scatterlist.h>
#include "cryptodev_int.h"
#include "ncr_int.h"
#include <linux/version.h>
#include "version.h"

MODULE_AUTHOR("Nikos Mavrogiannopoulos <nmav@gnutls.org>");
MODULE_DESCRIPTION("CryptoDev driver");
MODULE_LICENSE("GPL");

/* ====== Compile-time config ====== */

#define CRYPTODEV_STATS

/* ====== Module parameters ====== */

int cryptodev_verbosity = 0;
module_param(cryptodev_verbosity, int, 0644);
MODULE_PARM_DESC(cryptodev_verbosity, "0: normal, 1: verbose, 2: debug");

#ifdef CRYPTODEV_STATS
static int enable_stats = 0;
module_param(enable_stats, int, 0644);
MODULE_PARM_DESC(enable_stats, "collect statictics about cryptodev usage");
#endif

/* ====== CryptoAPI ====== */
struct fcrypt {
	struct list_head list;
	struct semaphore sem;
};

struct crypt_priv {
	void * ncr;
	struct fcrypt fcrypt;
};

#define FILL_SG(sg,ptr,len)					\
	do {							\
		(sg)->page = virt_to_page(ptr);			\
		(sg)->offset = offset_in_page(ptr);		\
		(sg)->length = len;				\
		(sg)->dma_address = 0;				\
	} while (0)

struct csession {
	struct list_head entry;
	struct semaphore sem;
	struct cipher_data cdata;
	struct hash_data hdata;
	uint32_t sid;
#ifdef CRYPTODEV_STATS
#if ! ((COP_ENCRYPT < 2) && (COP_DECRYPT < 2))
#error Struct csession.stat uses COP_{ENCRYPT,DECRYPT} as indices. Do something!
#endif
	unsigned long long stat[2];
	size_t stat_max_size, stat_count;
#endif
};

/* Prepare session for future use. */
static int
crypto_create_session(struct fcrypt *fcr, struct session_op *sop)
{
	struct csession	*ses_new = NULL, *ses_ptr;
	int ret = 0;
	const char *alg_name=NULL;
	const char *hash_name=NULL;
	int hmac_mode = 1;

	/* Does the request make sense? */
	if (unlikely(!sop->cipher && !sop->mac)) {
		dprintk(1,KERN_DEBUG,"Both 'cipher' and 'mac' unset.\n");
		return -EINVAL;
	}

	switch (sop->cipher) {
		case 0:
			break;
		case CRYPTO_DES_CBC:
			alg_name = "cbc(des)";
			break;
		case CRYPTO_3DES_CBC:
			alg_name = "cbc(des3_ede)";
			break;
		case CRYPTO_BLF_CBC:
			alg_name = "cbc(blowfish)";
			break;
		case CRYPTO_AES_CBC:
			alg_name = "cbc(aes)";
			break;
		case CRYPTO_CAMELLIA_CBC:
			alg_name = "cbc(camelia)";
			break;
		default:
			dprintk(1,KERN_DEBUG,"%s: bad cipher: %d\n", __func__, sop->cipher);
			return -EINVAL;
	}

	switch (sop->mac) {
		case 0:
			break;
		case CRYPTO_MD5_HMAC:
			hash_name = "hmac(md5)";
			break;
		case CRYPTO_RIPEMD160_HMAC:
			hash_name = "hmac(rmd160)";
			break;
		case CRYPTO_SHA1_HMAC:
			hash_name = "hmac(sha1)";
			break;
		case CRYPTO_SHA2_256_HMAC:
			hash_name = "hmac(sha256)";
			break;
		case CRYPTO_SHA2_384_HMAC:
			hash_name = "hmac(sha384)";
			break;
		case CRYPTO_SHA2_512_HMAC:
			hash_name = "hmac(sha512)";
			break;

		/* non-hmac cases */
		case CRYPTO_MD5:
			hash_name = "md5";
			hmac_mode = 0;
			break;
		case CRYPTO_RIPEMD160:
			hash_name = "rmd160";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA1:
			hash_name = "sha1";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_256:
			hash_name = "sha256";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_384:
			hash_name = "sha384";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_512:
			hash_name = "sha512";
			hmac_mode = 0;
			break;

		default:
			dprintk(1,KERN_DEBUG,"%s: bad mac: %d\n", __func__, sop->mac);
			return -EINVAL;
	}

	/* Create a session and put it to the list. */
	ses_new = kmalloc(sizeof(*ses_new), GFP_KERNEL);
	if(!ses_new) {
		return -ENOMEM;
	}

	memset(ses_new, 0, sizeof(*ses_new));

	/* Set-up crypto transform. */
	if (alg_name) {
		uint8_t keyp[CRYPTO_CIPHER_MAX_KEY_LEN];

		if (unlikely(sop->keylen > CRYPTO_CIPHER_MAX_KEY_LEN)) {
			dprintk(1,KERN_DEBUG,"Setting key failed for %s-%zu.\n",
				alg_name, (size_t)sop->keylen*8);
			ret = -EINVAL;
			goto error_cipher;
		}

		ret = copy_from_user(keyp, sop->key, sop->keylen);
		if (unlikely(ret)) {
			goto error_cipher;
		}

		ret = cryptodev_cipher_init(&ses_new->cdata, alg_name, keyp, sop->keylen);
		if (ret < 0) {
			dprintk(1,KERN_DEBUG,"%s: Failed to load cipher for %s\n", __func__,
				   alg_name);
			ret = -EINVAL;
			goto error_cipher;
		}
	}

	if (hash_name) {
		uint8_t keyp[CRYPTO_HMAC_MAX_KEY_LEN];

		if (unlikely(sop->mackeylen > CRYPTO_HMAC_MAX_KEY_LEN)) {
			dprintk(1,KERN_DEBUG,"Setting key failed for %s-%zu.\n",
				alg_name, (size_t)sop->mackeylen*8);
			ret = -EINVAL;
			goto error_hash;
		}
		
		ret = copy_from_user(keyp, sop->mackey, sop->mackeylen);
		if (unlikely(ret)) {
			goto error_hash;
		}

		ret = cryptodev_hash_init(&ses_new->hdata, hash_name, hmac_mode, keyp, sop->mackeylen);
		if (ret != 0) {
			dprintk(1,KERN_DEBUG,"%s: Failed to load hash for %s\n", __func__,
				   hash_name);
			ret = -EINVAL;
			goto error_hash;
		}
	}

	/* put the new session to the list */
	get_random_bytes(&ses_new->sid, sizeof(ses_new->sid));
	init_MUTEX(&ses_new->sem);

	down(&fcr->sem);
restart:
	list_for_each_entry(ses_ptr, &fcr->list, entry) {
		/* Check for duplicate SID */
		if (unlikely(ses_new->sid == ses_ptr->sid)) {
			get_random_bytes(&ses_new->sid, sizeof(ses_new->sid));
			/* Unless we have a broken RNG this 
			   shouldn't loop forever... ;-) */
			goto restart;
		}
	}

	list_add(&ses_new->entry, &fcr->list);
	up(&fcr->sem);

	/* Fill in some values for the user. */
	sop->ses = ses_new->sid;

	return 0;

error_hash:
	cryptodev_cipher_deinit( &ses_new->cdata);
error_cipher:
	if (ses_new) kfree(ses_new);

	return ret;

}

/* Everything that needs to be done when remowing a session. */
static inline void
crypto_destroy_session(struct csession *ses_ptr)
{
	if(down_trylock(&ses_ptr->sem)) {
		dprintk(2, KERN_DEBUG, "Waiting for semaphore of sid=0x%08X\n",
			ses_ptr->sid);
		down(&ses_ptr->sem);
	}
	dprintk(2, KERN_DEBUG, "Removed session 0x%08X\n", ses_ptr->sid);
#if defined(CRYPTODEV_STATS)
	if(enable_stats)
		dprintk(2, KERN_DEBUG,
			"Usage in Bytes: enc=%llu, dec=%llu, max=%zu, avg=%lu, cnt=%zu\n",
			ses_ptr->stat[COP_ENCRYPT], ses_ptr->stat[COP_DECRYPT],
			ses_ptr->stat_max_size, ses_ptr->stat_count > 0
				? ((unsigned long)(ses_ptr->stat[COP_ENCRYPT]+
						   ses_ptr->stat[COP_DECRYPT]) / 
				   ses_ptr->stat_count) : 0,
			ses_ptr->stat_count);
#endif
	cryptodev_cipher_deinit(&ses_ptr->cdata);
	cryptodev_hash_deinit(&ses_ptr->hdata);
	up(&ses_ptr->sem);
	kfree(ses_ptr);
}

/* Look up a session by ID and remove. */
static int
crypto_finish_session(struct fcrypt *fcr, uint32_t sid)
{
	struct csession *tmp, *ses_ptr;
	struct list_head *head;
	int ret = 0;

	down(&fcr->sem);
	head = &fcr->list;
	list_for_each_entry_safe(ses_ptr, tmp, head, entry) {
		if(ses_ptr->sid == sid) {
			list_del(&ses_ptr->entry);
			crypto_destroy_session(ses_ptr);
			break;
		}
	}

	if (unlikely(!ses_ptr)) {
		dprintk(1, KERN_ERR, "Session with sid=0x%08X not found!\n", sid);
		ret = -ENOENT;
	}
	up(&fcr->sem);

	return ret;
}

/* Remove all sessions when closing the file */
static int
crypto_finish_all_sessions(struct fcrypt *fcr)
{
	struct csession *tmp, *ses_ptr;
	struct list_head *head;

	down(&fcr->sem);

	head = &fcr->list;
	list_for_each_entry_safe(ses_ptr, tmp, head, entry) {
		list_del(&ses_ptr->entry);
		crypto_destroy_session(ses_ptr);
	}
	up(&fcr->sem);

	return 0;
}

/* Look up session by session ID. The returned session is locked. */
static struct csession *
crypto_get_session_by_sid(struct fcrypt *fcr, uint32_t sid)
{
	struct csession *ses_ptr;

	down(&fcr->sem);
	list_for_each_entry(ses_ptr, &fcr->list, entry) {
		if(ses_ptr->sid == sid) {
			down(&ses_ptr->sem);
			break;
		}
	}
	up(&fcr->sem);

	return ses_ptr;
}

/* This is the main crypto function - feed it with plaintext 
   and get a ciphertext (or vice versa :-) */
static int
crypto_run(struct fcrypt *fcr, struct crypt_op *cop)
{
	char *data;
	char __user *src, __user *dst;
	struct scatterlist sg;
	struct csession *ses_ptr;
	unsigned int ivsize=0;
	size_t nbytes, bufsize;
	int ret = 0;
	uint8_t hash_output[AALG_MAX_RESULT_LEN];

	if (unlikely(cop->op != COP_ENCRYPT && cop->op != COP_DECRYPT)) {
		dprintk(1, KERN_DEBUG, "invalid operation op=%u\n", cop->op);
		return -EINVAL;
	}

	ses_ptr = crypto_get_session_by_sid(fcr, cop->ses);
	if (unlikely(!ses_ptr)) {
		dprintk(1, KERN_ERR, "invalid session ID=0x%08X\n", cop->ses);
		return -EINVAL;
	}

	nbytes = cop->len;
	data = (char*)__get_free_page(GFP_KERNEL);

	if (unlikely(!data)) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	bufsize = PAGE_SIZE < nbytes ? PAGE_SIZE : nbytes;

	if (ses_ptr->hdata.init != 0) {
		ret = cryptodev_hash_reset(&ses_ptr->hdata);
		if (unlikely(ret)) {
			dprintk(1, KERN_ERR,
				"error in cryptodev_hash_reset()\n");
			goto out;
		}
	}

	if (ses_ptr->cdata.init != 0) {
		int blocksize = ses_ptr->cdata.blocksize;

		if (unlikely(nbytes % blocksize)) {
			dprintk(1, KERN_ERR,
				"data size (%zu) isn't a multiple of block size (%u)\n",
				nbytes, blocksize);
			ret = -EINVAL;
			goto out;
		}

		if (cop->iv) {
			uint8_t iv[EALG_MAX_BLOCK_LEN];

			ivsize = min((int)sizeof(iv), ses_ptr->cdata.ivsize);
			ret = copy_from_user(iv, cop->iv, ivsize);
			if (unlikely(ret))
				goto out;

			cryptodev_cipher_set_iv(&ses_ptr->cdata, iv, ivsize);
		}
	}

	src = cop->src;
	dst = cop->dst;


	while(nbytes > 0) {
		size_t current_len = nbytes > bufsize ? bufsize : nbytes;

		ret = copy_from_user(data, src, current_len);
		if (unlikely(ret))
			goto out;

		sg_init_one(&sg, data, current_len);

		/* Always hash before encryption and after decryption. Maybe
		 * we should introduce a flag to switch... TBD later on.
		 */
		if (cop->op == COP_ENCRYPT) {
			if (ses_ptr->hdata.init != 0) {
				ret = cryptodev_hash_update(&ses_ptr->hdata, &sg, current_len);
				if (unlikely(ret)) {
					dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
					goto out;
				}
			}
			if (ses_ptr->cdata.init != 0) {
				ret = cryptodev_cipher_encrypt( &ses_ptr->cdata, &sg, &sg, current_len);

				if (unlikely(ret)) {
					dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
					goto out;
				}

				ret = copy_to_user(dst, data, current_len);
				if (unlikely(ret))
					goto out;
				dst += current_len;
			}
		} else {
			if (ses_ptr->cdata.init != 0) {
				ret = cryptodev_cipher_decrypt( &ses_ptr->cdata, &sg, &sg, current_len);

				if (unlikely(ret)) {
					dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
					goto out;
				}
			
				ret = copy_to_user(dst, data, current_len);
				if (unlikely(ret))
					goto out;
				dst += current_len;

			}

			if (ses_ptr->hdata.init != 0) {
				ret = cryptodev_hash_update(&ses_ptr->hdata, &sg, current_len);
				if (unlikely(ret)) {
					dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
					goto out;
				}
			}
		}

		nbytes -= current_len;
		src += current_len;
	}

	if (ses_ptr->hdata.init != 0) {
		ret = cryptodev_hash_final(&ses_ptr->hdata, hash_output);
		if (unlikely(ret)) {
			dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
			goto out;
		}

		ret = copy_to_user(cop->mac, hash_output, ses_ptr->hdata.digestsize);
		if (unlikely(ret))
			goto out;
	}

#if defined(CRYPTODEV_STATS)
	if (enable_stats) {
		/* this is safe - we check cop->op at the function entry */
		ses_ptr->stat[cop->op] += cop->len;
		if (ses_ptr->stat_max_size < cop->len)
			ses_ptr->stat_max_size = cop->len;
		ses_ptr->stat_count++;
	}
#endif

out:
	free_page((unsigned long)data);

out_unlock:
	up(&ses_ptr->sem);

	return ret;
}

/* ====== /dev/crypto ====== */

static int
cryptodev_open(struct inode *inode, struct file *filp)
{
	struct crypt_priv *pcr;

	pcr = kmalloc(sizeof(*pcr), GFP_KERNEL);
	if(!pcr)
		return -ENOMEM;

	memset(pcr, 0, sizeof(*pcr));
	init_MUTEX(&pcr->fcrypt.sem);
	INIT_LIST_HEAD(&pcr->fcrypt.list);
	
	pcr->ncr = ncr_init_lists();
	if (pcr->ncr == NULL) {
		kfree(pcr);
		return -ENOMEM;
	}

	filp->private_data = pcr;
	return 0;
}

static int
cryptodev_release(struct inode *inode, struct file *filp)
{
	struct crypt_priv *pcr = filp->private_data;

	if(pcr) {
		crypto_finish_all_sessions(&pcr->fcrypt);
		ncr_deinit_lists(pcr->ncr);
		kfree(pcr);
		filp->private_data = NULL;
	}

	return 0;
}

static int
clonefd(struct file *filp)
{
	struct fdtable *fdt = files_fdtable(current->files);
	int ret;
	ret = get_unused_fd();
	if (ret >= 0) {
			get_file(filp);
			FD_SET(ret, fdt->open_fds);
			fd_install(ret, filp);
	}

	return ret;
}

static int
cryptodev_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	int __user *p = (void __user *)arg;
	struct session_op sop;
	struct crypt_op cop;
	struct crypt_priv *pcr = filp->private_data;
	struct fcrypt * fcr;
	uint32_t ses;
	int ret, fd;

	if (unlikely(!pcr))
		BUG();

	fcr = &pcr->fcrypt;

	switch (cmd) {
		case CIOCASYMFEAT:
			put_user(0, p);
			return 0;
		case CRIOGET:
			fd = clonefd(filp);
			put_user(fd, p);
			return 0;
		case CIOCGSESSION:
			ret = copy_from_user(&sop, (void*)arg, sizeof(sop));
			if (unlikely(ret))
				return ret;

			ret = crypto_create_session(fcr, &sop);
			if (unlikely(ret))
				return ret;
			return copy_to_user((void*)arg, &sop, sizeof(sop));
		case CIOCFSESSION:
			get_user(ses, (uint32_t*)arg);
			ret = crypto_finish_session(fcr, ses);
			return ret;
		case CIOCCRYPT:
			ret = copy_from_user(&cop, (void*)arg, sizeof(cop));
			if (unlikely(ret))
				return ret;

			ret = crypto_run(fcr, &cop);
			if (unlikely(ret))
				return ret;
			return copy_to_user((void*)arg, &cop, sizeof(cop));

		default:
			return ncr_ioctl(pcr->ncr, filp, cmd, arg);
	}
}

/* compatibility code for 32bit userlands */
#ifdef CONFIG_COMPAT

static inline void
compat_to_session_op(struct compat_session_op *compat, struct session_op *sop)
{
	sop->cipher = compat->cipher;
	sop->mac = compat->mac;
	sop->keylen = compat->keylen;

	sop->key       = compat_ptr(compat->key);
	sop->mackeylen = compat->mackeylen;
	sop->mackey    = compat_ptr(compat->mackey);
	sop->ses       = compat->ses;
}

static inline void
session_op_to_compat(struct session_op *sop, struct compat_session_op *compat)
{
	compat->cipher = sop->cipher;
	compat->mac = sop->mac;
	compat->keylen = sop->keylen;

	compat->key       = ptr_to_compat(sop->key);
	compat->mackeylen = sop->mackeylen;
	compat->mackey    = ptr_to_compat(sop->mackey);
	compat->ses       = sop->ses;
}

static inline void
compat_to_crypt_op(struct compat_crypt_op *compat, struct crypt_op *cop)
{
	cop->ses = compat->ses;
	cop->op = compat->op;
	cop->flags = compat->flags;
	cop->len = compat->len;

	cop->src = compat_ptr(compat->src);
	cop->dst = compat_ptr(compat->dst);
	cop->mac = compat_ptr(compat->mac);
	cop->iv  = compat_ptr(compat->iv);
}

static inline void
crypt_op_to_compat(struct crypt_op *cop, struct compat_crypt_op *compat)
{
	compat->ses = cop->ses;
	compat->op = cop->op;
	compat->flags = cop->flags;
	compat->len = cop->len;

	compat->src = ptr_to_compat(cop->src);
	compat->dst = ptr_to_compat(cop->dst);
	compat->mac = ptr_to_compat(cop->mac);
	compat->iv  = ptr_to_compat(cop->iv);
}

static long
cryptodev_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fcrypt *fcr = file->private_data;
	struct session_op sop;
	struct compat_session_op compat_sop;
	struct crypt_op cop;
	struct compat_crypt_op compat_cop;
	int ret;

	if (unlikely(!fcr))
		BUG();

	switch (cmd) {
	case CIOCASYMFEAT:
	case CRIOGET:
	case CIOCFSESSION:
		return cryptodev_ioctl(NULL, file, cmd, arg);

	case COMPAT_CIOCGSESSION:
		ret = copy_from_user(&compat_sop,
				(void *)arg, sizeof(compat_sop));
		compat_to_session_op(&compat_sop, &sop);
		if (unlikely(ret))
			return ret;

		ret = crypto_create_session(fcr, &sop);
		if (unlikely(ret))
			return ret;

		session_op_to_compat(&sop, &compat_sop);
		return copy_to_user((void*)arg,
				&compat_sop, sizeof(compat_sop));

	case COMPAT_CIOCCRYPT:
		ret = copy_from_user(&compat_cop,
				(void*)arg, sizeof(compat_cop));

		compat_to_crypt_op(&compat_cop, &cop);
		if (unlikely(ret))
			return ret;

		ret = crypto_run(fcr, &cop);
		if (unlikely(ret))
			return ret;

		crypt_op_to_compat(&cop, &compat_cop);
		return copy_to_user((void*)arg,
				&compat_cop, sizeof(compat_cop));

	default:
		return -EINVAL;
	}
}

#endif /* CONFIG_COMPAT */

struct file_operations cryptodev_fops = {
	.owner = THIS_MODULE,
	.open = cryptodev_open,
	.release = cryptodev_release,
	.ioctl = cryptodev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = cryptodev_compat_ioctl,
#endif /* CONFIG_COMPAT */
};

struct miscdevice cryptodev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "crypto",
	.fops = &cryptodev_fops,
};

static int
cryptodev_register(void)
{
	int rc;

	ncr_limits_init();
	ncr_master_key_reset();
	
	rc = ncr_pk_queue_init();
	if (unlikely(rc)) {
		ncr_limits_deinit();
		printk(KERN_ERR PFX "initialization of PK workqueue failed\n");
		return rc;
	}

	rc = misc_register (&cryptodev);
	if (unlikely(rc)) {
		ncr_limits_deinit();
		ncr_pk_queue_deinit();
		printk(KERN_ERR PFX "registration of /dev/crypto failed\n");
		return rc;
	}

	return 0;
}

static void
cryptodev_deregister(void)
{
	misc_deregister(&cryptodev);
	ncr_limits_deinit();
	ncr_pk_queue_deinit();
}

/* ====== Module init/exit ====== */
int __init init_cryptodev(void)
{
	int rc;

	rc = cryptodev_register();
	if (unlikely(rc))
		return rc;

	printk(KERN_INFO PFX "driver %s loaded.\n", VERSION);

	return 0;
}

void __exit exit_cryptodev(void)
{
	cryptodev_deregister();
	printk(KERN_INFO PFX "driver unloaded.\n");
}

module_init(init_cryptodev);
module_exit(exit_cryptodev);
