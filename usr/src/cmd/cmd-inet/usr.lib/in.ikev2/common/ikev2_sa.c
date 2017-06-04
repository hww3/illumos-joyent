/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2014 Jason King.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Manipulation and storage of IKEv2 Security Associations (SAs).
 */
#include <umem.h>
#include <pthread.h>
#include <errno.h>
#include <strings.h>
#include <locale.h>
#include <stddef.h>
#include <ipsec_util.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/debug.h>
#include <limits.h>

#include "defs.h"
#include "timer.h"
#include "ikev2_sa.h"

#define	IKEV2_SA_HASH_SPI(spi) \
    P2PHASE_TYPED((spi), num_buckets, uint64_t)
#define	IKEV2_SA_RHASH(ss, spi) \
    P2PHASE_TYPED(i2sa_rhash((ss), (spi)), num_buckets, uint64_t)

/* Our hashes */
enum {
	LSPI,
	RHASH
};

struct i2sa_bucket_s {
	pthread_mutex_t		lock;	/* bucket lock */
	uu_list_t		*chain;	/* hash chain of ikev2_sa_t's */
};

typedef struct i2sa_cmp_arg_s {
	const struct sockaddr	*laddr;
	const struct sockaddr	*raddr;
	const buf_t		*init_pkt;
	uint64_t		lspi;
	uint64_t		rspi;
	int			hash;
} i2sa_cmp_arg_t;

static volatile uint_t	half_open;	/* # of larval/half open IKEv2 SAs */
static uint_t		num_buckets;	/* Use same value for all hashes */
static uint32_t		remote_noise;	/* random noise for rhash */
static i2sa_bucket_t	*hash[I2SA_NUM_HASH];
static uu_list_pool_t	*list_pool[I2SA_NUM_HASH];
static umem_cache_t	*i2sa_cache;


static void	i2sa_init(ikev2_sa_t *);
static uint32_t	i2sa_rhash(const struct sockaddr_storage *, uint64_t);

static ikev2_sa_t *i2sa_verify(ikev2_sa_t *restrict, uint64_t,
    const struct sockaddr_storage *, const struct sockaddr_storage *);
static boolean_t i2sa_add_to_hash(int, ikev2_sa_t *);
static void	i2sa_unlink(ikev2_sa_t *);

static void inc_half_open(void);
static void dec_half_open(void);

/*
 * Attempt to find an IKEv2 SA that matches the given criteria, or return
 * NULL if not found.
 */
ikev2_sa_t *
ikev2_sa_get(uint64_t l_spi, uint64_t r_spi,
    const struct sockaddr_storage *restrict l_addr,
    const struct sockaddr_storage *restrict r_addr,
    const buf_t *restrict init_pkt)
{
	i2sa_bucket_t *bucket;
	ikev2_sa_t *sa;
	i2sa_cmp_arg_t arg = { 0 };

	arg.lspi = l_spi;
	arg.rspi = r_spi;
	arg.init_pkt = init_pkt;
	arg.laddr = l_addr;
	arg.raddr = r_addr;

	if (l_spi != 0) {
		/*
		 * We assign the local SPIs, so if there is one, we should
		 * only need that to find it.
		 */
		bucket = hash[LSPI] + IKEV2_SA_HASH_SPI(l_spi);
		arg.hash = LSPI;
	} else {
		/* Otherwise gotta use the other stuff */
		bucket = hash[RHASH] + IKEV2_SA_RHASH(r_addr, r_spi);
		arg.hash = RHASH;
	}

	VERIFY(pthread_mutex_lock(&bucket->lock) == 0);
	sa = (ikev2_sa_t *)uu_list_find(bucket->chain, NULL, &arg, NULL);
	if (sa != NULL)
		I2SA_REFHOLD(sa);
	VERIFY(pthread_mutex_unlock(&bucket->lock) == 0);

	return (i2sa_verify(sa, r_spi, l_addr, r_addr));
}

/*
 * Allocate a larval IKEv2 SA.
 *
 * Obtains a unique local SPI and assigns it to the SA and adds the SA to
 * the local SPI hash.  If the packet used to trigger the creation of the SA
 * is given, take over management of it.  Also create an SA expiration timer.
 *
 * If we initiated the SA creation, the remote SPI will not be known initially.
 * Once the protocol has proceeded enough to determine the remote SPI,
 * ikev2_sa_set_rspi() should be called.
 *
 * Parameters:
 * 	initiator	Was this SA locally initiated
 * 	init_pkt	The packet that trigged the creation of the SA.
 * 	laddr,
 * 	raddr		The local and remote addresses of this SA.
 *
 * On successful create, the larval IKEv2 SA is returned.
 * On failure, NULL is returned.  Caller maintains responsibility for
 * init_pkt in this instance.
 */
ikev2_sa_t *
ikev2_sa_alloc(boolean_t initiator,
    pkt_t *restrict init_pkt,
    const struct sockaddr_storage *restrict laddr,
    const struct sockaddr_storage *restrict raddr)
{
	ikev2_sa_t	*i2sa = NULL;

	if ((i2sa = umem_cache_alloc(i2sa_cache, UMEM_DEFAULT)) == NULL)
		return (NULL);

	/* Keep anyone else out while we initialize */
	VERIFY(pthread_mutex_lock(&i2sa->lock) == 0);

	ASSERT((init_pkt == NULL) ||
	    (init_pkt->hdr.exch_type == IKEV2_EXCHANGE_IKE_SA_INIT));

	i2sa->flags |= (initiator) ? I2SA_INITIATOR : 0;

	(void) memcpy(&i2sa->laddr, laddr, sizeof (*laddr));
	(void) memcpy(&i2sa->raddr, raddr, sizeof (*raddr));

	/* Get a random number for the local SPI that's currently unusued */
	while (1) {
		/*CONSTCOND*/
		uint64_t spi;

		/* 0 is never valid, exteremely unlikely, but easy to handle */
		if ((spi = random_low_64()) == 0)
			continue;

		if (initiator)
			i2sa->i_spi = spi;
		else
			i2sa->r_spi = spi;

		if (i2sa_add_to_hash(LSPI, i2sa)) {
			ASSERT(i2sa->refcnt == 1);

			/* XXX: refhold i2sa in init_pkt */
			i2sa->init = init_pkt;

			/* refhold for caller */
			I2SA_REFHOLD(i2sa);
			break;
		}
	};

	inc_half_open();

	/*
	 * Start SA expiration timer.
	 * XXX: Should this be reset after we've successfully authenticated?
	 */

	I2SA_REFHOLD(i2sa);	/* for the timer */
	if (!schedule_timeout(TE_SA_EXPIRE, i2sa_expire_cb, i2sa,
	    /* XXX: fixme */ 999 * NANOSEC)) {
		/* XXX: log error */

		/* remove from hashes */
		VERIFY(pthread_mutex_lock(&i2sa->lock) == 0);
		i2sa_unlink(i2sa);
		VERIFY(pthread_mutex_unlock(&i2sa->lock) == 0);

		/* should be free'd once these references are released */
		ASSERT(i2sa->refcnt == 2);
		I2SA_REFRELE(i2sa); /* timer */
		I2SA_REFRELE(i2sa); /* caller */

		return (NULL);
	}

	return (i2sa);
}

/*
 * Invoked when an SA has expired.  REF from timer is passed to this
 * function.
 */
static void
i2sa_expire_cb(void *data)
{
	ikev2_sa_t *i2sa = (ikev2_sa_t *)data;

	/* XXX: todo */
	I2SA_REFRELE(i2sa);
}

void
ikev2_sa_flush(void)
{
	/* TODO: implement me */
}

void
ikev2_sa_condemn(ikev2_sa_t *i2sa)
{
}

void
ikev2_sa_free(ikev2_sa_t *i2sa)
{
	if (i2sa == NULL)
		return;

	ASSERT(i2sa->refcnt == 0);

	/* All unauthenticated IKEv2 SAs are considered larval */
	if (!(i2sa->flags & I2SA_AUTHENTICATED))
		dec_half_open();

        /*
         * XXX: we have potential circular references here
         * as ikev2_pkt_t->sa and i2sa->init,
         * i2sa->last_{resp_sent,sent,recvd} reference each other.
         *
         * We will need to sit and thing about the lifecycles of
         * these packets to make sure when we want this SA to go
         * away for any reason, everything is properly cleaned up.
         *
         * For now, my thought is to punt until after the
         * IKE_SA_INIT and IKE_AUTH exchanges are written, as that
         * will likely help identify the best approach to resolving this.
         */
        pkt_free(i2sa->init);
        pkt_free(i2sa->last_resp_sent);
        pkt_free(i2sa->last_sent);
        pkt_free(i2sa->last_recvd);

#define DESTROY(x, y) pkcs11_destroy_obj(#y, &(x)->y, D_OP)
        DESTROY(i2sa, dh_pubkey);
        DESTROY(i2sa, dh_privkey);
        DESTROY(i2sa, dh_key);
        DESTROY(i2sa, sk_d);
        DESTROY(i2sa, sk_ai);
        DESTROY(i2sa, sk_ar);
        DESTROY(i2sa, sk_ei);
        DESTROY(i2sa, sk_er);
        DESTROY(i2sa, sk_pi);
        DESTROY(i2sa, sk_pr);
#undef  DESTROY

	/* TODO: free child SAs */

        i2sa_init(i2sa);
        umem_cache_free(i2sa_cache, i2sa);
}

void
ikev2_sa_set_hashsize(uint_t numbuckets)
{
	i2sa_bucket_t *old[I2SA_NUM_HASH];
	int i, hashtbl;
	boolean_t startup;

	if (old[LSPI] == NULL)
		startup = B_TRUE;
	else
		startup = B_FALSE;

	/* XXX: suspend threads if !startup */

	/* round up to a power of two if not already */
	if (!ISP2(numbuckets)) {
		ASSERT(sizeof (numbuckets) == 4);

		--numbuckets;
		for (i = 1; i <= 16; i++)
			numbuckets |= (numbuckets >> i);
		++numbuckets;
	}
	VERIFY(ISP2(numbuckets));

	for (i = 0; i < I2SA_NUM_BUCKETS; i++)
		hash[i] = NULL;

	/* Allocate new buckets */
	for (i = 0; i < I2SA_NUM_BUCKETS; i++) {
		hash[i] = calloc(numbuckets, sizeof (i2sa_bucket_t));
		if (hash[i] == NULL)
			goto nomem;
	}

	uint32_t flags = UU_LIST_SORTED;

#ifdef DEBUG
	flags |= UU_LIST_DEBUG;
#endif

	for (hashtbl = 0; hashtbl < I2SA_NUM_BUCKETS; hashtbl++) {
		for (i = 0; i < numbuckets; i++) {
			hash[hashtbl][i].chain =
			    uu_list_create(list_pool[hashtbl][i], NULL, flags);
			if (hash[hashtbl][i].chain == NULL)
				goto nomem;

			VERIFY(pthread_mutex_init(&hash[hashtbl][i].lock,
			    NULL) == 0);
		}
	}

	/* New tables means a new fudge factor.  Pick one randomly. */
	remote_noise = random_low_32();

	i = num_buckets;

	/* Set this so the hash functions work on the new buckets */
	num_buckets = numbuckets;

	if (startup)
		return;

	/*
	 * At this point, we've allocated all the necessary structures, so
	 * we can just move everything over to the new buckets.  Since the
	 * only remaining reference to the old number of buckets here is i,
	 * we work backwards to free each chain, and invert the normal
	 * inner/outer loop order.
	 */
	while (--i >= 0) {
		for (hashtbl = 0; hashtbl < I2SA_NUM_HASH; hashtbl++) {
			uu_list_t *list;
			ikev2_sa_t *i2sa;
			void *cookie;

			cookie = NULL;
			list = hash[hashtbl][i].chain;
			for (i2sa = uu_list_teardown(list, &cookie);
			    i2sa != NULL;
			    i2sa = uu_list_teardown(list, &cookie)) {
				/*
				 * i2sa_add_to_hash() will create a new
				 * ref, so we cannot transfer the ref from
				 * the old hash into the new one, but must
				 * release it instead.
				 */
				VERIFY(i2sa_add_to_hash(hashtbl, i2sa));
				I2SA_REFRELE(i2sa);
			}

			VERIFY(pthread_mutex_destroy(&old[hashtbl][i].lock) ==
			    0);
			uu_list_destroy(old[hashtbl][i].chain);
		}
	}

	for (hashtbl = 0; hashtbl < I2SA_NUM_HASH; hashtbl++)
		free(old[hashtbl]);

	/* XXX: resume threads */
	return;

nomem:
	if (startup)
		EXIT_FATAL("Exiting due to insufficient memory");

	/* XXX: write msg */

	/*
	 * Free what the new stuff we've constructed so far, and put the
	 * old buckets back into place
	 */
	for (hashtbl = 0; hashtbl < I2SA_NUM_HASH; hashtbl++) {
		if (hash[hashtbl] == NULL)
			continue;
		for (i = 0; i < numbuckets; i++) {
			if (hash[hashtbl][i].chain != NULL)
				uu_list_destroy(hash[hashtbl][i].chain);
		}
		free (hash[hashtbl]);
		hash[hashtbl] = old[hashtbl];
	}

	/* XXX: resume threads */
}

/*
 * Set the remote SPI of an IKEv2 SA and add to the rhash
 */
void
ikev2_sa_set_rspi(ikev2_sa_t *i2sa, uint64_t r_spi)
{
	/* better not be set already */
	ASSERT(i2sa->r_spi == 0);

	/* never a valid SPI value */
	ASSERT(r_spi != 0);

	/*
	 * A bit confusing at times, but if we are the initiator of the
	 * SA, the responder (ikev2_sa_t->r_spi) is the remote spi,
	 * otherwise we are the responder, so the remote spi is the
	 * initiator (ikev2_sa_t->i_spi)
	 */
	if (i2sa->flags & I2SA_INITIATOR)
		i2sa->r_spi = r_spi;
	else
		i2sa->i_spi = r_spi;

	VERIFY(i2sa_add_to_hash(RHASH, i2sa));
}

static i2sa_bucket_t *
i2sa_get_bucket(int hashtbl, ikev2_sa_t *i2sa)
{
	i2sa_bucket_t *bucket;

	VERIFY3S(hashtbl, <, I2SA_NUM_HASH);

	bucket = hash[hashtbl];
	switch (hashtbl) {
	case LSPI:
		bucket += IKEV2_SA_HASH_SPI(I2SA_LOCAL_SPI(i2sa));
		break;
	case RHASH:
		bucket += IKEV2_SA_RHASH(&i2sa->raddr, I2SA_REMOTE_SPI(i2sa));
		break;
	}
	return (bucket);
}


/*
 * Add an IKEv2 SA to the given hash.
 *
 * Returns:
 * 	B_TRUE	successfully added, hash holds ref to IKEv2 SA
 * 	B_FALSE	IKEv2 SA already exists in hash, no ref held.
 * 
 */
static boolean_t
i2sa_add_to_hash(int hashtbl, ikev2_sa_t *i2sa)
{
	i2sa_bucket_t	*bucket;
	void		*node;
	i2sa_cmp_arg_t	arg = { 0 };
	uu_list_index_t idx;

	VERIFY3S(hashtbl, <, I2SA_NUM_HASH);

	bucket = i2sa_get_bucket(hashtbl, i2sa);
	VERIFY(pthread_mutex_lock(&bucket->lock) == 0);

	arg.hash = hashtbl;

	/* Set idx to where the SA should be inserted */
	node = uu_list_find(bucket->chain, i2sa, &arg, &idx);
	if (node != NULL) {
		/*
		 * Found a match, should only happen while choosing
		 * a local SPI value and we happen to pick one already
		 * in use.
		 */

		VERIFY(node != i2sa);

		/*
		 * XXX: Should we do anything different for an rhash
		 * match?
		 */
		VERIFY(pthread_mutex_unlock(&bucket->lock) == 0);
		return (B_FALSE);
	}

	I2SA_REFHOLD(i2sa);	/* ref for chain */
	i2sa->bucket[hashtbl] = bucket;
	uu_list_insert(bucket->chain, i2sa, idx);
	VERIFY(pthead_mutex_unlock(&bucket->lock) == 0);

	return (B_TRUE);
}

static ikev2_sa_t *
i2sa_verify(ikev2_sa_t *restrict i2sa, uint64_t rem_spi,
    const struct sockaddr_storage *laddr,
    const struct sockaddr_storage *raddr)
{
	if (i2sa == NULL)
		return (NULL);

	if (rem_spi != 0 && I2SA_REMOTE_SPI(i2sa) != rem_spi) {
		/* XXX: log message */
		goto bad_match;
	}

	if (laddr != NULL && !SA_ADDR_EQ(laddr, &i2sa->laddr)) {
		/* XXX: log message */
		goto bad_match;
	}

	if (raddr != NULL && !SA_ADDR_EQ(raddr, &i2sa->raddr)) {
		/* XXX: log message */
		goto bad_match;
	}

        /*
         * XXX KEBE ASKS - if remote port changes, do remap?
         * Probably have caller do this after packet is really legit.
         */

        /* XXX KEBE SAYS FILL IN OTHER REALITY CHECKS HERE. */

	/* XXX: log full match */
	return (i2sa);

bad_match:
	I2SA_REFRELE(i2sa);
	return (NULL);
}

static void
i2sa_hash_remove(int hashtbl, ikev2_sa_t *i2sa)
{
	i2sa_bucket_t *bucket;

	VERIFY3S(hidx, <, I2SA_NUM_HASH);
	ASSERT(MUTEX_HELD(i2sa));

	/* We shouldn't be holding the lock if this is the last reference */
	ASSERT(i2sa->refcnt > 1);

	bucket = i2sa_get_bucket(hashtbl, i2sa);
	VERIFY(pthread_mutex_lock(&bucket->lock) == 0);
	uu_list_remove(bucket->chain, i2sa);
	i2sa->bucket[hashtbl] = NULL;
	VERIFY(pthread_mutex_unlock(&bucket->lock) == 0);
	I2SA_REFRELE(i2sa);
}

static void
i2sa_unlink(ikev2_sa_t *i2sa)
{
	ASSERT(MUTEX_HELD(i2sa));
	for (int i = 0; i < I2SA_NUM_HASH; i++)
		i2sa_hash_remove(i, i2sa);
}

/*
 * Generate a hash value for a remote SA based off the
 * address and remote SPI.
 */
static uint32_t
i2sa_rhash(const struct sockaddr_storage *ss, uint64_t spi)
{
	uint32_t rc = remote_noise;
	const uint32_t *ptr = (const uint32_t *)&spi;

	rc ^= ptr[0];
	rc ^= ptr[1];

	if (ss->ss_family == AF_INET6) {
		const struct sockaddr_sin6 *s6 =
			(const struct sockaddr_sin6 *)ss;

		ptr = (const uint32_t *)s6->sin6_addr;
		rc ^= ptr[0];
		rc ^= ptr[1];
		rc ^= ptr[2];
		rc ^= ptr[3];
	} else {
		const struct sockaddr_sin *s4 =
			(const struct sockaddr_sin *)ss;

		ASSERT(ss->ss_family == AF_INET);
		rc ^= s4->sin_addr.s_addr;
	}

	return (rc);
}

/*
 * Increase the count of larval SAs.  If we reach our threshold for larval SAs,
 * enable the use of cookies.
 */
static void
inc_half_open(void)
{
	atomic_inc_uint(&half_open);

	/* TODO: cookie check */
}

/*
 * Decrease the count of larval SAs.  Disable cookies if the count falls
 * below the threshold
 */
static void
dec_half_open(void)
{
	atomic_dec_uint(&half_open);

	/*
	 * TODO: Add cookie check.  Include hystersis to avoid potential
	 * flopping.
	 */
}

/*
 * Reset all the fields of an IKEv2 SA.  Used during umem construction, as
 * well as before an SA is returned to the umem cache, per umem requirements.
 */
static void
i2sa_init(ikev2_sa_t *i2sa)
{
	uchar_t *zero_start;
	size_t len;

	zero_start = (uchar_t *)i2sa + I2SA_ZERO_OFFSET;
	(void) memset(zero_start, 0, I2SA_ZERO_LEN);
	i2sa->msgwin = 1;
}

static int
i2sa_ctor(void *buf, void *dummy, int flags)
{
	_NOTE(ARGUNUSUED(dummy, flags))

	ikev2_sa_t *i2sa = (ikev2_sa_t *)&buf;

	VERIFY(pthread_mutex_init(&i2sa->lock, NULL) == 0);
	uu_list_node_init(buf, &i2sa->node_lspi, i2sa_list_pool);
	uu_list_node_fini(buf, &i2sa->node_rhash, i2sa_list_pool);

	i2sa_init(i2sa);
	return (0);
}

static void
i2sa_dtor(void *buf, void *dummy)
{
	_NOTE(ARGUNUSUED(dummy))

	ikev2_sa_t *i2sa = (ikev2_sa_t *)buf;

	VERIFY(pthread_mutex_destroy(&i2sa->lock) == 0);
	uu_list_node_fini(buf, &i2sa->node_lspi, i2sa_list_pool);
	uu_list_node_fini(buf, &i2sa->node_rhash, i2sa_list_pool);
}

static int
sockaddr_compare(const struct sockaddr_storage *restrict l,
    const struct sockaddr_storage *restrict r)
{
	const struct sockaddr_in *l4 = (const struct sockaddr_in *)l;
	const struct sockaddr_in *r4 = (const struct sockaddr_in *)r;
	const struct sockaddr_in6 *l6 = (const struct sockaddr_in6 *)l;
	const struct sockaddr_in6 *r6 = (const struct sockaddr_in6 *)r;
	int cmp;

	if (l->ss_family > r->ss_family)
		return (1);
	if (l->ss_family < r->ss_family)
		return (-1);

	if (l->ss_family == AF_INET) {
		cmp = memcmp(l4->sin_addr, r4->sin_addr, XX);
		if (cmp > 0)
			return (1);
		if (cmp < 0)
			return (-1);

		if (l4->sin_port > r4->sin_port)
			return (1);
		if (l4->sin_port < r4->sin_port)
			return (-1);
		return (0);
	}

	ASSERT(l->ss_family == AF_INET6);

	cmp = memcmp(l6->sin6_addr, r6->sin6_addr, XX);
	if (cmp > 0)
		return (1);
	if (cmp < 0)
		return (-1);

	if (l6->sin6_port > r6->sin6_port)
		return (1);
	if (l6->sin6_port < r6->sin6_port)
		return (-1);
	return (0);
}

/*
 * This is called by uu_list_find() to find an IKEv2 SA.  Since our
 * uu_lists are sorted, it is also used to find the insertion spot
 * in a hash chain.  When finding an IKEv2 SA, rarg will be NULL and
 * arg will have the information to compare.  When finding an insertion
 * spot, rarg will be the IKEv2 SA to insert to determine its location.
 */
static int
i2sa_compare(void *larg, void *rarg, void *arg)
{
	ikev2_sa_t *l = (ikev2_sa_t *)larg;
	ikev2_sa_t *r = (ikev2_sa_t *)rarg;
	i2sa_cmp_arg_t *carg = (i2sa_cmp_arg_t *)arg;
	uint64_t spi;
	const buf_t *lbuf, *rbuf;
	int cmp;

	if (carg->hash == LSPI) {
		ASSERT(MUTEX_HELD(&l->bucket[LSPI]->lock));
		if (r != NULL) {
			ASSERT(l->bucket[LSPI] == r->bucket[LSPI]);
			spi = I2SA_LOCAL_SPI(r);
		} else {
			spi = carg->lspi;
		}

		ASSERT(spi != 0);

		/*
		 * Since we assign the local SPI, we enforce that
		 * they are globally unique
		 */
		if (I2SA_LOCAL_SPI(l) > spi)
			return (1);
		if (I2SA_LOCAL_SPI(l) < spi)
			return (-1);
		return (0);
	}

	ASSERT(carg->hash == RHASH);
	ASSERT(MUTEX_HELD(l->bucket_lock_rhash));

	if (r != NULL) {
		ASSERT(l->bucket_lock_rhash == r->bucket_lock_rhash);
		spi = I2SA_REMOTE_SPI(r);
	} else {
		spi = carg->rspi;
	}

	if (I2SA_REMOTE_SPI(l) > spi)
		return (1);
	if (I2SA_REMOTE_SPI(l) < spi)
		return (-1);

	/* more likely to be different, so check these first */
	cmp = sockaddr_compare(&l->raddr,
	    (r != NULL) ? &r->raddr : carg->raddr);
	if (cmp > 0)
		return (1);
	if (cmp < 0)
		return (-1);

	/* a multihomed system might have different local addresses */
        cmp = sockaddr_compare(&l->laddr,
	    (r != NULL) ? &r->laddr : carg->raddr);
	if (cmp > 0)
		return (1);
	if (cmp < 0)
		return (-1);

	/*
	 * RFC5996 2.1 - We cannot merely rely on the remote SPI and
	 * address as clients behind NATs might choose the same SPI by chance.
	 * We must in addition look at the initial packet.  This is only
	 * an issue for half-opened remotely initiated SAs, as this is the
	 * only time the local SPI is not yet known.
	 */
	if (r != NULL) {
		if (r->init_pkt != NULL)
			rbuf = &r->init_pkt->buf;
		else
			rbuf = NULL;
	} else {
		rbuf = carg->init_pkt;
	}

	if (l->init_pkt != NULL)
		lbuf = &l->init_pkt->buf;
	else
		lbuf = NULL;

	return (buf_cmp(lbuf, rbuf));
}

void
ikev2_sa_init(void)
{
	const char *pool_names[] = {
		"ikev2_lspi_chain",
		"ikev2_rhash_chain",
		NULL
	};
	uint32_t flag = UU_LIST_SORTED;

	VERIFY((i2sa_cache = umem_cache_create("IKEv2 SAs",
	    sizeof (ikev2_sa_t), i2sa_ctor, i2sa_dtor, NULL, NULL, NULL,
	    0)) != NULL);

#ifdef DEBUG
	flag |= UU_LIST_POOL_DEBUG;
#endif

	for (int i = 0; i < I2SA_NUM_HASH; i++) {
		list_pool[i] = uu_list_pool_create(pool_names[i],
		    sizeof (ikev2_sa_t), i2sa_ctor, i2sa_dtor, NULL, NULL,
		    NULL, 0);
		if (list_pool[i] != NULL)
			EXIT_FATAL("Unable to allocate IKEv2 SA hash chains");
	}
}

void
ikev2_sa_fini(void)
{
	umem_cache_destroy(i2sa_cache);
	for (int i = 0; i < I2SA_NUM_HASH; i++)
		uu_list_pool_destroy(list_pool[i]);
}

