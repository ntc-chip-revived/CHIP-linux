/*
 * Copyright (c) International Business Machines Corp., 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Author: Artem Bityutskiy (Битюцкий Артём)
 */

/*
 * The UBI Eraseblock Association (EBA) sub-system.
 *
 * This sub-system is responsible for I/O to/from logical eraseblock.
 *
 * Although in this implementation the EBA table is fully kept and managed in
 * RAM, which assumes poor scalability, it might be (partially) maintained on
 * flash in future implementations.
 *
 * The EBA sub-system implements per-logical eraseblock locking. Before
 * accessing a logical eraseblock it is locked for reading or writing. The
 * per-logical eraseblock locking is implemented by means of the lock tree. The
 * lock tree is an RB-tree which refers all the currently locked logical
 * eraseblocks. The lock tree elements are &struct ubi_ltree_entry objects.
 * They are indexed by (@vol_id, @lnum) pairs.
 *
 * EBA also maintains the global sequence counter which is incremented each
 * time a logical eraseblock is mapped to a physical eraseblock and it is
 * stored in the volume identifier header. This means that each VID header has
 * a unique sequence number. The sequence number is only increased an we assume
 * 64 bits is enough to never overflow.
 */

#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/err.h>
#include "ubi.h"

struct ubi_consolidated_peb {
	int pnum;
	int lnums[];
};

struct ubi_eba_desc {
	int pnum;
};

/**
 * struct ubi_eba_cdesc - UBI erase block association desc used with MLC
 *			  safe volumes
 * @base: base fields
 * @node: list element to queue the LEB in the different LEB state lists
 */
struct ubi_eba_cdesc {
	struct list_head node;
	union {
		int pnum;
		struct ubi_consolidated_peb *cpeb;
	};
};

/**
 * struct ubi_eba_table - UBI eraseblock association table
 * @descs: array of descriptors (one entry for each available LEB)
 * @consolidated: bitmap encoding whether a LEB is consolidated or not
 * @hot: list of hot LEBs. Contains X elements, where X is the number of
 *	 minimum number of non-consolidated LEBs. Used to implement an
 *	 LRU mechanism to avoid consolidating LEBs that are regularly
 *	 updated/unmapped/mapped.
 * @cooling: when an element is evicted from the hot list it is placed in the
 *	     cooling list to avoid unnecessary consolidations. Those LEBs can
 *	     still be consolidated under pressure (when the UBI user needs to
 *	     manipulate an already consolidated PEB).
 * @cold: bitmap referencing all cold LEBs that are not already frozen. These
 *	  LEBs are candidates for consolidation.
 * @frozen: bitmap referencing all consolidated LEBs that are part of PEBs
 *	    containing only valid LEBs.
 */
struct ubi_eba_table {
	union {
		struct ubi_eba_desc *descs;
		struct ubi_eba_cdesc *cdescs;
	};
	unsigned long *consolidated;
	struct list_head open;
	struct {
		struct list_head clean;
		struct list_head *dirty;
	} closed;
	int free_pebs;
};

/* Number of physical eraseblocks reserved for atomic LEB change operation */
#define EBA_RESERVED_PEBS 1

static int cdesc_to_lnum(struct ubi_volume *vol, struct ubi_eba_cdesc *cdesc)
{
	struct ubi_eba_table *tbl = vol->eba_tbl;
	unsigned long idx = (unsigned long)cdesc - (unsigned long)tbl->cdescs;

	idx /= sizeof(*cdesc);

	ubi_assert(idx < vol->avail_lebs);

	return idx;
}

/**
 * next_sqnum - get next sequence number.
 * @ubi: UBI device description object
 *
 * This function returns next sequence number to use, which is just the current
 * global sequence counter value. It also increases the global sequence
 * counter.
 */
unsigned long long ubi_next_sqnum(struct ubi_device *ubi)
{
	unsigned long long sqnum;

	spin_lock(&ubi->ltree_lock);
	sqnum = ubi->global_sqnum++;
	spin_unlock(&ubi->ltree_lock);

	return sqnum;
}

/**
 * ubi_get_compat - get compatibility flags of a volume.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 *
 * This function returns compatibility flags for an internal volume. User
 * volumes have no compatibility flags, so %0 is returned.
 */
static int ubi_get_compat(const struct ubi_device *ubi, int vol_id)
{
	if (vol_id == UBI_LAYOUT_VOLUME_ID)
		return UBI_LAYOUT_VOLUME_COMPAT;
	return 0;
}

/**
 * ltree_lookup - look up the lock tree.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function returns a pointer to the corresponding &struct ubi_ltree_entry
 * object if the logical eraseblock is locked and %NULL if it is not.
 * @ubi->ltree_lock has to be locked.
 */
static struct ubi_ltree_entry *ltree_lookup(struct ubi_device *ubi, int vol_id,
					    int lnum)
{
	struct rb_node *p;

	p = ubi->ltree.rb_node;
	while (p) {
		struct ubi_ltree_entry *le;

		le = rb_entry(p, struct ubi_ltree_entry, rb);

		if (vol_id < le->vol_id)
			p = p->rb_left;
		else if (vol_id > le->vol_id)
			p = p->rb_right;
		else {
			if (lnum < le->lnum)
				p = p->rb_left;
			else if (lnum > le->lnum)
				p = p->rb_right;
			else
				return le;
		}
	}

	return NULL;
}

/**
 * ltree_add_entry - add new entry to the lock tree.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function adds new entry for logical eraseblock (@vol_id, @lnum) to the
 * lock tree. If such entry is already there, its usage counter is increased.
 * Returns pointer to the lock tree entry or %-ENOMEM if memory allocation
 * failed.
 */
static struct ubi_ltree_entry *ltree_add_entry(struct ubi_device *ubi,
					       int vol_id, int lnum)
{
	struct ubi_ltree_entry *le, *le1, *le_free;

	le = kmalloc(sizeof(struct ubi_ltree_entry), GFP_NOFS);
	if (!le)
		return ERR_PTR(-ENOMEM);

	le->users = 0;
	init_rwsem(&le->mutex);
	le->vol_id = vol_id;
	le->lnum = lnum;

	spin_lock(&ubi->ltree_lock);
	le1 = ltree_lookup(ubi, vol_id, lnum);

	if (le1) {
		/*
		 * This logical eraseblock is already locked. The newly
		 * allocated lock entry is not needed.
		 */
		le_free = le;
		le = le1;
	} else {
		struct rb_node **p, *parent = NULL;

		/*
		 * No lock entry, add the newly allocated one to the
		 * @ubi->ltree RB-tree.
		 */
		le_free = NULL;

		p = &ubi->ltree.rb_node;
		while (*p) {
			parent = *p;
			le1 = rb_entry(parent, struct ubi_ltree_entry, rb);

			if (vol_id < le1->vol_id)
				p = &(*p)->rb_left;
			else if (vol_id > le1->vol_id)
				p = &(*p)->rb_right;
			else {
				ubi_assert(lnum != le1->lnum);
				if (lnum < le1->lnum)
					p = &(*p)->rb_left;
				else
					p = &(*p)->rb_right;
			}
		}

		rb_link_node(&le->rb, parent, p);
		rb_insert_color(&le->rb, &ubi->ltree);
	}
	le->users += 1;
	spin_unlock(&ubi->ltree_lock);

	kfree(le_free);
	return le;
}

/**
 * leb_read_lock - lock logical eraseblock for reading.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function locks a logical eraseblock for reading. Returns zero in case
 * of success and a negative error code in case of failure.
 */
static int leb_read_lock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	le = ltree_add_entry(ubi, vol_id, lnum);
	if (IS_ERR(le))
		return PTR_ERR(le);
	down_read(&le->mutex);
	return 0;
}

/**
 * leb_read_try_lock - try to lock logical eraseblock for reading.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function locks a logical eraseblock for writing if there is no
 * contention and does nothing if there is contention. Returns %0 in case of
 * success, %1 in case of contention, and and a negative error code in case of
 * failure.
 */
static int leb_read_trylock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	le = ltree_add_entry(ubi, vol_id, lnum);
	if (IS_ERR(le))
		return PTR_ERR(le);
	if (down_read_trylock(&le->mutex))
		return 0;

	/* Contention, cancel */
	spin_lock(&ubi->ltree_lock);
	le->users -= 1;
	ubi_assert(le->users >= 0);
	if (le->users == 0) {
		rb_erase(&le->rb, &ubi->ltree);
		kfree(le);
	}
	spin_unlock(&ubi->ltree_lock);

	return 1;
}

/**
 * leb_read_unlock - unlock logical eraseblock.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 */
static void leb_read_unlock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	spin_lock(&ubi->ltree_lock);
	le = ltree_lookup(ubi, vol_id, lnum);
	le->users -= 1;
	ubi_assert(le->users >= 0);
	up_read(&le->mutex);
	if (le->users == 0) {
		rb_erase(&le->rb, &ubi->ltree);
		kfree(le);
	}
	spin_unlock(&ubi->ltree_lock);
}

/**
 * leb_write_lock - lock logical eraseblock for writing.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function locks a logical eraseblock for writing. Returns zero in case
 * of success and a negative error code in case of failure.
 */
static int leb_write_lock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	le = ltree_add_entry(ubi, vol_id, lnum);
	if (IS_ERR(le))
		return PTR_ERR(le);
	down_write(&le->mutex);
	return 0;
}

/**
 * leb_write_lock - lock logical eraseblock for writing.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 *
 * This function locks a logical eraseblock for writing if there is no
 * contention and does nothing if there is contention. Returns %0 in case of
 * success, %1 in case of contention, and and a negative error code in case of
 * failure.
 */
static int leb_write_trylock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	le = ltree_add_entry(ubi, vol_id, lnum);
	if (IS_ERR(le))
		return PTR_ERR(le);
	if (down_write_trylock(&le->mutex))
		return 0;

	/* Contention, cancel */
	spin_lock(&ubi->ltree_lock);
	le->users -= 1;
	ubi_assert(le->users >= 0);
	if (le->users == 0) {
		rb_erase(&le->rb, &ubi->ltree);
		kfree(le);
	}
	spin_unlock(&ubi->ltree_lock);

	return 1;
}

/**
 * leb_write_unlock - unlock logical eraseblock.
 * @ubi: UBI device description object
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 */
static void leb_write_unlock(struct ubi_device *ubi, int vol_id, int lnum)
{
	struct ubi_ltree_entry *le;

	spin_lock(&ubi->ltree_lock);
	le = ltree_lookup(ubi, vol_id, lnum);
	le->users -= 1;
	ubi_assert(le->users >= 0);
	up_write(&le->mutex);
	if (le->users == 0) {
		rb_erase(&le->rb, &ubi->ltree);
		kfree(le);
	}
	spin_unlock(&ubi->ltree_lock);
}

/* Must be called with the eba_lock held. */
static void stop_leb_consolidation(struct ubi_volume *vol,
				   const struct ubi_leb_desc *ldesc)
{
	struct ubi_consolidated_peb *cpeb;
	int i, lebs_per_cpeb;

	/* No consolidation running. */
	if (vol->consolidation.ldesc.lpos < 0)
		return;

	lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);
	cpeb = vol->consolidation.cpeb;

	/* Make sure the LEB is not being consolidated. */
	for (i = 0; i < lebs_per_cpeb; i++) {
		if (cpeb->lnums[i] == UBI_LEB_UNMAPPED)
			break;
		else if (cpeb->lnums[i] != ldesc->lnum)
			continue;

		/* Cancel the consolidation. */
		vol->consolidation.cancel = true;
		break;
	}
}

/* Must be called with eba_lock held. */
bool ubi_eba_invalidate_leb_locked(struct ubi_volume *vol,
				   struct ubi_leb_desc *ldesc,
				   bool consolidating)
{
	bool release_peb = true;
	int lnum = ldesc->lnum;

	if (!vol->mlc_safe) {
		vol->eba_tbl->descs[lnum].pnum = UBI_LEB_UNMAPPED;
	} else if (ldesc->lpos < 0) {
		/* The LEB is not consolidated. */
		if (ldesc->pnum != UBI_LEB_UNMAPPED) {
			list_del_init(&vol->eba_tbl->cdescs[lnum].node);
			if (!consolidating)
				stop_leb_consolidation(vol, ldesc);
			vol->eba_tbl->cdescs[lnum].pnum = UBI_LEB_UNMAPPED;
		}
	} else {
		int lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);
		struct ubi_consolidated_peb *cpeb =
					vol->eba_tbl->cdescs[lnum].cpeb;
		struct list_head *dirty = NULL;
		int i, valid = 0;

		/*
		 * Remove the first valid LEB from it's classification list
		 * (the other entries of a consolidated PEBs are not
		 * classified).
		 */
		for (i = 0; i < lebs_per_cpeb; i++) {
			struct ubi_eba_cdesc *cdesc;

			if (cpeb->lnums[i] >= 0) {
				cdesc = &vol->eba_tbl->cdescs[cpeb->lnums[i]];
				list_del(&cdesc->node);
				break;
			}
		}

		/* Invalidate the LEB pointed by ldesc. */
		for (i = 0; i < lebs_per_cpeb; i++) {
			if (cpeb->lnums[i] == lnum)
				cpeb->lnums[i] = UBI_LEB_UNMAPPED;
			else if (cpeb->lnums[i] >= 0)
				valid++;
		}

		/*
		 * We have several dirty lists. The dirty list is selected
		 * based on the number of valid LEBs present in the
		 * consolidated PEB. This allows better selection of
		 * consolidable LEBs (for example, on TLC NANDs you might
		 * prefer to first pick LEBs that are alone in their PEB
		 * to generate more PEBs, or combine LEBs from 2 different
		 * dirty lists to always produce at least 2 free PEBs at
		 * each consolidation step.
		 */
		if (valid)
			dirty = &vol->eba_tbl->closed.dirty[valid - 1];

		/*
		 * Re-insert the first valid LEB in the appropriate dirty
		 * list.
		 */
		for (i = 0; dirty && i < lebs_per_cpeb; i++) {
			struct ubi_eba_cdesc *cdesc;

			if (cpeb->lnums[i] >= 0) {
				cdesc = &vol->eba_tbl->cdescs[cpeb->lnums[i]];
				list_add(&cdesc->node, dirty);
				break;
			}
		}

		if (!consolidating)
			stop_leb_consolidation(vol, ldesc);

		clear_bit(lnum, vol->eba_tbl->consolidated);
		vol->eba_tbl->cdescs[lnum].pnum = UBI_LEB_UNMAPPED;

		if (!valid)
			kfree(cpeb);
		else
			release_peb = false;
	}

	if (release_peb)
		vol->eba_tbl->free_pebs++;

	return release_peb;
}

static bool ubi_eba_invalidate_leb(struct ubi_volume *vol,
				   struct ubi_leb_desc *ldesc)
{
	bool release_peb;

	mutex_lock(&vol->eba_lock);
	release_peb = ubi_eba_invalidate_leb_locked(vol, ldesc, false);
	mutex_lock(&vol->eba_lock);

	return release_peb;
}

/* TODO: Get rid of ubi_eba_get_pnum() and ubi_eba_set_pnum() */
static int ubi_eba_get_pnum(struct ubi_volume *vol, int lnum)
{
	int pnum;

	if (!vol->mlc_safe)
		pnum = vol->eba_tbl->cdescs[lnum].pnum;
	else if (test_bit(lnum, vol->eba_tbl->consolidated))
		pnum = vol->eba_tbl->cdescs[lnum].cpeb->pnum;
	else
		pnum = vol->eba_tbl->descs[lnum].pnum;

	return pnum;
}

static void ubi_eba_set_pnum(struct ubi_volume *vol, int lnum, int pnum)
{
	if (!vol->mlc_safe)
		vol->eba_tbl->descs[lnum].pnum = pnum;
	else
		vol->eba_tbl->cdescs[lnum].pnum = pnum;
}

static int ubi_eba_put_peb(struct ubi_volume *vol, int lnum, int pnum,
			    int torture)
{
	int err;

	err = ubi_wl_put_peb(vol->ubi, vol->vol_id, lnum, pnum, torture);
	if (err)
		return err;

	mutex_lock(&vol->eba_lock);
	vol->eba_tbl->free_pebs++;
	mutex_unlock(&vol->eba_lock);

	return 0;
}

static int ubi_eba_get_peb(struct ubi_volume *vol)
{
	/*
	 * TODO: check number of free PEBs, force/wait for consolidation
	 * if there's not enough, otherwise ask WL layer for a free PEB.
	 */

	mutex_lock(&vol->eba_lock);
	while (vol->eba_tbl->free_pebs < 1) {
		mutex_unlock(&vol->eba_lock);
		mutex_lock(&vol->eba_lock);
	}
	vol->eba_tbl->free_pebs--;
	mutex_unlock(&vol->eba_lock);
	ubi_assert(vol->eba_tbl->free_pebs > 0);

	return ubi_wl_get_peb(vol->ubi);
}

/**
 * ubi_eba_unmap_leb - un-map logical eraseblock.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 *
 * This function un-maps logical eraseblock @lnum and schedules corresponding
 * physical eraseblock for erasure. Returns zero in case of success and a
 * negative error code in case of failure.
 */
int ubi_eba_unmap_leb(struct ubi_device *ubi, struct ubi_volume *vol,
		      int lnum)
{
	int err, vol_id = vol->vol_id;
	struct ubi_leb_desc ldesc;
	bool release_peb;

	if (ubi->ro_mode)
		return -EROFS;

	err = leb_write_lock(ubi, vol_id, lnum);
	if (err)
		return err;

	ubi_eba_get_ldesc(vol, lnum, &ldesc);

	if (ldesc.pnum < 0)
		/* This logical eraseblock is already unmapped */
		goto out_unlock;

	dbg_eba("invalidate LEB %d:%d", vol_id, lnum);
	down_read(&ubi->fm_eba_sem);
	release_peb = ubi_eba_invalidate_leb(vol, &ldesc);
	up_read(&ubi->fm_eba_sem);

	if (release_peb) {
		dbg_eba("release PEB %d after LEB %d:%d invalidation",
			ldesc.pnum, vol_id, lnum);
		err = ubi_eba_put_peb(vol, lnum, ldesc.pnum, 0);
	}

out_unlock:
	leb_write_unlock(ubi, vol_id, lnum);
	return err;
}

static int read_leb(struct ubi_volume *vol, void *buf,
		    const struct ubi_leb_desc *ldesc, int loffset, int len)
{
	struct ubi_device *ubi = vol->ubi;
	int offset = loffset + ubi->leb_start;
	int lpos = vol->mlc_safe ? ldesc->lpos : 0;

	if (lpos < 0)
		return ubi_io_slc_read(ubi, buf, ldesc->pnum, offset, len);

	offset += lpos * vol->leb_size;

	return ubi_io_read(ubi, buf, ldesc->pnum, offset, len);
}

static int write_leb(struct ubi_volume *vol, const void *buf,
		     const struct ubi_leb_desc *ldesc, int loffset, int len)
{
	struct ubi_device *ubi = vol->ubi;
	int offset = loffset + ubi->leb_start;
	int lpos = vol->mlc_safe ? ldesc->lpos : 0;

	if (lpos < 0)
		return ubi_io_slc_write(ubi, buf, ldesc->pnum, offset, len);

	offset += lpos * vol->leb_size;

	return ubi_io_write(ubi, buf, ldesc->pnum, offset, len);
}

static void leb_updated(struct ubi_volume *vol, struct ubi_leb_desc *ldesc)
{
	struct ubi_eba_cdesc *cdesc;
	int lnum = ldesc->lnum;

	ubi_assert(ldesc->lpos < 0);

	if (!vol->eba_tbl->consolidated)
		return;

	/* Put the LEB at the beginning of the used list. */
	mutex_lock(&vol->eba_lock);
	cdesc = &vol->eba_tbl->cdescs[lnum];
	if (!list_empty(&cdesc->node))
		list_del(&vol->eba_tbl->cdescs[lnum].node);

	stop_leb_consolidation(vol, ldesc);

	list_add(&vol->eba_tbl->cdescs[lnum].node, &vol->eba_tbl->open);
	mutex_unlock(&vol->eba_lock);
}

/**
 * ubi_eba_read_leb - read data.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 * @buf: buffer to store the read data
 * @offset: offset from where to read
 * @len: how many bytes to read
 * @check: data CRC check flag
 *
 * If the logical eraseblock @lnum is unmapped, @buf is filled with 0xFF
 * bytes. The @check flag only makes sense for static volumes and forces
 * eraseblock data CRC checking.
 *
 * In case of success this function returns zero. In case of a static volume,
 * if data CRC mismatches - %-EBADMSG is returned. %-EBADMSG may also be
 * returned for any volume type if an ECC error was detected by the MTD device
 * driver. Other negative error cored may be returned in case of other errors.
 */
int ubi_eba_read_leb(struct ubi_device *ubi, struct ubi_volume *vol, int lnum,
		     void *buf, int offset, int len, int check)
{
	int err, scrub = 0, vol_id = vol->vol_id;
	struct ubi_vid_hdr *vid_hdrs = NULL, *vid_hdr;
	uint32_t uninitialized_var(crc);
	struct ubi_leb_desc ldesc;

	err = leb_read_lock(ubi, vol_id, lnum);
	if (err)
		return err;

	ubi_eba_get_ldesc(vol, lnum, &ldesc);

	if (ldesc.pnum < 0) {
		/*
		 * The logical eraseblock is not mapped, fill the whole buffer
		 * with 0xFF bytes. The exception is static volumes for which
		 * it is an error to read unmapped logical eraseblocks.
		 */
		dbg_eba("read %d bytes from offset %d of LEB %d:%d (unmapped)",
			len, offset, vol_id, lnum);
		leb_read_unlock(ubi, vol_id, lnum);
		ubi_assert(vol->vol_type != UBI_STATIC_VOLUME);
		memset(buf, 0xFF, len);
		return 0;
	}

	dbg_eba("read %d bytes from offset %d of LEB %d:%d, PEB %d",
		len, offset, vol_id, lnum, ldesc.pnum);

	if (vol->vol_type == UBI_DYNAMIC_VOLUME)
		check = 0;

retry:
	if (check) {
		int nhdrs = mtd_pairing_groups_per_eb(ubi->mtd);

		vid_hdrs = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
		if (!vid_hdrs) {
			err = -ENOMEM;
			goto out_unlock;
		}

		err = ubi_io_read_vid_hdrs(ubi, ldesc.pnum, vid_hdrs, &nhdrs,
					   1);
		if (err && err != UBI_IO_BITFLIPS) {
			if (err > 0) {
				/*
				 * The header is either absent or corrupted.
				 * The former case means there is a bug -
				 * switch to read-only mode just in case.
				 * The latter case means a real corruption - we
				 * may try to recover data. FIXME: but this is
				 * not implemented.
				 */
				if (err == UBI_IO_BAD_HDR_EBADMSG ||
				    err == UBI_IO_BAD_HDR) {
					ubi_warn(ubi, "corrupted VID header at PEB %d, LEB %d:%d",
						 ldesc.pnum, vol_id, lnum);
					err = -EBADMSG;
				} else {
					/*
					 * Ending up here in the non-Fastmap case
					 * is a clear bug as the VID header had to
					 * be present at scan time to have it referenced.
					 * With fastmap the story is more complicated.
					 * Fastmap has the mapping info without the need
					 * of a full scan. So the LEB could have been
					 * unmapped, Fastmap cannot know this and keeps
					 * the LEB referenced.
					 * This is valid and works as the layer above UBI
					 * has to do bookkeeping about used/referenced
					 * LEBs in any case.
					 */
					if (ubi->fast_attach) {
						err = -EBADMSG;
					} else {
						err = -EINVAL;
						ubi_ro_mode(ubi);
					}
				}
			}
			goto out_free;
		} else if (err == UBI_IO_BITFLIPS)
			scrub = 1;

		if (ldesc.lpos < 0)
			vid_hdr = vid_hdrs;
		else
			vid_hdr = &vid_hdrs[ldesc.lpos];

		ubi_assert(lnum < be32_to_cpu(vid_hdr->used_ebs));
		ubi_assert(len == be32_to_cpu(vid_hdr->data_size));

		crc = be32_to_cpu(vid_hdr->data_crc);
		ubi_free_vid_hdr(ubi, vid_hdr);
	}

	err = read_leb(vol, buf, &ldesc, offset, len);
	if (err) {
		if (err == UBI_IO_BITFLIPS)
			scrub = 1;
		else if (mtd_is_eccerr(err)) {
			if (vol->vol_type == UBI_DYNAMIC_VOLUME)
				goto out_unlock;
			scrub = 1;
			if (!check) {
				ubi_msg(ubi, "force data checking");
				check = 1;
				goto retry;
			}
		} else
			goto out_unlock;
	}

	if (check) {
		uint32_t crc1 = crc32(UBI_CRC32_INIT, buf, len);
		if (crc1 != crc) {
			ubi_warn(ubi, "CRC error: calculated %#08x, must be %#08x",
				 crc1, crc);
			err = -EBADMSG;
			goto out_unlock;
		}
	}

	if (scrub)
		err = ubi_wl_scrub_peb(ubi, ldesc.pnum);

	leb_read_unlock(ubi, vol_id, lnum);
	return err;

out_free:
	ubi_free_vid_hdr(ubi, vid_hdrs);
out_unlock:
	leb_read_unlock(ubi, vol_id, lnum);
	return err;
}

/**
 * ubi_eba_read_leb_sg - read data into a scatter gather list.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 * @sgl: UBI scatter gather list to store the read data
 * @offset: offset from where to read
 * @len: how many bytes to read
 * @check: data CRC check flag
 *
 * This function works exactly like ubi_eba_read_leb(). But instead of
 * storing the read data into a buffer it writes to an UBI scatter gather
 * list.
 */
int ubi_eba_read_leb_sg(struct ubi_device *ubi, struct ubi_volume *vol,
			struct ubi_sgl *sgl, int lnum, int offset, int len,
			int check)
{
	int to_read;
	int ret;
	struct scatterlist *sg;

	for (;;) {
		ubi_assert(sgl->list_pos < UBI_MAX_SG_COUNT);
		sg = &sgl->sg[sgl->list_pos];
		if (len < sg->length - sgl->page_pos)
			to_read = len;
		else
			to_read = sg->length - sgl->page_pos;

		ret = ubi_eba_read_leb(ubi, vol, lnum,
				       sg_virt(sg) + sgl->page_pos, offset,
				       to_read, check);
		if (ret < 0)
			return ret;

		offset += to_read;
		len -= to_read;
		if (!len) {
			sgl->page_pos += to_read;
			if (sgl->page_pos == sg->length) {
				sgl->list_pos++;
				sgl->page_pos = 0;
			}

			break;
		}

		sgl->list_pos++;
		sgl->page_pos = 0;
	}

	return ret;
}

/**
 * recover_peb - recover from write failure.
 * @ubi: UBI device description object
 * @pnum: the physical eraseblock to recover
 * @vol_id: volume ID
 * @lnum: logical eraseblock number
 * @buf: data which was not written because of the write failure
 * @offset: offset of the failed write
 * @len: how many bytes should have been written
 *
 * This function is called in case of a write failure and moves all good data
 * from the potentially bad physical eraseblock to a good physical eraseblock.
 * This function also writes the data which was not written due to the failure.
 * Returns new physical eraseblock number in case of success, and a negative
 * error code in case of failure.
 */
static int recover_peb(struct ubi_volume *vol, struct ubi_leb_desc *ldesc,
		       int lnum, const void *buf, int offset, int len)
{
	struct ubi_device *ubi = vol->ubi;
	int err, vol_id = vol->vol_id;
	int new_pnum, old_pnum, data_size, tries = 0;
	struct ubi_vid_hdr *vid_hdr;

	vid_hdr = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
	if (!vid_hdr)
		return -ENOMEM;

	old_pnum = ldesc->pnum;

retry:
	/*
	 * We do not use the ubi_eba_get_peb() helper here because we know
	 * another one will be released at some point.
	 */
	new_pnum = ubi_wl_get_peb(ubi);
	if (new_pnum < 0) {
		ubi_free_vid_hdr(ubi, vid_hdr);
		up_read(&ubi->fm_eba_sem);
		return new_pnum;
	}

	ubi_msg(ubi, "recover PEB %d, move data to PEB %d",
		ldesc->pnum, new_pnum);

	err = ubi_io_read_vid_hdr(ubi, ldesc->pnum, vid_hdr, 1);
	if (err && err != UBI_IO_BITFLIPS) {
		if (err > 0)
			err = -EIO;
		up_read(&ubi->fm_eba_sem);
		goto out_put;
	}

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	err = ubi_io_write_vid_hdr(ubi, new_pnum, vid_hdr);
	if (err) {
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	data_size = offset + len;
	mutex_lock(&ubi->buf_mutex);
	memset(ubi->peb_buf + offset, 0xFF, len);

	/* Read everything before the area where the write failure happened */
	if (offset > 0) {
		err = read_leb(vol, ubi->peb_buf, ldesc, 0, offset);
		if (err && err != UBI_IO_BITFLIPS) {
			up_read(&ubi->fm_eba_sem);
			goto out_unlock;
		}
	}

	memcpy(ubi->peb_buf + offset, buf, len);

	ldesc->pnum = new_pnum;
	err = write_leb(vol, ubi->peb_buf, ldesc, 0, data_size);
	if (err) {
		mutex_unlock(&ubi->buf_mutex);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	mutex_unlock(&ubi->buf_mutex);
	ubi_free_vid_hdr(ubi, vid_hdr);

	ubi_eba_set_pnum(vol, lnum, new_pnum);
	up_read(&ubi->fm_eba_sem);
	ubi_wl_put_peb(ubi, vol_id, lnum, old_pnum, 1);

	ubi_msg(ubi, "data was successfully recovered");
	return 0;

out_unlock:
	mutex_unlock(&ubi->buf_mutex);
out_put:
	ubi_wl_put_peb(ubi, vol_id, lnum, new_pnum, 1);
	ubi_free_vid_hdr(ubi, vid_hdr);
	return err;

write_error:
	/*
	 * Bad luck? This physical eraseblock is bad too? Crud. Let's try to
	 * get another one.
	 */
	ubi_warn(ubi, "failed to write to PEB %d", new_pnum);

	/* Restore old pnum in the LEB descriptor. */
	ldesc->pnum = old_pnum;

	ubi_wl_put_peb(ubi, vol_id, lnum, new_pnum, 1);
	if (++tries > UBI_IO_RETRIES) {
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}
	ubi_msg(ubi, "try again");
	goto retry;
}

/**
 * Must be called the LEB lock held in write mode.
 */
static int unconsolidate_leb(struct ubi_volume *vol,
			     struct ubi_leb_desc *ldesc, int len)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_vid_hdr *vid_hdr = ubi->peb_buf + ubi->vid_hdr_offset;
	void *buf = ubi->peb_buf + ubi->leb_start;
	int pnum, err, vol_id = vol->vol_id, lnum = ldesc->lnum;
	u32 crc;

	if (ldesc->lpos < 0 || !len)
		return 0;

	/* Get a new PEB. */
	pnum = ubi_eba_get_peb(vol);
	if (pnum < 0)
		return pnum;

	/* First read the existing data */
	mutex_lock(&ubi->buf_mutex);
	memset(ubi->peb_buf, 0, ubi->leb_start);
	err = read_leb(vol, buf, ldesc, 0, len);
	if (err)
		goto err_unlock_buf;

	/* Initialize the VID header */
	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	vid_hdr->vol_id = cpu_to_be32(vol_id);
	vid_hdr->lnum = cpu_to_be32(lnum);
	vid_hdr->compat = ubi_get_compat(ubi, vol_id);
	vid_hdr->data_pad = cpu_to_be32(vol->data_pad);

	crc = crc32(UBI_CRC32_INIT, buf, len);
	vid_hdr->vol_type = UBI_VID_DYNAMIC;
	vid_hdr->data_size = cpu_to_be32(len);
	vid_hdr->copy_flag = 1;
	vid_hdr->data_crc = cpu_to_be32(crc);

	err = ubi_io_write_vid_hdr(ubi, pnum, vid_hdr);
	if (err)
		goto err_unlock_buf;

	err = ubi_io_slc_write(ubi, buf, pnum, ubi->leb_start, len);
	if (err)
		goto err_unlock_buf;

	mutex_unlock(&ubi->buf_mutex);

	/* Release the PEB if we were the last user. */
	if (ubi_eba_invalidate_leb(vol, ldesc))
		ubi_eba_put_peb(vol, lnum, ldesc->pnum, 0);

	/* Update the EBA entry and LEB descriptor. */
	down_read(&ubi->fm_eba_sem);
	vol->eba_tbl->cdescs[lnum].pnum = pnum;
	up_read(&ubi->fm_eba_sem);
	ldesc->pnum = pnum;
	ldesc->lpos = -1;

	return 0;

err_unlock_buf:
	mutex_unlock(&ubi->buf_mutex);
	ubi_eba_put_peb(vol, lnum, pnum, 0);

	return err;
}

/**
 * ubi_eba_write_leb - write data to dynamic volume.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 * @buf: the data to write
 * @offset: offset within the logical eraseblock where to write
 * @len: how many bytes to write
 *
 * This function writes data to logical eraseblock @lnum of a dynamic volume
 * @vol. Returns zero in case of success and a negative error code in case
 * of failure. In case of error, it is possible that something was still
 * written to the flash media, but may be some garbage.
 */
int ubi_eba_write_leb(struct ubi_device *ubi, struct ubi_volume *vol, int lnum,
		      const void *buf, int offset, int len)
{
	int err, tries = 0, vol_id = vol->vol_id;
	struct ubi_vid_hdr *vid_hdr;
	struct ubi_leb_desc ldesc;

	if (ubi->ro_mode)
		return -EROFS;

	err = leb_write_lock(ubi, vol_id, lnum);
	if (err)
		return err;

	ubi_eba_get_ldesc(vol, lnum, &ldesc);

	/* Make sure we un-consolidate the LEB if needed. */
	err = unconsolidate_leb(vol, &ldesc, len);
	if (err) {
		leb_write_unlock(ubi, vol_id, lnum);
		return err;
	}

	if (ldesc.pnum >= 0) {
		dbg_eba("write %d bytes at offset %d of LEB %d:%d, PEB %d",
			len, offset, vol_id, lnum, ldesc.pnum);

		err = write_leb(vol, buf, &ldesc, offset, len);
		if (err) {
			ubi_warn(ubi, "failed to write data to PEB %d",
				 ldesc.pnum);
			if (err == -EIO && ubi->bad_allowed)
				err = recover_peb(vol, &ldesc, lnum, buf,
						  offset, len);
			if (err)
				ubi_ro_mode(ubi);
		}
		leb_write_unlock(ubi, vol_id, lnum);
		return err;
	}

	/*
	 * The logical eraseblock is not mapped. We have to get a free physical
	 * eraseblock and write the volume identifier header there first.
	 */
	vid_hdr = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
	if (!vid_hdr) {
		leb_write_unlock(ubi, vol_id, lnum);
		return -ENOMEM;
	}

	vid_hdr->vol_type = UBI_VID_DYNAMIC;
	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	vid_hdr->vol_id = cpu_to_be32(vol_id);
	vid_hdr->lnum = cpu_to_be32(lnum);
	vid_hdr->compat = ubi_get_compat(ubi, vol_id);
	vid_hdr->data_pad = cpu_to_be32(vol->data_pad);

retry:
	ldesc.pnum = ubi_eba_get_peb(vol);
	if (ldesc.pnum < 0) {
		ubi_free_vid_hdr(ubi, vid_hdr);
		leb_write_unlock(ubi, vol_id, lnum);
		up_read(&ubi->fm_eba_sem);
		return ldesc.pnum;
	}

	dbg_eba("write VID hdr and %d bytes at offset %d of LEB %d:%d, PEB %d",
		len, offset, vol_id, lnum, ldesc.pnum);

	err = ubi_io_write_vid_hdr(ubi, ldesc.pnum, vid_hdr);
	if (err) {
		ubi_warn(ubi, "failed to write VID header to LEB %d:%d, PEB %d",
			 vol_id, lnum, ldesc.pnum);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	if (len) {
		err = write_leb(vol, buf, &ldesc, offset, len);
		if (err) {
			ubi_warn(ubi, "failed to write %d bytes at offset %d of LEB %d:%d, PEB %d",
				 len, offset, vol_id, lnum, ldesc.pnum);
			up_read(&ubi->fm_eba_sem);
			goto write_error;
		}
	}

	/* TODO: hide this set_pnum operation in an high-level helper? */
	ubi_eba_set_pnum(vol, lnum, ldesc.pnum);

	/* LEB has been updated, put it at the beginning of the used list. */
	leb_updated(vol, &ldesc);

	up_read(&ubi->fm_eba_sem);

	leb_write_unlock(ubi, vol_id, lnum);
	ubi_free_vid_hdr(ubi, vid_hdr);
	return 0;

write_error:
	if (err != -EIO || !ubi->bad_allowed) {
		ubi_ro_mode(ubi);
		leb_write_unlock(ubi, vol_id, lnum);
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}

	/*
	 * Fortunately, this is the first write operation to this physical
	 * eraseblock, so just put it and request a new one. We assume that if
	 * this physical eraseblock went bad, the erase code will handle that.
	 */
	err = ubi_eba_put_peb(vol, lnum, ldesc.pnum, 1);
	if (err || ++tries > UBI_IO_RETRIES) {
		ubi_ro_mode(ubi);
		leb_write_unlock(ubi, vol_id, lnum);
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	ubi_msg(ubi, "try another PEB");
	goto retry;
}

/**
 * ubi_eba_write_leb_st - write data to static volume.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 * @buf: data to write
 * @len: how many bytes to write
 * @used_ebs: how many logical eraseblocks will this volume contain
 *
 * This function writes data to logical eraseblock @lnum of static volume
 * @vol. The @used_ebs argument should contain total number of logical
 * eraseblock in this static volume.
 *
 * When writing to the last logical eraseblock, the @len argument doesn't have
 * to be aligned to the minimal I/O unit size. Instead, it has to be equivalent
 * to the real data size, although the @buf buffer has to contain the
 * alignment. In all other cases, @len has to be aligned.
 *
 * It is prohibited to write more than once to logical eraseblocks of static
 * volumes. This function returns zero in case of success and a negative error
 * code in case of failure.
 */
int ubi_eba_write_leb_st(struct ubi_device *ubi, struct ubi_volume *vol,
			 int lnum, const void *buf, int len, int used_ebs)
{
	int err, pnum, tries = 0, data_size = len, vol_id = vol->vol_id;
	struct ubi_vid_hdr *vid_hdr;
	uint32_t crc;

	if (ubi->ro_mode)
		return -EROFS;

	if (lnum == used_ebs - 1)
		/* If this is the last LEB @len may be unaligned */
		len = ALIGN(data_size, ubi->min_io_size);
	else
		ubi_assert(!(len & (ubi->min_io_size - 1)));

	vid_hdr = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
	if (!vid_hdr)
		return -ENOMEM;

	err = leb_write_lock(ubi, vol_id, lnum);
	if (err) {
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	vid_hdr->vol_id = cpu_to_be32(vol_id);
	vid_hdr->lnum = cpu_to_be32(lnum);
	vid_hdr->compat = ubi_get_compat(ubi, vol_id);
	vid_hdr->data_pad = cpu_to_be32(vol->data_pad);

	crc = crc32(UBI_CRC32_INIT, buf, data_size);
	vid_hdr->vol_type = UBI_VID_STATIC;
	vid_hdr->data_size = cpu_to_be32(data_size);
	vid_hdr->used_ebs = cpu_to_be32(used_ebs);
	vid_hdr->data_crc = cpu_to_be32(crc);

retry:
	pnum = ubi_wl_get_peb(ubi);
	if (pnum < 0) {
		ubi_free_vid_hdr(ubi, vid_hdr);
		leb_write_unlock(ubi, vol_id, lnum);
		up_read(&ubi->fm_eba_sem);
		return pnum;
	}

	dbg_eba("write VID hdr and %d bytes at LEB %d:%d, PEB %d, used_ebs %d",
		len, vol_id, lnum, pnum, used_ebs);

	err = ubi_io_write_vid_hdr(ubi, pnum, vid_hdr);
	if (err) {
		ubi_warn(ubi, "failed to write VID header to LEB %d:%d, PEB %d",
			 vol_id, lnum, pnum);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	err = ubi_io_write_data(ubi, buf, pnum, 0, len);
	if (err) {
		ubi_warn(ubi, "failed to write %d bytes of data to PEB %d",
			 len, pnum);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	ubi_assert(ubi_eba_get_pnum(vol, lnum) < 0);
	ubi_eba_set_pnum(vol, lnum, pnum);
	up_read(&ubi->fm_eba_sem);

	leb_write_unlock(ubi, vol_id, lnum);
	ubi_free_vid_hdr(ubi, vid_hdr);
	return 0;

write_error:
	if (err != -EIO || !ubi->bad_allowed) {
		/*
		 * This flash device does not admit of bad eraseblocks or
		 * something nasty and unexpected happened. Switch to read-only
		 * mode just in case.
		 */
		ubi_ro_mode(ubi);
		leb_write_unlock(ubi, vol_id, lnum);
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}

	err = ubi_eba_put_peb(vol, lnum, pnum, 1);
	if (err || ++tries > UBI_IO_RETRIES) {
		ubi_ro_mode(ubi);
		leb_write_unlock(ubi, vol_id, lnum);
		ubi_free_vid_hdr(ubi, vid_hdr);
		return err;
	}

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	ubi_msg(ubi, "try another PEB");
	goto retry;
}

/*
 * ubi_eba_atomic_leb_change - change logical eraseblock atomically.
 * @ubi: UBI device description object
 * @vol: volume description object
 * @lnum: logical eraseblock number
 * @buf: data to write
 * @len: how many bytes to write
 *
 * This function changes the contents of a logical eraseblock atomically. @buf
 * has to contain new logical eraseblock data, and @len - the length of the
 * data, which has to be aligned. This function guarantees that in case of an
 * unclean reboot the old contents is preserved. Returns zero in case of
 * success and a negative error code in case of failure.
 *
 * UBI reserves one LEB for the "atomic LEB change" operation, so only one
 * LEB change may be done at a time. This is ensured by @ubi->alc_mutex.
 */
int ubi_eba_atomic_leb_change(struct ubi_device *ubi, struct ubi_volume *vol,
			      int lnum, const void *buf, int len)
{
	int err, tries = 0, vol_id = vol->vol_id;
	struct ubi_vid_hdr *vid_hdr;
	struct ubi_leb_desc ldesc, oldesc;
	uint32_t crc;

	if (ubi->ro_mode)
		return -EROFS;

	if (len == 0) {
		/*
		 * Special case when data length is zero. In this case the LEB
		 * has to be unmapped and mapped somewhere else.
		 */
		err = ubi_eba_unmap_leb(ubi, vol, lnum);
		if (err)
			return err;
		return ubi_eba_write_leb(ubi, vol, lnum, NULL, 0, 0);
	}

	vid_hdr = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
	if (!vid_hdr)
		return -ENOMEM;

	mutex_lock(&ubi->alc_mutex);
	err = leb_write_lock(ubi, vol_id, lnum);
	if (err)
		goto out_mutex;

	ubi_eba_get_ldesc(vol, lnum, &oldesc);
	ldesc.lpos = -1;
	ldesc.lnum = lnum;

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	vid_hdr->vol_id = cpu_to_be32(vol_id);
	vid_hdr->lnum = cpu_to_be32(lnum);
	vid_hdr->compat = ubi_get_compat(ubi, vol_id);
	vid_hdr->data_pad = cpu_to_be32(vol->data_pad);

	crc = crc32(UBI_CRC32_INIT, buf, len);
	vid_hdr->vol_type = UBI_VID_DYNAMIC;
	vid_hdr->data_size = cpu_to_be32(len);
	vid_hdr->copy_flag = 1;
	vid_hdr->data_crc = cpu_to_be32(crc);

retry:
	ldesc.pnum = ubi_eba_get_peb(vol);
	if (ldesc.pnum < 0) {
		err = ldesc.pnum;
		up_read(&ubi->fm_eba_sem);
		goto out_leb_unlock;
	}


	dbg_eba("change LEB %d:%d, PEB %d, write VID hdr to PEB %d",
		vol_id, oldesc.lnum, oldesc.pnum, ldesc.pnum);

	err = ubi_io_write_vid_hdr(ubi, ldesc.pnum, vid_hdr);
	if (err) {
		ubi_warn(ubi, "failed to write VID header to LEB %d:%d, PEB %d",
			 vol_id, lnum, ldesc.pnum);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	err = write_leb(vol, buf, &ldesc, 0, len);
	if (err) {
		ubi_warn(ubi, "failed to write %d bytes of data to PEB %d",
			 len, ldesc.pnum);
		up_read(&ubi->fm_eba_sem);
		goto write_error;
	}

	ubi_eba_set_pnum(vol, lnum, ldesc.pnum);

	/* LEB has been updated, put it at the beginning of the used list. */
	leb_updated(vol, &ldesc);
	up_read(&ubi->fm_eba_sem);

	if (oldesc.pnum >= 0) {
		err = ubi_eba_put_peb(vol, lnum, oldesc.pnum, 0);
		if (err)
			goto out_leb_unlock;
	}

out_leb_unlock:
	leb_write_unlock(ubi, vol_id, lnum);
out_mutex:
	mutex_unlock(&ubi->alc_mutex);
	ubi_free_vid_hdr(ubi, vid_hdr);
	return err;

write_error:
	if (err != -EIO || !ubi->bad_allowed) {
		/*
		 * This flash device does not admit of bad eraseblocks or
		 * something nasty and unexpected happened. Switch to read-only
		 * mode just in case.
		 */
		ubi_ro_mode(ubi);
		goto out_leb_unlock;
	}

	err = ubi_eba_put_peb(vol, lnum, ldesc.pnum, 1);
	if (err || ++tries > UBI_IO_RETRIES) {
		ubi_ro_mode(ubi);
		goto out_leb_unlock;
	}

	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	ubi_msg(ubi, "try another PEB");
	goto retry;
}

/**
 * is_error_sane - check whether a read error is sane.
 * @err: code of the error happened during reading
 *
 * This is a helper function for 'ubi_eba_copy_leb()' which is called when we
 * cannot read data from the target PEB (an error @err happened). If the error
 * code is sane, then we treat this error as non-fatal. Otherwise the error is
 * fatal and UBI will be switched to R/O mode later.
 *
 * The idea is that we try not to switch to R/O mode if the read error is
 * something which suggests there was a real read problem. E.g., %-EIO. Or a
 * memory allocation failed (-%ENOMEM). Otherwise, it is safer to switch to R/O
 * mode, simply because we do not know what happened at the MTD level, and we
 * cannot handle this. E.g., the underlying driver may have become crazy, and
 * it is safer to switch to R/O mode to preserve the data.
 *
 * And bear in mind, this is about reading from the target PEB, i.e. the PEB
 * which we have just written.
 */
static int is_error_sane(int err)
{
	if (err == -EIO || err == -ENOMEM || err == UBI_IO_BAD_HDR ||
	    err == UBI_IO_BAD_HDR_EBADMSG || err == -ETIMEDOUT)
		return 0;
	return 1;
}

static int select_leb_for_consolidation(struct ubi_volume *vol)
{
	struct ubi_consolidation_ctx *conso = &vol->consolidation;
	struct list_head *pool = NULL;

	mutex_lock(&vol->eba_lock);

	/*
	 * FIXME: For simplicity, we only try to consolidate dirty PEBs if
	 * they contain only one valid LEB. This should work fine for SLC
	 * NANDs, but can be a problem for TLC ones.
	 *
	 * If there's no dirty PEBs, pick the oldest open one.
	 */
	if (!list_empty(&vol->eba_tbl->closed.dirty[0]))
		pool = &vol->eba_tbl->closed.dirty[0];
	else if (!list_empty(&vol->eba_tbl->open))
		pool = &vol->eba_tbl->open;

	if (pool) {
		struct ubi_eba_cdesc *cdesc;

		cdesc = list_first_entry(pool, struct ubi_eba_cdesc, node);
		conso->loffset = 0;
		conso->ldesc.lnum = cdesc_to_lnum(vol, cdesc);
		conso->ldesc.lpos++;
		conso->cpeb->lnums[conso->ldesc.lpos] = conso->ldesc.lnum;
	}
	mutex_lock(&vol->eba_lock);

	return pool ? 0 : -ENOENT;
}

static void reset_consolidation(struct ubi_consolidation_ctx *ctx)
{
	ctx->cancel = 0;
	ctx->ldesc.lnum = UBI_LEB_UNMAPPED;
	ctx->ldesc.pnum = -1;
	ctx->ldesc.lpos = -1;
	ctx->loffset = 0;
	ctx->cpeb = NULL;
}

static int init_consolidation(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;

	ctx->buf = kmalloc(ubi->min_io_size, GFP_KERNEL);
	if  (!ctx->buf)
		return -ENOMEM;

	reset_consolidation(ctx);

	return 0;
}

static void cleanup_consolidation(struct ubi_volume *vol)
{
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;

	kfree(ctx->buf);
}

static void cancel_consolidation(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	struct ubi_consolidated_peb *cpeb = ctx->cpeb;

	mutex_lock(&vol->eba_lock);
	reset_consolidation(ctx);
	mutex_unlock(&vol->eba_lock);

	if (cpeb)
		return;

	ubi_wl_put_peb(ubi, vol->vol_id, UBI_LEB_UNMAPPED, cpeb->pnum, 0);
	kfree(cpeb);
}

static int start_consolidation(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	struct ubi_consolidated_peb *cpeb;
	int err, i, lebs_per_cpeb = mtd_pairing_groups_per_eb(ubi->mtd);
	struct ubi_vid_hdr *hdr;

	cpeb = kzalloc(sizeof(*cpeb) + (lebs_per_cpeb * sizeof(*cpeb->lnums)),
		       GFP_KERNEL);
	if (!cpeb)
		return -ENOMEM;

	for (i = 0; i < lebs_per_cpeb; i++)
		cpeb->lnums[i] = UBI_LEB_UNMAPPED;

	cpeb->pnum = ubi_wl_get_peb(vol->ubi);
	if (cpeb->pnum < 0) {
		up_read(&ubi->fm_eba_sem);
		err = cpeb->pnum;
		goto err_free_cpeb;
	}

	/* Write the dummy VID header. */
	hdr = ctx->buf;
	memset(hdr, 0, ubi->min_io_size);
	hdr->flags = cpu_to_be32(VIDH_FLAG_CONSOLIDATED);
	err = ubi_io_write_vid_hdr(ubi, cpeb->pnum, hdr);
	if (err)
		goto err_put_peb;

	mutex_lock(&vol->eba_lock);
	err = select_leb_for_consolidation(vol);
	if (err)
		goto err_unlock_eba;

	ctx->cpeb = cpeb;
	mutex_unlock(&vol->eba_lock);

	return 0;

err_unlock_eba:
	ctx->cpeb = NULL;
	mutex_unlock(&vol->eba_lock);

err_put_peb:
	up_read(&ubi->fm_eba_sem);
	ubi_wl_put_peb(ubi, vol->vol_id, UBI_LEB_UNMAPPED, cpeb->pnum, 0);

err_free_cpeb:
	kfree(cpeb);

	return err;
}

static int continue_consolidation(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	struct ubi_leb_desc src;
	int err = 0;

	ubi_assert(ctx->cpeb);

	if (ctx->loffset == ubi->leb_size) {
		err = select_leb_for_consolidation(vol);
		if (err)
			return err;
	}

	ubi_eba_get_ldesc(vol, ctx->ldesc.lnum, &src);

	/*
	 * We only try to take the lock. If it fails this means someone
	 * is modifying the LEB, which means we should cancel the
	 * consolidation.
	 * */
	err = leb_read_trylock(ubi, vol->vol_id, ctx->ldesc.lnum);
	if (err == 0)
		return -EBUSY;
	else if (err < 0)
		return err;

	/*
	 * Only copy one page here.
	 * TODO: support an 'aggressive' mode where we run consolidation
	 * until we're able to free the consolidated PEBs.
	 */
	err = read_leb(vol, ctx->buf, &src, ctx->loffset, ubi->min_io_size);

	/*
	 * We can safely release the lock here: we'll check if the LEB is
	 * still valid before writing the VID headers. Which means  we can
	 * cancel the consolidation if one of the LEBs we're consolidating is
	 * invalidated.
	 */
	leb_read_unlock(ubi, vol->vol_id, ctx->ldesc.lnum);

	if (err && !mtd_is_bitflip(err))
		return err;

	/* Write data to the consolidated PEB. */
	err = write_leb(vol, ctx->buf, &ctx->ldesc, ctx->loffset,
			ubi->min_io_size);
	if (err)
		return err;

	ctx->loffset += ubi->min_io_size;

	return -EAGAIN;
}

static int finish_consolidation(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	struct ubi_consolidated_peb *cpeb = ctx->cpeb;
	struct ubi_vid_hdr *hdrs = ctx->buf;
	int lebs_per_cpeb = mtd_pairing_groups_per_eb(ubi->mtd);
	int err, offset, locked, i;
	int *opnums, *olnums;

	/* Allocate an array to store old pnum and lnum values. */
	opnums = kmalloc(sizeof(opnums) * lebs_per_cpeb * 2, GFP_KERNEL);
	if (!opnums)
		return -ENOMEM;

	olnums = opnums + lebs_per_cpeb;

	/* Try to lock all consolidated LEBs in write mode. */
	for (locked = 0; locked < lebs_per_cpeb; locked++) {
		err = leb_write_trylock(ubi, vol->vol_id, cpeb->lnums[locked]);
		if (!err)
			err = -EAGAIN;

		if (err < 0)
			goto err_unlock;
	}

	/*
	 * We locked all the LEBs in write mode, now make sure nobody tried to
	 * cancel the consolidation in the meantime.
	 */
	if (ctx->cancel) {
		err = -EBUSY;
		goto err_unlock;
	}

	/* Pad with zeros. */
	memset(ctx->buf, 0, ubi->min_io_size);
	for (i = 0; i < lebs_per_cpeb; i++) {
		u32 crc;

		hdrs[i].magic = cpu_to_be32(UBI_VID_HDR_MAGIC);
		hdrs[i].data_pad = cpu_to_be32(vol->data_pad);
		hdrs[i].sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
		hdrs[i].vol_id = cpu_to_be32(vol->vol_id);
		hdrs[i].lnum = cpu_to_be32(cpeb->lnums[i]);
		hdrs[i].compat = ubi_get_compat(ubi, vol->vol_id);
		hdrs[i].vol_type = UBI_VID_DYNAMIC;
		hdrs[i].magic = cpu_to_be32(UBI_VID_HDR_MAGIC);
		hdrs[i].version = UBI_VERSION;
		crc = crc32(UBI_CRC32_INIT, &hdrs[i], UBI_VID_HDR_SIZE_CRC);
		hdrs[i].hdr_crc = cpu_to_be32(crc);
	}

	/* Duplicate the VID headers on the last free pages. */
	offset = (lebs_per_cpeb * ubi->leb_size) + ubi->leb_start;
	for (; offset < ubi->peb_size - ubi->min_io_size;
	     offset += ubi->min_io_size) {
		err = ubi_io_write(ubi, hdrs, cpeb->pnum, offset,
				   ubi->min_io_size);
		if (err)
			goto err_unlock;
	}

	down_read(&ubi->fm_eba_sem);
	mutex_lock(&vol->eba_lock);
	for (i = 0; i < lebs_per_cpeb; i++) {
		int lnum = cpeb->lnums[i];
		struct ubi_eba_cdesc *cdesc = &vol->eba_tbl->cdescs[lnum];
		struct ubi_leb_desc ldesc;

		ubi_eba_get_ldesc(vol, lnum, &ldesc);
		if (ubi_eba_invalidate_leb_locked(vol, &ldesc, true)) {
			/*
			 * We are about to release this PEB, update the free
			 * PEB counter accordingly.
			 */
			vol->eba_tbl->free_pebs++;
			opnums[i] = ldesc.pnum;
		} else {
			opnums[i] = -1;
		}

		olnums[i] = lnum;

		cdesc->cpeb = cpeb;
		list_del_init(&cdesc->node);

		/* Only add the first LEB. */
		if (!i)
			list_add_tail(&cdesc->node,
				      &vol->eba_tbl->closed.clean);
		set_bit(lnum, vol->eba_tbl->consolidated);
	}
	reset_consolidation(&vol->consolidation);

	/* Consolidation took one PEB. */
	vol->eba_tbl->free_pebs--;

	mutex_unlock(&vol->eba_lock);
	up_read(&ubi->fm_eba_sem);

	for (locked--; locked >= 0; locked--)
		leb_write_unlock(vol->ubi, vol->vol_id, cpeb->lnums[locked]);

	for (i = 0; i < lebs_per_cpeb; i++) {
		/* The PEB still contains valid LEBs. */
		if (opnums[i] == -1)
			continue;

		ubi_wl_put_peb(ubi, vol->vol_id, olnums[i], opnums[i], 0);
	}

	kfree(opnums);

	return 0;

err_unlock:
	for (locked--; locked >= 0; locked--)
		leb_write_unlock(vol->ubi, vol->vol_id, cpeb->lnums[locked]);

	kfree(opnums);

	return err;
}

static bool consolidation_cancelled(struct ubi_volume *vol)
{
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	bool ret = false;

	mutex_lock(&vol->eba_lock);
	if (ctx->cancel)
		ret = true;
	mutex_unlock(&vol->eba_lock);

	return ret;
}

static int consolidation_step(struct ubi_volume *vol)
{
	struct ubi_device *ubi = vol->ubi;
	int lebs_per_cpeb = mtd_pairing_groups_per_eb(ubi->mtd);
	struct ubi_consolidation_ctx *ctx = &vol->consolidation;
	int err = 0;

	if (ctx->ldesc.lpos < 0) {
		err = start_consolidation(vol);
		if (err)
			return err;
	}

	/* Check if consolidation has been cancelled. */
	if (consolidation_cancelled(vol)) {
		err = -EBUSY;
		goto cancel;
	}

	if (ctx->ldesc.lpos == lebs_per_cpeb - 1 &&
	    ctx->loffset == ubi->leb_size)
		err = finish_consolidation(vol);
	else
		err = continue_consolidation(vol);

	if (err && err != -EAGAIN)
		goto cancel;

	/* Check again if consolidation has been cancelled. */
	if (consolidation_cancelled(vol)) {
		err = -EBUSY;
		goto cancel;
	}

	return err;

cancel:
	cancel_consolidation(vol);

	return err;
}

static bool consolidation_possible(struct ubi_volume *vol)
{
	/* TODO: check the number of open and dirty PEBs. */
	return true;
}

static bool consolidation_required(struct ubi_volume *vol)
{
	/*
	 * TODO: consolidation is required when some UBI users are
	 * waiting for open LEBs.
	 */
	return false;
}

static bool consolidation_recommended(struct ubi_volume *vol)
{
	/*
	 * TODO: consolidation when the number of availables go
	 * below 1/3 of the total number of PEBs?
	 */
	return false;
}

static void consolidation_work(struct work_struct *work)
{
	struct ubi_consolidation_ctx *conso = container_of(work,
						struct ubi_consolidation_ctx,
						work);
	struct ubi_volume *vol = container_of(conso, struct ubi_volume,
					      consolidation);
	int err;

	/*
	 * TODO: decide when to continue consolidating and when to
	 * reschedule.
	 */
	while (true) {
		err = consolidation_step(vol);
		if (err != -EAGAIN) {
			if (!consolidation_required(vol)) {
				schedule_work(work);
				break;
			}
		}
	}

	/*
	 * Make sure we never run into a case where consolidation is required
	 * but impossible.
	 */
	ubi_assert(!consolidation_required(vol) ||
		   consolidation_possible(vol));

	if (consolidation_required(vol))
		schedule_work(work);
}

/**
 * ubi_eba_copy_leb - copy logical eraseblock.
 * @ubi: UBI device description object
 * @from: physical eraseblock number from where to copy
 * @to: physical eraseblock number where to copy
 * @vid_hdr: VID header of the @from physical eraseblock
 *
 * This function copies logical eraseblock from physical eraseblock @from to
 * physical eraseblock @to. The @vid_hdr buffer may be changed by this
 * function. Returns:
 *   o %0 in case of success;
 *   o %MOVE_CANCEL_RACE, %MOVE_TARGET_WR_ERR, %MOVE_TARGET_BITFLIPS, etc;
 *   o a negative error code in case of failure.
 */

/*
 * TODO: rename into ubi_eba_copy_peb() and support the consolidated PEB
 * case
 */
int ubi_eba_copy_peb(struct ubi_device *ubi, int from, int to,
		     struct ubi_vid_hdr *vid_hdr)
{
	int err, vol_id, lnum, data_size, aldata_size, idx;
	struct ubi_leb_desc ldesc;
	struct ubi_volume *vol;
	uint32_t crc;

	vol_id = be32_to_cpu(vid_hdr->vol_id);
	lnum = be32_to_cpu(vid_hdr->lnum);

	dbg_wl("copy LEB %d:%d, PEB %d to PEB %d", vol_id, lnum, from, to);

	idx = vol_id2idx(ubi, vol_id);
	spin_lock(&ubi->volumes_lock);
	/*
	 * Note, we may race with volume deletion, which means that the volume
	 * this logical eraseblock belongs to might be being deleted. Since the
	 * volume deletion un-maps all the volume's logical eraseblocks, it will
	 * be locked in 'ubi_wl_put_peb()' and wait for the WL worker to finish.
	 */
	vol = ubi->volumes[idx];
	spin_unlock(&ubi->volumes_lock);
	if (!vol) {
		/* No need to do further work, cancel */
		dbg_wl("volume %d is being removed, cancel", vol_id);
		return MOVE_CANCEL_RACE;
	}

	/*
	 * We do not want anybody to write to this logical eraseblock while we
	 * are moving it, so lock it.
	 *
	 * Note, we are using non-waiting locking here, because we cannot sleep
	 * on the LEB, since it may cause deadlocks. Indeed, imagine a task is
	 * unmapping the LEB which is mapped to the PEB we are going to move
	 * (@from). This task locks the LEB and goes sleep in the
	 * 'ubi_wl_put_peb()' function on the @ubi->move_mutex. In turn, we are
	 * holding @ubi->move_mutex and go sleep on the LEB lock. So, if the
	 * LEB is already locked, we just do not move it and return
	 * %MOVE_RETRY. Note, we do not return %MOVE_CANCEL_RACE here because
	 * we do not know the reasons of the contention - it may be just a
	 * normal I/O on this LEB, so we want to re-try.
	 */
	err = leb_write_trylock(ubi, vol_id, lnum);
	if (err) {
		dbg_wl("contention on LEB %d:%d, cancel", vol_id, lnum);
		return MOVE_RETRY;
	}

	/*
	 * The LEB might have been put meanwhile, and the task which put it is
	 * probably waiting on @ubi->move_mutex. No need to continue the work,
	 * cancel it.
	 */
	ubi_eba_get_ldesc(vol, lnum, &ldesc);
	if (ldesc.pnum != from) {
		dbg_wl("LEB %d:%d is no longer mapped to PEB %d, mapped to PEB %d, cancel",
		       vol_id, lnum, from, ldesc.pnum);
		err = MOVE_CANCEL_RACE;
		goto out_unlock_leb;
	}

	if (vid_hdr->vol_type == UBI_VID_STATIC) {
		data_size = be32_to_cpu(vid_hdr->data_size);
		aldata_size = ALIGN(data_size, ubi->min_io_size);
	} else
		data_size = aldata_size =
			    vol->leb_size - be32_to_cpu(vid_hdr->data_pad);

	/*
	 * OK, now the LEB is locked and we can safely start moving it. Since
	 * this function utilizes the @ubi->peb_buf buffer which is shared
	 * with some other functions - we lock the buffer by taking the
	 * @ubi->buf_mutex.
	 */
	mutex_lock(&ubi->buf_mutex);
	dbg_wl("read %d bytes of data", aldata_size);
	err = ubi_io_read_data(ubi, ubi->peb_buf, from, 0, aldata_size);
	if (err && err != UBI_IO_BITFLIPS) {
		ubi_warn(ubi, "error %d while reading data from PEB %d",
			 err, from);
		err = MOVE_SOURCE_RD_ERR;
		goto out_unlock_buf;
	}

	/*
	 * Now we have got to calculate how much data we have to copy. In
	 * case of a static volume it is fairly easy - the VID header contains
	 * the data size. In case of a dynamic volume it is more difficult - we
	 * have to read the contents, cut 0xFF bytes from the end and copy only
	 * the first part. We must do this to avoid writing 0xFF bytes as it
	 * may have some side-effects. And not only this. It is important not
	 * to include those 0xFFs to CRC because later the they may be filled
	 * by data.
	 */
	if (vid_hdr->vol_type == UBI_VID_DYNAMIC)
		aldata_size = data_size =
			ubi_calc_data_len(ubi, ubi->peb_buf, data_size);

	cond_resched();
	crc = crc32(UBI_CRC32_INIT, ubi->peb_buf, data_size);
	cond_resched();

	/*
	 * It may turn out to be that the whole @from physical eraseblock
	 * contains only 0xFF bytes. Then we have to only write the VID header
	 * and do not write any data. This also means we should not set
	 * @vid_hdr->copy_flag, @vid_hdr->data_size, and @vid_hdr->data_crc.
	 */
	if (data_size > 0) {
		vid_hdr->copy_flag = 1;
		vid_hdr->data_size = cpu_to_be32(data_size);
		vid_hdr->data_crc = cpu_to_be32(crc);
	}
	vid_hdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));

	err = ubi_io_write_vid_hdr(ubi, to, vid_hdr);
	if (err) {
		if (err == -EIO)
			err = MOVE_TARGET_WR_ERR;
		goto out_unlock_buf;
	}

	cond_resched();

	/* Read the VID header back and check if it was written correctly */
	err = ubi_io_read_vid_hdr(ubi, to, vid_hdr, 1);
	if (err) {
		if (err != UBI_IO_BITFLIPS) {
			ubi_warn(ubi, "error %d while reading VID header back from PEB %d",
				 err, to);
			if (is_error_sane(err))
				err = MOVE_TARGET_RD_ERR;
		} else
			err = MOVE_TARGET_BITFLIPS;
		goto out_unlock_buf;
	}

	if (data_size > 0) {
		err = ubi_io_write_data(ubi, ubi->peb_buf, to, 0, aldata_size);
		if (err) {
			if (err == -EIO)
				err = MOVE_TARGET_WR_ERR;
			goto out_unlock_buf;
		}

		cond_resched();
	}

	ubi_eba_get_ldesc(vol, lnum, &ldesc);
	ubi_assert(ldesc.pnum == from);
	down_read(&ubi->fm_eba_sem);
	ubi_eba_set_pnum(vol, lnum, to);
	up_read(&ubi->fm_eba_sem);

out_unlock_buf:
	mutex_unlock(&ubi->buf_mutex);
out_unlock_leb:
	leb_write_unlock(ubi, vol_id, lnum);
	return err;
}

/**
 * print_rsvd_warning - warn about not having enough reserved PEBs.
 * @ubi: UBI device description object
 *
 * This is a helper function for 'ubi_eba_init()' which is called when UBI
 * cannot reserve enough PEBs for bad block handling. This function makes a
 * decision whether we have to print a warning or not. The algorithm is as
 * follows:
 *   o if this is a new UBI image, then just print the warning
 *   o if this is an UBI image which has already been used for some time, print
 *     a warning only if we can reserve less than 10% of the expected amount of
 *     the reserved PEB.
 *
 * The idea is that when UBI is used, PEBs become bad, and the reserved pool
 * of PEBs becomes smaller, which is normal and we do not want to scare users
 * with a warning every time they attach the MTD device. This was an issue
 * reported by real users.
 */
static void print_rsvd_warning(struct ubi_device *ubi,
			       struct ubi_attach_info *ai)
{
	/*
	 * The 1 << 18 (256KiB) number is picked randomly, just a reasonably
	 * large number to distinguish between newly flashed and used images.
	 */
	if (ai->max_sqnum > (1 << 18)) {
		int min = ubi->beb_rsvd_level / 10;

		if (!min)
			min = 1;
		if (ubi->beb_rsvd_pebs > min)
			return;
	}

	ubi_warn(ubi, "cannot reserve enough PEBs for bad PEB handling, reserved %d, need %d",
		 ubi->beb_rsvd_pebs, ubi->beb_rsvd_level);
	if (ubi->corr_peb_count)
		ubi_warn(ubi, "%d PEBs are corrupted and not used",
			 ubi->corr_peb_count);
}

/**
 * self_check_eba - run a self check on the EBA table constructed by fastmap.
 * @ubi: UBI device description object
 * @ai_fastmap: UBI attach info object created by fastmap
 * @ai_scan: UBI attach info object created by scanning
 *
 * Returns < 0 in case of an internal error, 0 otherwise.
 * If a bad EBA table entry was found it will be printed out and
 * ubi_assert() triggers.
 */
int self_check_eba(struct ubi_device *ubi, struct ubi_attach_info *ai_fastmap,
		   struct ubi_attach_info *ai_scan)
{
	int i, j, num_volumes, ret = 0;
	int **scan_eba, **fm_eba;
	struct ubi_ainf_volume *av;
	struct ubi_volume *vol;
	struct ubi_ainf_peb *aeb;
	struct rb_node *rb;

	num_volumes = ubi->vtbl_slots + UBI_INT_VOL_COUNT;

	scan_eba = kmalloc(sizeof(*scan_eba) * num_volumes, GFP_KERNEL);
	if (!scan_eba)
		return -ENOMEM;

	fm_eba = kmalloc(sizeof(*fm_eba) * num_volumes, GFP_KERNEL);
	if (!fm_eba) {
		kfree(scan_eba);
		return -ENOMEM;
	}

	for (i = 0; i < num_volumes; i++) {
		vol = ubi->volumes[i];
		if (!vol)
			continue;

		scan_eba[i] = kmalloc(vol->avail_lebs * sizeof(**scan_eba),
				      GFP_KERNEL);
		if (!scan_eba[i]) {
			ret = -ENOMEM;
			goto out_free;
		}

		fm_eba[i] = kmalloc(vol->avail_lebs * sizeof(**fm_eba),
				    GFP_KERNEL);
		if (!fm_eba[i]) {
			ret = -ENOMEM;
			goto out_free;
		}

		for (j = 0; j < vol->avail_lebs; j++)
			scan_eba[i][j] = fm_eba[i][j] = UBI_LEB_UNMAPPED;

		av = ubi_find_av(ai_scan, idx2vol_id(ubi, i));
		if (!av)
			continue;

		ubi_rb_for_each_entry(rb, aeb, &av->root, u.rb)
			scan_eba[i][aeb->lnum] = aeb->pnum;

		av = ubi_find_av(ai_fastmap, idx2vol_id(ubi, i));
		if (!av)
			continue;

		ubi_rb_for_each_entry(rb, aeb, &av->root, u.rb)
			fm_eba[i][aeb->lnum] = aeb->pnum;

		for (j = 0; j < vol->avail_lebs; j++) {
			if (scan_eba[i][j] != fm_eba[i][j]) {
				if (scan_eba[i][j] == UBI_LEB_UNMAPPED ||
					fm_eba[i][j] == UBI_LEB_UNMAPPED)
					continue;

				ubi_err(ubi, "LEB:%i:%i is PEB:%i instead of %i!",
					vol->vol_id, j, fm_eba[i][j],
					scan_eba[i][j]);
				ubi_assert(0);
			}
		}
	}

out_free:
	for (i = 0; i < num_volumes; i++) {
		if (!ubi->volumes[i])
			continue;

		kfree(scan_eba[i]);
		kfree(fm_eba[i]);
	}

	kfree(scan_eba);
	kfree(fm_eba);
	return ret;
}

void ubi_eba_get_ldesc(struct ubi_volume *vol, int lnum,
		      struct ubi_leb_desc *ldesc)
{
	if (!vol->mlc_safe) {
		ldesc->pnum = vol->eba_tbl->descs[lnum].pnum;
		ldesc->lpos = -1;
	} else if (test_bit(lnum, vol->eba_tbl->consolidated)) {
		struct ubi_consolidated_peb *cpeb =
				vol->eba_tbl->cdescs[lnum].cpeb;
		int lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);
		int i;

		for (i = 0; i < lebs_per_cpeb; i++) {
			if (cpeb->lnums[i] == lnum)
				break;
		}

		ubi_assert(i < lebs_per_cpeb);
		ldesc->lpos = i;
		ldesc->pnum = cpeb->pnum;
	} else {
		ldesc->pnum = vol->eba_tbl->descs[lnum].pnum;
		ldesc->lpos = -1;
	}

	ldesc->lnum = lnum;
}

void ubi_eba_set_cpeb(struct ubi_volume *vol, int lnum,
		      struct ubi_consolidated_peb *cpeb)
{
	ubi_assert(vol->mlc_safe);
	ubi_assert(vol->eba_tbl->consolidated);

	set_bit(lnum, vol->eba_tbl->consolidated);

}

bool ubi_eba_is_mapped(struct ubi_volume *vol, int lnum)
{
	int pnum;

	if (!vol->mlc_safe)
		pnum = vol->eba_tbl->cdescs[lnum].pnum;
	else if (test_bit(lnum, vol->eba_tbl->consolidated))
		pnum = vol->eba_tbl->cdescs[lnum].cpeb->pnum;
	else
		pnum = vol->eba_tbl->descs[lnum].pnum;

	return pnum >= 0;
}

struct ubi_eba_table *ubi_eba_create_table(struct ubi_volume *vol, int nlebs)
{
	struct ubi_eba_table *tbl;
	int err = -ENOMEM;
	int i;

	tbl = kzalloc(sizeof(*tbl), GFP_KERNEL);
	if (!tbl)
		return ERR_PTR(-ENOMEM);

	if (!vol->mlc_safe) {
		tbl->descs = kmalloc(nlebs * sizeof(*tbl->descs), GFP_KERNEL);
		if (!tbl->descs)
			goto err;
	} else {
		int lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);

		tbl->descs = kmalloc(nlebs * sizeof(*tbl->cdescs), GFP_KERNEL);
		if (!tbl->descs)
			goto err;

		tbl->consolidated = kzalloc(DIV_ROUND_UP(nlebs, BITS_PER_LONG),
					    GFP_KERNEL);
		if (!tbl->consolidated)
			goto err;

		INIT_LIST_HEAD(&tbl->open);
		INIT_LIST_HEAD(&tbl->closed.clean);

		tbl->closed.dirty = kmalloc((lebs_per_cpeb - 1) *
					    sizeof(*tbl->closed.dirty),
					    GFP_KERNEL);
		if (tbl->closed.dirty)
			goto err;

		for (i = 0; i < lebs_per_cpeb - 1; i++)
			INIT_LIST_HEAD(&tbl->closed.dirty[i]);

		for (i = 0; i < nlebs; i++) {
			tbl->cdescs[i].pnum = UBI_LEB_UNMAPPED;
			INIT_LIST_HEAD(&tbl->cdescs[i].node);
		}
	}

	return tbl;

err:
	kfree(tbl->closed.dirty);
	kfree(tbl->consolidated);
	kfree(tbl->descs);
	kfree(tbl);

	return ERR_PTR(err);
}

void ubi_eba_destroy_table(struct ubi_eba_table *tbl)
{
	if (!tbl)
		return;

	kfree(tbl->consolidated);
	kfree(tbl->descs);
	kfree(tbl);
}

void ubi_eba_copy_table(struct ubi_volume *vol, struct ubi_eba_table *dst,
		        int nentries)
{
	struct ubi_eba_table *src;
	int i;

	ubi_assert(dst && vol && vol->eba_tbl);

	src = vol->eba_tbl;

	if (!vol->mlc_safe) {
		for (i = 0; i < nentries; i++)
			dst->descs[i].pnum = src->descs[i].pnum;
	} else {
		int lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);
		struct ubi_eba_cdesc *cdesc;
		int lnum;

		for (i = 0; i < nentries; i++) {
			if (test_bit(i, src->consolidated)) {
				/*
				 * No need to copy the cpeb resource, only
				 * ubi_leb_unmap() should do that.
				 */
				dst->cdescs[i].cpeb = src->cdescs[i].cpeb;

				set_bit(i, dst->consolidated);
			} else {
				dst->cdescs[i].pnum = src->cdescs[i].pnum;
			}
		}

		list_for_each_entry(cdesc, &src->open, node) {
			lnum = cdesc_to_lnum(vol, cdesc);
			list_add_tail(&dst->cdescs[lnum].node, &dst->open);
		}

		list_for_each_entry(cdesc, &src->closed.clean, node) {
			lnum = cdesc_to_lnum(vol, cdesc);
			list_add_tail(&dst->cdescs[lnum].node,
				      &dst->closed.clean);
		}

		for (i = 0; i < lebs_per_cpeb; i++) {
			struct list_head *dirty = &src->closed.dirty[i];

			list_for_each_entry(cdesc, dirty, node) {
				lnum = cdesc_to_lnum(vol, cdesc);
				list_add_tail(&dst->cdescs[lnum].node, dirty);
			}
		}
	}
}

int ubi_eba_count_free_pebs(struct ubi_volume *vol)
{
	int i, used_pebs = 0;

	if (!vol->mlc_safe) {
		for (i = 0; i < vol->avail_lebs; i++) {
			if (vol->eba_tbl->descs[i].pnum >= 0)
				used_pebs++;
		}
	} else {
		int lebs_per_cpeb = mtd_pairing_groups_per_eb(vol->ubi->mtd);

		for (i = 0; i < vol->avail_lebs; i++) {
			if (!test_bit(i, vol->eba_tbl->consolidated)) {
				if (vol->eba_tbl->cdescs[i].pnum >= 0)
					used_pebs++;
			} else {
				struct ubi_consolidated_peb *cpeb;
				int j;

				cpeb = vol->eba_tbl->cdescs[i].cpeb;

				for (j = 0; j < lebs_per_cpeb; j++) {
					if (cpeb->lnums[j] >= 0 &&
					    cpeb->lnums[j] < i)
						break;
				}

				if (j == lebs_per_cpeb)
					used_pebs++;
			}
		}
	}

	return vol->reserved_pebs - used_pebs;
}

void ubi_eba_set_table(struct ubi_volume *vol, struct ubi_eba_table *tbl)
{
	ubi_eba_destroy_table(vol->eba_tbl);
	vol->eba_tbl = tbl;
}

/**
 * ubi_eba_init - initialize the EBA sub-system using attaching information.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
int ubi_eba_init(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	int i, err, num_volumes;
	struct ubi_ainf_volume *av;
	struct ubi_volume *vol;
	struct ubi_ainf_peb *aeb;
	struct rb_node *rb;

	dbg_eba("initialize EBA sub-system");

	spin_lock_init(&ubi->ltree_lock);
	mutex_init(&ubi->alc_mutex);
	ubi->ltree = RB_ROOT;

	ubi->global_sqnum = ai->max_sqnum + 1;
	num_volumes = ubi->vtbl_slots + UBI_INT_VOL_COUNT;

	for (i = 0; i < num_volumes; i++) {
		struct ubi_eba_table *tbl;

		vol = ubi->volumes[i];
		if (!vol)
			continue;

		cond_resched();

		mutex_init(&vol->eba_lock);

		err = init_consolidation(vol);
		if (err)
			goto out_free;

		tbl = ubi_eba_create_table(vol, vol->reserved_pebs);
		if (IS_ERR(tbl)) {
			err = PTR_ERR(tbl);
			goto out_free;
		}

		ubi_eba_set_table(vol, tbl);

		av = ubi_find_av(ai, idx2vol_id(ubi, i));
		if (!av)
			continue;

		ubi_rb_for_each_entry(rb, aeb, &av->root, u.rb) {
			if (aeb->lnum >= vol->avail_lebs)
				/*
				 * This may happen in case of an unclean reboot
				 * during re-size.
				 */
				ubi_move_aeb_to_list(av, aeb, &ai->erase);
			else
				ubi_eba_set_pnum(vol, aeb->lnum, aeb->pnum);
		}

		vol->eba_tbl->free_pebs = ubi_eba_count_free_pebs(vol);
	}

	if (ubi->avail_pebs < EBA_RESERVED_PEBS) {
		ubi_err(ubi, "no enough physical eraseblocks (%d, need %d)",
			ubi->avail_pebs, EBA_RESERVED_PEBS);
		if (ubi->corr_peb_count)
			ubi_err(ubi, "%d PEBs are corrupted and not used",
				ubi->corr_peb_count);
		err = -ENOSPC;
		goto out_free;
	}
	ubi->avail_pebs -= EBA_RESERVED_PEBS;
	ubi->rsvd_pebs += EBA_RESERVED_PEBS;

	if (ubi->bad_allowed) {
		ubi_calculate_reserved(ubi);

		if (ubi->avail_pebs < ubi->beb_rsvd_level) {
			/* No enough free physical eraseblocks */
			ubi->beb_rsvd_pebs = ubi->avail_pebs;
			print_rsvd_warning(ubi, ai);
		} else
			ubi->beb_rsvd_pebs = ubi->beb_rsvd_level;

		ubi->avail_pebs -= ubi->beb_rsvd_pebs;
		ubi->rsvd_pebs  += ubi->beb_rsvd_pebs;
	}

	dbg_eba("EBA sub-system is initialized");
	return 0;

out_free:
	for (i = 0; i < num_volumes; i++) {
		if (!ubi->volumes[i])
			continue;

		cleanup_consolidation(ubi->volumes[i]);
		ubi_eba_set_table(ubi->volumes[i], NULL);
	}
	return err;
}
