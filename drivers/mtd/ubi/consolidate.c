#include <linux/slab.h>
#include <linux/crc32.h>
#include "ubi.h"

static void add_full_leb(struct ubi_device *ubi, struct ubi_full_leb *fleb)
{
	spin_lock(&ubi->full_lock);
	list_add_tail(&fleb->node, &ubi->full);
	ubi->full_count++;
	spin_unlock(&ubi->full_lock);
}

static struct ubi_full_leb *first_full_leb_locked(struct ubi_device *ubi)
{
	return list_first_entry_or_null(&ubi->full,
					struct ubi_full_leb, node);
}

static int first_full_leb_desc_locked(struct ubi_device *ubi,
				      struct ubi_leb_desc *lebd)
{
	struct ubi_full_leb *fleb;

	fleb = first_full_leb_locked(ubi);
	if (!fleb)
		return -ENOENT;

	*lebd = fleb->desc;
	return 0;
}

static int first_full_leb_desc(struct ubi_device *ubi,
			       struct ubi_leb_desc *lebd)
{
	int ret;

	spin_lock(&ubi->full_lock);
	ret = first_full_leb_desc_locked(ubi, lebd);
	spin_unlock(&ubi->full_lock);

	return ret;
}

static int cmp_leb_desc(const struct ubi_leb_desc *a,
			const struct ubi_leb_desc *b)
{
	return memcmp(a, b, sizeof(*a));
}

static struct ubi_full_leb *
acquire_full_leb(struct ubi_device *ubi, struct ubi_volume **vol, int *retried,
		 int max_retry)
{
	struct ubi_full_leb *fleb = ERR_PTR(-EAGAIN);

	for (; *retried < max_retry; (*retried)++) {
		struct ubi_full_leb *first_fleb;
		struct ubi_leb_desc lebd;
		int err;

		err = first_full_leb_desc(ubi, &lebd);
		if (err)
			break;

		/*
		 * Volume vanished under us. It can happen when someone is
		 * removing a volume.
		 * The volume structure won't be freed before all LEBs have been
		 * unmapped.
		 * LEB unmapping acquires the LEB lock and  we're also taking
		 * this lock when marking LEBs for consolidation, which
		 * guarantees that *vol is valid until we release all the
		 * LEB.
		 *
		 * FIXME: all this is required because ubi_remove_volume()
		 * assign the ubi->volumes[X] entry to NULL before actually
		 * unmapping all the LEBs.
		 */
		spin_lock(&ubi->volumes_lock);
		*vol = ubi->volumes[vol_id2idx(ubi, lebd.vol_id)];
		spin_unlock(&ubi->volumes_lock);

		if (!*vol)
			continue;

		err = leb_write_trylock(ubi, lebd.vol_id, lebd.lnum);
		if (err < 0)
			return ERR_PTR(err);

		if (!err) {
			/*
			 * Lock acquired, let's make sure the LEB is still in
			 * the full list. It may have vanished if someone
			 * unmapped it before our leb_write_trylock() call.
			 */
			spin_lock(&ubi->full_lock);
			first_fleb = first_full_leb_locked(ubi);
			if (first_fleb &&
			    !cmp_leb_desc(&lebd, &first_fleb->desc)) {
				list_del(&first_fleb->node);
				ubi->full_count--;
				fleb = first_fleb;
			}
			spin_unlock(&ubi->full_lock);

			/* We acquired the LEB. */
			if (fleb != ERR_PTR(-EAGAIN))
				break;

			/*
			 * Release the LEB lock if the full LEB entry
			 * disappeared.
			 */
			ubi_eba_leb_write_unlock(ubi, lebd.vol_id, lebd.lnum);

		} else if (err) {
			/*
			 * Contention. Let's make sure the LEB is still in
			 * the full list, and move it at the end of the list.
			 */
			spin_lock(&ubi->full_lock);
			first_fleb = first_full_leb_locked(ubi);
			if (fleb &&
			    !cmp_leb_desc(&lebd, &first_fleb->desc)) {
				list_del(&first_fleb->node);
				list_add_tail(&first_fleb->node, &ubi->full);
			}
			spin_unlock(&ubi->full_lock);
		}

	}

	return fleb;
}

static void return_full_leb(struct ubi_device *ubi,
					  struct ubi_full_leb *fleb)
{
	add_full_leb(ubi, fleb);
	ubi_eba_leb_write_unlock(ubi, fleb->desc.vol_id,
				 fleb->desc.lnum);
}

static void return_consolidable_lebs(struct ubi_device *ubi,
				     struct list_head *flebs)
{
	struct ubi_full_leb *fleb;

	while(!list_empty(flebs)) {
		fleb = list_first_entry(flebs, struct ubi_full_leb, node);
		list_del(&fleb->node);
		return_full_leb(ubi, fleb);
	}
}

static void release_consolidated_lebs(struct ubi_device *ubi,
				      struct list_head *flebs)
{
	struct ubi_full_leb *fleb;

	while(!list_empty(flebs)) {
		fleb = list_first_entry(flebs, struct ubi_full_leb, node);
		list_del(&fleb->node);
		ubi_eba_leb_write_unlock(ubi, fleb->desc.vol_id,
					 fleb->desc.lnum);
		kfree(fleb);
	}
}

static int find_consolidable_lebs(struct ubi_device *ubi,
				  struct list_head *flebs,
				  struct ubi_volume **vols)
{
	int i, err = 0, retried = 0, max_retry = ubi->lebs_per_cpeb * 3;
	struct ubi_full_leb *fleb;

	spin_lock(&ubi->full_lock);
	if (ubi->full_count < ubi->lebs_per_cpeb)
		err = -EAGAIN;
	spin_unlock(&ubi->full_lock);
	if (err)
		return err;

	for (i = 0; i < ubi->lebs_per_cpeb; i++) {
		fleb = acquire_full_leb(ubi, &vols[i], &retried, max_retry);
		if (IS_ERR(fleb)) {
			err = PTR_ERR(fleb);
			goto err;
		}

		list_add_tail(&fleb->node, flebs);
	}

	return 0;

err:
	return_consolidable_lebs(ubi, flebs);

	return err;
}

static int consolidate_lebs(struct ubi_device *ubi)
{
	int i, pnum, offset = ubi->leb_start, err = 0;
	struct ubi_vid_hdr *vid_hdrs;
	struct ubi_leb_desc *clebs;
	struct ubi_full_leb *fleb;
	struct ubi_volume **vols = NULL;
	int *opnums = NULL;
	LIST_HEAD(flebs);

	if (!ubi_conso_consolidation_needed(ubi))
		return 0;

	vols = kzalloc(sizeof(*vols) * ubi->lebs_per_cpeb, GFP_KERNEL);
	if (!vols)
		return -ENOMEM;

	opnums = kzalloc(sizeof(*opnums) * ubi->lebs_per_cpeb, GFP_KERNEL);
	if (!opnums) {
		err = -ENOMEM;
		goto err_free_mem;
	}

	clebs = kzalloc(sizeof(*clebs) * ubi->lebs_per_cpeb, GFP_KERNEL);
	if (!clebs) {
		err = -ENOMEM;
		goto err_free_mem;
	}

	err = find_consolidable_lebs(ubi, &flebs, vols);
	if (err)
		goto err_free_mem;

	mutex_lock(&ubi->buf_mutex);

	pnum = ubi_wl_get_peb(ubi, true);
	if (pnum < 0) {
		err = pnum;
		/* TODO: cleanup exit path */
		mutex_unlock(&ubi->buf_mutex);
		up_read(&ubi->fm_eba_sem);
		goto err_unlock_lebs;
	}

	memset(ubi->peb_buf, 0, ubi->peb_size);
	vid_hdrs = ubi->peb_buf + ubi->vid_hdr_aloffset + ubi->vid_hdr_shift;

	i = 0;
	list_for_each_entry(fleb, &flebs, node) {
		void *buf = ubi->peb_buf + offset;
		struct ubi_volume *vol = vols[i];
		int spnum, data_size, i;
		u32 crc;

		ubi_assert(i < ubi->lebs_per_cpeb);

		/* We have a write lock on the LEB, so it should be mapped. */
		spnum = vol->eba_tbl[fleb->desc.lnum];
		ubi_assert(spnum != UBI_LEB_UNMAPPED);

		opnums[i] = spnum;

		ubi_assert(offset + ubi->leb_size < ubi->peb_size);

		if (!ubi->consolidated[spnum]) {
			ubi_assert(!fleb->desc.lpos);
			err = ubi_io_read(ubi, buf, spnum, ubi->leb_start,
					  ubi->leb_size);
		} else {
			int leb_start = ubi->leb_start +
					(fleb->desc.lpos * ubi->leb_size);

			err = ubi_io_raw_read(ubi, buf, spnum, leb_start,
					      ubi->leb_size);
		}

		if (err && err != UBI_IO_BITFLIPS)
			goto err_unlock_fm_eba;

		if (vol->vol_type == UBI_DYNAMIC_VOLUME) {
			data_size = ubi->leb_size - vol->data_pad;
			vid_hdrs[i].vol_type = UBI_VID_DYNAMIC;
		} else {
			int nvidh = ubi->lebs_per_cpeb;
			struct ubi_vid_hdr *vh;

			vh = ubi_zalloc_vid_hdr(ubi, GFP_NOFS);
			if (!vh) {
				err = -ENOMEM;
				goto err_unlock_fm_eba;
			}

			err = ubi_io_read_vid_hdrs(ubi, spnum, vh, &nvidh, 0);
			if (err && err != UBI_IO_BITFLIPS) {
				ubi_free_vid_hdr(ubi, vh);
				goto err_unlock_fm_eba;
			}

			ubi_free_vid_hdr(ubi, vh);

			data_size = be32_to_cpu(vh[fleb->desc.lpos].data_size);
			vid_hdrs[i].vol_type = UBI_VID_STATIC;
			vid_hdrs[i].used_ebs = cpu_to_be32(vol->used_ebs);
		}

		vid_hdrs[i].data_pad = cpu_to_be32(vol->data_pad);
		vid_hdrs[i].sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
		vid_hdrs[i].vol_id = cpu_to_be32(fleb->desc.vol_id);
		vid_hdrs[i].lnum = cpu_to_be32(fleb->desc.lnum);
		vid_hdrs[i].compat = ubi_get_compat(ubi, fleb->desc.vol_id);
		vid_hdrs[i].data_size = cpu_to_be32(data_size);
		vid_hdrs[i].copy_flag = 1;
		crc = crc32(UBI_CRC32_INIT, buf, data_size);
		vid_hdrs[i].data_crc = cpu_to_be32(crc);
		offset += ubi->leb_size;

		clebs[i].lnum = fleb->desc.lnum;
		clebs[i].vol_id = fleb->desc.vol_id;
		clebs[i].lpos = i;
		i++;
	}

	/*
	 * Pad remaining pages with zeros to prevent problem on some MLC chip
	 * that expect the whole block to be programmed in order to work
	 * reliably (some Hynix chips are impacted).
	 */
	memset(ubi->peb_buf + offset, 0, ubi->peb_size - offset);

	err = ubi_io_write_vid_hdrs(ubi, pnum, vid_hdrs, ubi->lebs_per_cpeb);
	if (err) {
		ubi_warn(ubi, "failed to write VID headers to PEB %d",
			 pnum);
		goto err_unlock_lebs;
	}

	err = ubi_io_raw_write(ubi, ubi->peb_buf + ubi->leb_start,
			       pnum, ubi->leb_start,
			       ubi->peb_size - ubi->leb_start);
	if (err) {
		ubi_warn(ubi, "failed to write %d bytes of data to PEB %d",
			 ubi->peb_size - ubi->leb_start, pnum);
		goto err_unlock_fm_eba;
	}

	mutex_unlock(&ubi->buf_mutex);

	for (i = 0; i < ubi->lebs_per_cpeb; i++) {
		struct ubi_volume *vol = vols[i];
		int vol_id = clebs[i].vol_id;
		int lnum = clebs[i].lnum;

		vol->eba_tbl[lnum] = pnum;

		/*
		 * Invalidate the old pnum entry to avoid releasing the
		 * PEB is some valid LEBs are still stored there.
		 */
		if (ubi->consolidated[opnums[i]] &&
		    !ubi_conso_invalidate_leb(ubi, opnums[i], vol_id, lnum))
			opnums[i] = -1;
	}

	/* Update the consolidated entry. */
	ubi->consolidated[pnum] = clebs;

	up_read(&ubi->fm_eba_sem);
	release_consolidated_lebs(ubi, &flebs);

	for (i = 0; i < ubi->lebs_per_cpeb; i++) {
		/* TODO set torture if needed */
		if (opnums[i] >= 0)
			ubi_wl_put_peb(ubi, opnums[i], 0);
	}

	kfree(clebs);
	kfree(opnums);
	kfree(vols);

	return 0;

err_unlock_fm_eba:
	mutex_unlock(&ubi->buf_mutex);
	up_read(&ubi->fm_eba_sem);

	ubi_wl_put_peb(ubi, pnum, 0);
err_unlock_lebs:
	return_consolidable_lebs(ubi, &flebs);
err_free_mem:
	kfree(clebs);
	kfree(opnums);
	kfree(vols);

	return err;
}

static int consolidation_worker(struct ubi_device *ubi,
				struct ubi_work *wrk,
				int shutdown)
{
	int ret;

	if (shutdown)
		return 0;

	ret = consolidate_lebs(ubi);
	if (ret == -EAGAIN)
		ret = 0;

	ubi->conso_scheduled = 0;
	smp_wmb();

	if (ubi_conso_consolidation_needed(ubi))
		ubi_conso_schedule(ubi);

	return ret;
}

static bool consolidation_possible(struct ubi_device *ubi)
{
	if (ubi->lebs_per_cpeb < 2)
		return false;

	if (ubi->full_count < ubi->lebs_per_cpeb)
		return false;

	return true;
}

bool ubi_conso_consolidation_needed(struct ubi_device *ubi)
{
	if (!consolidation_possible(ubi))
		return false;

	if (ubi_dbg_force_leb_consolidation(ubi))
		return true;

	return ubi->free_count - ubi->beb_rsvd_pebs <=
	       ubi->consolidation_threshold;
}

void ubi_conso_schedule(struct ubi_device *ubi)
{
	struct ubi_work *wrk;

	if (ubi->conso_scheduled)
		return;

	wrk = ubi_alloc_work(ubi);
	if (wrk) {
		ubi->conso_scheduled = 1;
		smp_wmb();

		wrk->func = &consolidation_worker;
		INIT_LIST_HEAD(&wrk->list);
		ubi_schedule_work(ubi, wrk);
	} else
		BUG();
}

int ubi_conso_sync(struct ubi_device *ubi)
{
	int ret = -ENOMEM;

	struct ubi_work *wrk = ubi_alloc_work(ubi);

	if (wrk) {
		wrk->func = &consolidation_worker;
		ret = ubi_schedule_work_sync(ubi, wrk);
	}

	return ret;
}

void ubi_eba_consolidate(struct ubi_device *ubi)
{
	if (consolidation_possible(ubi) && ubi->consolidation_pnum >= 0)
		ubi_conso_schedule(ubi);
}

static void ubi_conso_remove_full_leb(struct ubi_device *ubi, int vol_id,
				      int lnum)
{
	struct ubi_full_leb *fleb;

	spin_lock(&ubi->full_lock);
	list_for_each_entry(fleb, &ubi->full, node) {
		if (fleb->desc.lnum == lnum && fleb->desc.vol_id == vol_id) {
			ubi->full_count--;
			list_del(&fleb->node);
			kfree(fleb);
			break;
		}
	}
	spin_unlock(&ubi->full_lock);
}

struct ubi_leb_desc *
ubi_conso_get_consolidated(struct ubi_device *ubi, int pnum)
{
	if (ubi->consolidated)
		return ubi->consolidated[pnum];

	return NULL;
}

int ubi_conso_add_full_leb(struct ubi_device *ubi, int vol_id, int lnum, int lpos)
{
	struct ubi_full_leb *fleb;

	/*
	 * We don't track full LEBs if we don't need to (which is the case
	 * when UBI does not need or does not support LEB consolidation).
	 */
	if (!ubi->consolidated)
		return 0;

	fleb = kzalloc(sizeof(*fleb), GFP_KERNEL);
	if (!fleb)
		return -ENOMEM;

	fleb->desc.vol_id = vol_id;
	fleb->desc.lnum = lnum;
	fleb->desc.lpos = lpos;

	spin_lock(&ubi->full_lock);
	list_add_tail(&fleb->node, &ubi->full);
	ubi->full_count++;
	spin_unlock(&ubi->full_lock);

	return 0;
}

bool ubi_conso_invalidate_leb(struct ubi_device *ubi, int pnum, int vol_id,
			      int lnum)
{
	struct ubi_leb_desc *clebs = NULL;
	int i, pos = -1, remaining = 0;

	if (!ubi->consolidated)
		return true;

	clebs = ubi->consolidated[pnum];
	if (!clebs) {
		ubi_conso_remove_full_leb(ubi, vol_id, lnum);
		return true;
	}

	for (i = 0; i < ubi->lebs_per_cpeb; i++) {
		if (clebs[i].lnum == lnum && clebs[i].vol_id == vol_id) {
			clebs[i].lnum = -1;
			clebs[i].vol_id = -1;
			pos = i;
		} else if (clebs[i].lnum >= 0) {
			remaining++;
		}
	}

	ubi_assert(pos >= 0);

	if (remaining == ubi->lebs_per_cpeb - 1) {
		for (i = 0; i < ubi->lebs_per_cpeb; i++) {
			if (i == pos)
				continue;

			ubi_conso_add_full_leb(ubi, clebs[i].vol_id,
					       clebs[i].lnum, clebs[i].lpos);
		}
	} else {
		ubi_conso_remove_full_leb(ubi, vol_id, lnum);

		if (!remaining) {
			ubi->consolidated[pnum] = NULL;
			kfree(clebs);
		}
	}

	return !remaining;
}

int ubi_conso_init(struct ubi_device *ubi)
{
	spin_lock_init(&ubi->full_lock);
	INIT_LIST_HEAD(&ubi->full);
	ubi->full_count = 0;
	ubi->consolidation_threshold = (ubi->avail_pebs + ubi->rsvd_pebs) / 3;

	if (ubi->consolidation_threshold < ubi->lebs_per_cpeb)
		ubi->consolidation_threshold = ubi->lebs_per_cpeb;

	if (ubi->lebs_per_cpeb == 1)
		return 0;

	if (ubi->avail_pebs < UBI_CONSO_RESERVED_PEBS) {
		ubi_err(ubi, "no enough physical eraseblocks (%d, need %d)",
			ubi->avail_pebs, UBI_CONSO_RESERVED_PEBS);
		if (ubi->corr_peb_count)
			ubi_err(ubi, "%d PEBs are corrupted and not used",
				ubi->corr_peb_count);
		return -ENOSPC;
	}

	ubi->avail_pebs -= UBI_CONSO_RESERVED_PEBS;
	ubi->rsvd_pebs += UBI_CONSO_RESERVED_PEBS;

	return 0;
}

void ubi_conso_close(struct ubi_device *ubi)
{
	struct ubi_full_leb *fleb;

	while(!list_empty(&ubi->full)) {
		fleb = list_first_entry(&ubi->full, struct ubi_full_leb, node);
		list_del(&fleb->node);
		kfree(fleb);
		ubi->full_count--;
	}

	ubi_assert(ubi->full_count == 0);
}
