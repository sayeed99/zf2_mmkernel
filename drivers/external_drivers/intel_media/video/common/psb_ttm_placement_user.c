/**************************************************************************
 *
 * Copyright (c) 2006-2008 Tungsten Graphics, Inc., Cedar Park, TX., USA
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thomas-at-tungstengraphics-dot-com>
 */

#include "psb_ttm_placement_user.h"
#include "ttm/ttm_bo_driver.h"
#include "ttm/ttm_object.h"
#include "psb_ttm_userobj_api.h"
#include "ttm/ttm_lock.h"
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/dma-buf.h>
#include "drmP.h"
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
#include "drm.h"
#else
#include <uapi/drm/drm.h>
#endif
#include "psb_drv.h"

#define PSB_TTM_DMA_BIT_MASK 32

struct ttm_bo_user_object {
	struct ttm_base_object base;
	struct ttm_buffer_object bo;
	struct page **pages;
	struct {
		/* dma-buf exported from this object, NULL if not exported */
		struct dma_buf *export;
		/* dma-buf attachment backing this object, NULL if not dma-buf backed */
		struct dma_buf_attachment *import;
	} dmabuf;
};

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
static size_t pl_bo_size;
#endif

static uint32_t psb_busy_prios[] = {
	TTM_PL_FLAG_TT | TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED,
	TTM_PL_FLAG_PRIV0, /* CI */
	TTM_PL_FLAG_PRIV2, /* IMR */
	TTM_PL_FLAG_PRIV1, /* DRM_PSB_MEM_MMU */
	TTM_PL_FLAG_SYSTEM
};

const struct ttm_placement default_placement = {0, 0, 0, NULL, 5, psb_busy_prios};

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
static size_t ttm_pl_size(struct ttm_bo_device *bdev, unsigned long num_pages)
{
	size_t page_array_size =
		(num_pages * sizeof(void *) + PAGE_SIZE - 1) & PAGE_MASK;

	if (unlikely(pl_bo_size == 0)) {
		pl_bo_size = bdev->glob->ttm_bo_extra_size +
			     ttm_round_pot(sizeof(struct ttm_bo_user_object));
	}

	return bdev->glob->ttm_bo_size + 2 * page_array_size;
}
#endif

static struct ttm_bo_user_object *ttm_bo_user_lookup(struct ttm_object_file
		*tfile, uint32_t handle) {
	struct ttm_base_object *base;

	base = ttm_base_object_lookup(tfile, handle);
	if (unlikely(base == NULL)) {
		printk(KERN_ERR "Invalid buffer object handle 0x%08lx.\n",
		       (unsigned long)handle);
		return NULL;
	}

	if (unlikely(base->object_type != ttm_buffer_type)) {
		ttm_base_object_unref(&base);
		printk(KERN_ERR "Invalid buffer object handle 0x%08lx.\n",
		       (unsigned long)handle);
		return NULL;
	}

	return container_of(base, struct ttm_bo_user_object, base);
}

struct ttm_buffer_object *ttm_buffer_object_lookup(struct ttm_object_file
		*tfile, uint32_t handle) {
	struct ttm_bo_user_object *user_bo;
	struct ttm_base_object *base;

	user_bo = ttm_bo_user_lookup(tfile, handle);
	if (unlikely(user_bo == NULL))
		return NULL;

	(void)ttm_bo_reference(&user_bo->bo);
	base = &user_bo->base;
	ttm_base_object_unref(&base);
	return &user_bo->bo;
}

static void ttm_bo_user_destroy(struct ttm_buffer_object *bo)
{
	struct ttm_bo_user_object *user_bo =
		container_of(bo, struct ttm_bo_user_object, bo);

	ttm_mem_global_free(bo->glob->mem_glob, bo->acc_size);
	kfree(user_bo);
}

/* This is used for sg_table which is derived from user-pointer */
static void ttm_tt_free_user_pages(struct ttm_buffer_object *bo)
{
	struct page *page;
	struct page **pages = NULL;
	int i, ret;
	struct ttm_bo_user_object *user_bo =
		container_of(bo, struct ttm_bo_user_object, bo);
#if 0
	struct page **pages_to_wb;

	pages_to_wb = kmalloc(ttm->num_pages * sizeof(struct page *),
			GFP_KERNEL);

	if (pages_to_wb && ttm->caching_state != tt_cached) {
		int num_pages_wb = 0;

		for (i = 0; i < ttm->num_pages; ++i) {
			page = ttm->pages[i];
			if (page == NULL)
				continue;
			pages_to_wb[num_pages_wb++] = page;
		}

		if (set_pages_array_wb(pages_to_wb, num_pages_wb))
			printk(KERN_ERR TTM_PFX "Failed to set pages to wb\n");

	} else if (NULL == pages_to_wb) {
		printk(KERN_ERR TTM_PFX
		       "Failed to allocate memory for set wb operation.\n");
	}

#endif
	pages = user_bo->pages;
	ret = drm_prime_sg_to_page_addr_arrays(bo->sg, pages,
						 NULL, bo->num_pages);
	if (WARN_ON(ret)) {
		printk(KERN_ERR "sg to pages failed\n");
		return ;
	}

	for (i = 0; i < bo->num_pages; ++i) {
		page = pages[i];
		if (page == NULL)
			continue;

		put_page(page);
	}
}

/* This is used for sg_table which is derived from user-pointer */
static void ttm_ub_bo_user_destroy(struct ttm_buffer_object *bo)
{
	struct ttm_bo_user_object *user_bo =
		container_of(bo, struct ttm_bo_user_object, bo);

#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0))
	if (user_bo->dmabuf.import) {
		struct dma_buf_attachment *attachment = user_bo->dmabuf.import;
		struct dma_buf *dmabuf = attachment->dmabuf;
		dma_buf_unmap_attachment(attachment,
					bo->sg, DMA_NONE);
		dma_buf_detach(dmabuf, attachment);
		dma_buf_put(dmabuf);
		drm_free_large(user_bo->pages);
		bo->sg= NULL;
	}
	else if (bo->sg) {
		ttm_tt_free_user_pages(bo);
		sg_free_table(bo->sg);
		kfree(bo->sg);
		drm_free_large(user_bo->pages);
		bo->sg = NULL;
	}
	else
		DRM_ERROR("invalid user_bo: neither dmabuf or userptr type\n");

#endif
	ttm_mem_global_free(bo->glob->mem_glob, bo->acc_size);
	kfree(user_bo);
}

static void ttm_bo_user_release(struct ttm_base_object **p_base)
{
	struct ttm_bo_user_object *user_bo;
	struct ttm_base_object *base = *p_base;
	struct ttm_buffer_object *bo;

	*p_base = NULL;

	if (unlikely(base == NULL))
		return;

	user_bo = container_of(base, struct ttm_bo_user_object, base);
	bo = &user_bo->bo;
	ttm_bo_unref(&bo);
}

static void ttm_bo_user_ref_release(struct ttm_base_object *base,
				    enum ttm_ref_type ref_type)
{
	struct ttm_bo_user_object *user_bo =
		container_of(base, struct ttm_bo_user_object, base);
	struct ttm_buffer_object *bo = &user_bo->bo;

	switch (ref_type) {
	case TTM_REF_SYNCCPU_WRITE:
		ttm_bo_synccpu_write_release(bo);
		break;
	default:
		BUG();
	}
}

static void ttm_pl_fill_rep(struct ttm_buffer_object *bo,
			    struct ttm_pl_rep *rep)
{
	struct ttm_bo_user_object *user_bo =
		container_of(bo, struct ttm_bo_user_object, bo);

	rep->gpu_offset = bo->offset;
	rep->bo_size = bo->num_pages << PAGE_SHIFT;
	rep->map_handle = bo->addr_space_offset;
	rep->placement = bo->mem.placement;
	rep->handle = user_bo->base.hash.key;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	rep->sync_object_arg = (uint32_t)(unsigned long)bo->sync_obj_arg;
#endif
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
/* FIXME Copy from upstream TTM */
static inline size_t ttm_bo_size(struct ttm_bo_global *glob,
				 unsigned long num_pages)
{
	size_t page_array_size = (num_pages * sizeof(void *) + PAGE_SIZE - 1) &
				 PAGE_MASK;

	return glob->ttm_bo_size + 2 * page_array_size;
}
#endif /* if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)) */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
/* FIXME Copy from upstream TTM "ttm_bo_create", upstream TTM does not export this, so copy it here */
static int ttm_bo_create_private(struct ttm_bo_device *bdev,
				 unsigned long size,
				 enum ttm_bo_type type,
				 struct ttm_placement *placement,
				 uint32_t page_alignment,
				 unsigned long buffer_start,
				 bool interruptible,
				 struct file *persistent_swap_storage,
				 struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo;
	struct ttm_mem_global *mem_glob = bdev->glob->mem_glob;
	int ret;

	size_t acc_size =
		ttm_bo_size(bdev->glob, (size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false);
	if (unlikely(ret != 0))
		return ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);

	if (unlikely(bo == NULL)) {
		ttm_mem_global_free(mem_glob, acc_size);
		return -ENOMEM;
	}

	ret = ttm_bo_init(bdev, bo, size, type, placement, page_alignment,
			  buffer_start, interruptible,
			  persistent_swap_storage, acc_size, NULL);
	if (likely(ret == 0))
		*p_bo = bo;

	return ret;
}
#endif /* if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)) */

int psb_ttm_bo_check_placement(struct ttm_buffer_object *bo,
			       struct ttm_placement *placement)
{
	int i;

	for (i = 0; i < placement->num_placement; i++) {
		if (!capable(CAP_SYS_ADMIN)) {
			if (placement->placement[i] & TTM_PL_FLAG_NO_EVICT) {
				printk(KERN_ERR TTM_PFX "Need to be root to "
				       "modify NO_EVICT status.\n");
				return -EINVAL;
			}
		}
	}
	for (i = 0; i < placement->num_busy_placement; i++) {
		if (!capable(CAP_SYS_ADMIN)) {
			if (placement->busy_placement[i] & TTM_PL_FLAG_NO_EVICT) {
				printk(KERN_ERR TTM_PFX "Need to be root to "
				       "modify NO_EVICT status.\n");
				return -EINVAL;
			}
		}
	}
	return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
int ttm_buffer_object_create(struct ttm_bo_device *bdev,
			     unsigned long size,
			     enum ttm_bo_type type,
			     uint32_t flags,
			     uint32_t page_alignment,
			     unsigned long buffer_start,
			     bool interruptible,
			     struct file *persistent_swap_storage,
			     struct ttm_buffer_object **p_bo)
{
	struct ttm_placement placement = default_placement;
	int ret;

	if ((flags & TTM_PL_MASK_CACHING) == 0)
		flags |= TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED;

	placement.num_placement = 1;
	placement.placement = &flags;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
	ret = ttm_bo_create_private(bdev, size, type, &placement,
		page_alignment, buffer_start, interruptible,
		persistent_swap_storage, p_bo);
#else
	ret = ttm_bo_create(bdev, size, type, &placement, page_alignment,
		buffer_start, interruptible, persistent_swap_storage, p_bo);
#endif

	return ret;
}
#else
int ttm_buffer_object_create(struct ttm_bo_device *bdev,
			     unsigned long size,
			     enum ttm_bo_type type,
			     uint32_t flags,
			     uint32_t page_alignment,
			     bool interruptible,
			     struct file *persistent_swap_storage,
			     struct ttm_buffer_object **p_bo)
{
	struct ttm_placement placement = default_placement;
	int ret;

	if ((flags & TTM_PL_MASK_CACHING) == 0)
		flags |= TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED;

	placement.num_placement = 1;
	placement.placement = &flags;

	ret = ttm_bo_create(bdev, size, type, &placement, page_alignment,
		interruptible, persistent_swap_storage, p_bo);

	return ret;
}
#endif

static bool ttm_pfn_sanity_check(struct page **pages, int npages, int dma_bit_mask)
{
	int i;
	for (i = 0; i < npages; ++i) {
		unsigned long phyaddr = page_to_pfn(pages[i]) << PAGE_SHIFT;
		if (phyaddr > DMA_BIT_MASK(dma_bit_mask))
			return false;
	}
	return true;
}
int ttm_pl_create_ioctl(struct ttm_object_file *tfile,
			struct ttm_bo_device *bdev,
			struct ttm_lock *lock, void *data)
{
	union ttm_pl_create_arg *arg = data;
	struct ttm_pl_create_req *req = &arg->req;
	struct ttm_pl_rep *rep = &arg->rep;
	struct ttm_buffer_object *bo;
	struct ttm_buffer_object *tmp;
	struct ttm_bo_user_object *user_bo;
	uint32_t flags;
	int ret = 0;
	struct ttm_mem_global *mem_glob = bdev->glob->mem_glob;
	struct ttm_placement placement = default_placement;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
	size_t acc_size =
		ttm_pl_size(bdev, (req->size + PAGE_SIZE - 1) >> PAGE_SHIFT);
#else
	size_t acc_size = ttm_bo_acc_size(bdev, req->size,
		sizeof(struct ttm_buffer_object));
#endif
	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false);
	if (unlikely(ret != 0))
		return ret;

	flags = req->placement;
	user_bo = kzalloc(sizeof(*user_bo), GFP_KERNEL);
	if (unlikely(user_bo == NULL)) {
		ttm_mem_global_free(mem_glob, acc_size);
		return -ENOMEM;
	}

	bo = &user_bo->bo;
	ret = ttm_read_lock(lock, true);
	if (unlikely(ret != 0)) {
		ttm_mem_global_free(mem_glob, acc_size);
		kfree(user_bo);
		return ret;
	}

	placement.num_placement = 1;
	placement.placement = &flags;

	if ((flags & TTM_PL_MASK_CACHING) == 0)
		flags |=  TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	ret = ttm_bo_init(bdev, bo, req->size,
			  ttm_bo_type_device, &placement,
			  req->page_alignment, 0, true,
			  NULL, acc_size, NULL, &ttm_bo_user_destroy);
#else
	ret = ttm_bo_init(bdev, bo, req->size,
			  ttm_bo_type_device, &placement,
			  req->page_alignment, true,
			  NULL, acc_size, NULL, &ttm_bo_user_destroy);
#endif
	ttm_read_unlock(lock);
	/*
	 * Note that the ttm_buffer_object_init function
	 * would've called the destroy function on failure!!
	 */

	if (unlikely(ret != 0))
		goto out;

	tmp = ttm_bo_reference(bo);
	ret = ttm_base_object_init(tfile, &user_bo->base,
				   flags & TTM_PL_FLAG_SHARED,
				   ttm_buffer_type,
				   &ttm_bo_user_release,
				   &ttm_bo_user_ref_release);
	if (unlikely(ret != 0))
		goto out_err;

	ret = ttm_bo_reserve(bo, true, false, false, 0);
	if (unlikely(ret != 0))
		goto out_err;
	ttm_pl_fill_rep(bo, rep);
	ttm_bo_unreserve(bo);
	ttm_bo_unref(&bo);
out:
	return 0;
out_err:
	ttm_bo_unref(&tmp);
	ttm_bo_unref(&bo);
	return ret;
}

int ttm_pl_dmabuf_create_ioctl(struct ttm_object_file *tfile,
			   struct ttm_bo_device *bdev,
			   struct ttm_lock *lock, void *data)
{
	union ttm_pl_create_dmabuf_arg *arg = data;
	struct ttm_pl_create_dmabuf_req *req = &arg->req;
	struct ttm_pl_rep *rep = &arg->rep;
	struct ttm_buffer_object *bo;
	struct ttm_buffer_object *tmp;
	struct ttm_bo_user_object *user_bo;
	struct drm_psb_private *dev_priv;
	struct dma_buf *psDmaBuf;
	struct dma_buf_attachment *psAttachment;
	struct page **pages;
	int npages;
	size_t bo_size;
	int32_t fd = req->dmabuf_fd;
	uint32_t flags;
	int ret = 0;
	bool pfn_check = true;
	size_t acc_size;
	struct ttm_mem_global *mem_glob = bdev->glob->mem_glob;
	struct ttm_placement placement = default_placement;
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0))
	struct sg_table *sg = NULL;
#endif

	if (req->dmabuf_fd < 0) {
		printk(KERN_ERR "Dma-buf FD invalid: %d\n", req->dmabuf_fd);
		return -ENOENT;
	}

	dev_priv = container_of(bdev, struct drm_psb_private, bdev);
	if ((dev_priv == NULL) || (dev_priv->dev == NULL)) {
		printk(KERN_ERR "failed to get dev_priv\n");
		return -ENODEV;
	}

	flags = req->placement;
	user_bo = kzalloc(sizeof(*user_bo), GFP_KERNEL);
	if (unlikely(user_bo == NULL)) {
		dev_err(dev_priv->dev->dev, "%s: Failed to allocate dmabuf_bo\n", __func__);
		return -ENOMEM;
	}

	ret = ttm_read_lock(lock, true);
	if (unlikely(ret != 0)) {
		dev_err(dev_priv->dev->dev, "%s: Failed to read_lock: %d\n", __func__, ret);
		goto out_err_kfree;
	}
	bo = &user_bo->bo;

	placement.num_placement = 1;
	placement.placement = &flags;

	psDmaBuf = dma_buf_get(fd);
	if (IS_ERR(psDmaBuf)) {
		ret = PTR_ERR(psDmaBuf);
		dev_err(dev_priv->dev->dev, "failed to get DMA_BUF from Fd: %d\n", ret);
		ttm_read_unlock(lock);
		goto out_err_kfree;
	}
	psAttachment = dma_buf_attach(psDmaBuf, dev_priv->dev->dev);
	if (IS_ERR(psAttachment)) {
		ret = PTR_ERR(psAttachment);
		dev_err(dev_priv->dev->dev, "failed to get attachment from dma_buf: %d\n", ret);
		ttm_read_unlock(lock);
		goto out_err_put_dmabuf;
	}
	sg = dma_buf_map_attachment(psAttachment, DMA_NONE);
	if (IS_ERR(sg)) {
		ret = PTR_ERR(sg);
		dev_err(dev_priv->dev->dev, "failed to get sg from DMA_BUF: %d\n", ret);
		ttm_read_unlock(lock);
		goto out_err_detach_dmabuf;
	}

	/* don't trust app privided size, use dmabuf actual size
	 */
	if (req->size < psDmaBuf->size) {
		dev_warn(dev_priv->dev->dev, "%s: DMABUF actual size %zu is larger "
			"than requested %llu, use actual size\n",
			__func__, psDmaBuf->size, req->size);
		bo_size = psDmaBuf->size;
	}
	else
		bo_size = req->size;
	npages = (bo_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
	acc_size = ttm_pl_size(bdev, npages);
#else
	acc_size = ttm_bo_acc_size(bdev, bo_size,
		sizeof(struct ttm_buffer_object));
#endif
	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false);
	if (unlikely(ret != 0)) {
		dev_err(dev_priv->dev->dev, "failed ttm_mem_global_alloc: %d\n", ret);
		ttm_read_unlock(lock);
		goto out_err_unmap_dmabuf;
	}

	pages = drm_malloc_ab(npages, sizeof(struct page*));
	if (!pages) {
		dev_err(dev_priv->dev->dev, "failed to malloc page pointer array\n");
		ret = -ENOMEM;
		ttm_read_unlock(lock);
		goto out_err_ttm_free;
	}
	user_bo->pages = pages;

	if (pfn_check) {
		if (pages) {
			ret = drm_prime_sg_to_page_addr_arrays(sg, pages, NULL, npages);
			if (!ret) {
				if (!ttm_pfn_sanity_check(pages, npages, PSB_TTM_DMA_BIT_MASK)) {
					dev_warn(dev_priv->dev->dev, "%s: PFN check failed!\n", __func__);
					ttm_read_unlock(lock);
					goto out_free_pages;
				}
			}
			else
				dev_warn(dev_priv->dev->dev, "%s: skipped PFN check due to err\n", __func__);
		}
		else
			dev_warn(dev_priv->dev->dev, "%s: skipped PFN check due to err\n", __func__);

	}

	user_bo->dmabuf.import = psAttachment;
	user_bo->dmabuf.export = NULL;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0))
	ret = ttm_bo_init(bdev,
			  bo,
			  bo_size,
			  TTM_HACK_WORKAROUND_ttm_bo_type_user,
			  &placement,
			  req->page_alignment,
			  req->user_address,
			  true,
			  NULL,
			  acc_size,
			  NULL,
			  &ttm_bo_user_destroy);
#elif (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	ret = ttm_bo_init(bdev,
			  bo,
			  bo_size,
			  ttm_bo_type_sg,
			  &placement,
			  req->page_alignment,
			  req->user_address,
			  true,
			  NULL,
			  acc_size,
			  sg,
			  &ttm_ub_bo_user_destroy);
#else
	ret = ttm_bo_init(bdev,
			  bo,
			  bo_size,
			  ttm_bo_type_sg,
			  &placement,
			  req->page_alignment,
			  true,
			  NULL,
			  acc_size,
			  sg,
			  &ttm_ub_bo_user_destroy);
#endif

	/*
	 * Note that the ttm_buffer_object_init function
	 * would've called the destroy function on failure!!
	 */
	ttm_read_unlock(lock);
	if (unlikely(ret != 0)) {
		goto out_free_pages;
	}

	tmp = ttm_bo_reference(bo);
	ret = ttm_base_object_init(tfile, &user_bo->base,
				   flags & TTM_PL_FLAG_SHARED,
				   ttm_buffer_type,
				   &ttm_bo_user_release,
				   &ttm_bo_user_ref_release);
	if (unlikely(ret != 0))
		goto out_err_unref;

	ret = ttm_bo_reserve(bo, true, false, false, 0);
	if (unlikely(ret != 0))
		goto out_err_unref;
	ttm_pl_fill_rep(bo, rep);
	ttm_bo_unreserve(bo);
	ttm_bo_unref(&bo);
	return 0;

out_err_unref:
	ttm_bo_unref(&tmp);
	ttm_bo_unref(&bo);
out_free_pages:
	drm_free_large(pages);
out_err_ttm_free:
	ttm_mem_global_free(mem_glob, acc_size);
out_err_unmap_dmabuf:
	dma_buf_unmap_attachment(psAttachment, sg, DMA_NONE);
out_err_detach_dmabuf:
	dma_buf_detach(psDmaBuf, psAttachment);
out_err_put_dmabuf:
	dma_buf_put(psDmaBuf);
out_err_kfree:
	kfree(user_bo);
	return ret;
}

int ttm_pl_userptr_create_ioctl(struct ttm_object_file *tfile,
			   struct ttm_bo_device *bdev,
			   struct ttm_lock *lock, void *data)
{
	union ttm_pl_create_userptr_arg *arg = data;
	struct ttm_pl_create_userptr_req *req = &arg->req;
	struct ttm_pl_rep *rep = &arg->rep;
	struct ttm_buffer_object *bo;
	struct ttm_buffer_object *tmp;
	struct ttm_bo_user_object *user_bo;
	uint32_t flags;
	bool pfn_check = true;
	int ret = 0;
	struct ttm_mem_global *mem_glob = bdev->glob->mem_glob;
	struct ttm_placement placement = default_placement;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
	size_t acc_size =
		ttm_pl_size(bdev, (req->size + PAGE_SIZE - 1) >> PAGE_SHIFT);
#else
	size_t acc_size = ttm_bo_acc_size(bdev, req->size,
		sizeof(struct ttm_buffer_object));
#endif
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0))
	unsigned int page_nr = 0;
	struct vm_area_struct *vma = NULL;
	struct sg_table *sg = NULL;
	unsigned long num_pages = 0;
	struct page **pages = 0;
	unsigned long before_flags;
#endif

	if (req->user_address & ~PAGE_MASK) {
		printk(KERN_ERR "User pointer buffer need page alignment\n");
		return -EFAULT;
	}

	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false);
	if (unlikely(ret != 0))
		return ret;

	flags = req->placement;
	user_bo = kzalloc(sizeof(*user_bo), GFP_KERNEL);
	if (unlikely(user_bo == NULL)) {
		ret = -ENOMEM;
		goto out_err_ttm_free;
	}
	user_bo->dmabuf.import = NULL;
	user_bo->dmabuf.export = NULL;
	ret = ttm_read_lock(lock, true);
	if (unlikely(ret != 0)) {
		goto out_err_kfree_bo;
	}
	bo = &user_bo->bo;

	placement.num_placement = 1;
	placement.placement = &flags;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0))
	num_pages = (req->size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	pages = drm_malloc_ab(num_pages, sizeof(struct page*));
	if (unlikely(pages == NULL)) {
		ret = -ENOMEM;
		printk(KERN_ERR "kzalloc pages failed\n");
		ttm_read_unlock(lock);
		goto out_err_kfree_bo;
	}
	user_bo->pages = pages;

	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, req->user_address);
	if (unlikely(vma == NULL)) {
		printk(KERN_ERR "find_vma failed\n");
		up_read(&current->mm->mmap_sem);
		drm_free_large(pages);
		ttm_read_unlock(lock);
		goto out_err_kfree_bo;
	}
	before_flags = vma->vm_flags;
	if (vma->vm_flags & (VM_IO | VM_PFNMAP))
		vma->vm_flags = vma->vm_flags & ((~VM_IO) & (~VM_PFNMAP));
	page_nr = get_user_pages(current, current->mm,
				 req->user_address,
				 (int)(num_pages), 1, 0, pages,
				 NULL);
	vma->vm_flags = before_flags;
	up_read(&current->mm->mmap_sem);

	/* can be written by caller, not forced */
	if (unlikely(page_nr < num_pages)) {
		printk(KERN_ERR "get_user_pages err.\n");
		drm_free_large(pages);
		ttm_read_unlock(lock);
		goto out_err_kfree_bo;
	}
	sg = drm_prime_pages_to_sg(pages, num_pages);
	if (IS_ERR(sg)) {
		printk(KERN_ERR "drm_prime_pages_to_sg err.\n");
		drm_free_large(pages);
		ttm_read_unlock(lock);
		goto out_err_kfree_bo;
	}

	if (pfn_check && !ttm_pfn_sanity_check(pages, num_pages, PSB_TTM_DMA_BIT_MASK)) {
		printk(KERN_ERR "PFN check failed!\n");
		ttm_read_unlock(lock);
		drm_free_large(pages);
		goto out_err_kfree_bo;
	}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0))
	ret = ttm_bo_init(bdev,
			  bo,
			  req->size,
			  TTM_HACK_WORKAROUND_ttm_bo_type_user,
			  &placement,
			  req->page_alignment,
			  req->user_address,
			  true,
			  NULL,
			  acc_size,
			  NULL,
			  &ttm_bo_user_destroy);
#elif (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	ret = ttm_bo_init(bdev,
			  bo,
			  req->size,
			  ttm_bo_type_sg,
			  &placement,
			  req->page_alignment,
			  req->user_address,
			  true,
			  NULL,
			  acc_size,
			  sg,
			  &ttm_ub_bo_user_destroy);
#else
	ret = ttm_bo_init(bdev,
			  bo,
			  req->size,
			  ttm_bo_type_sg,
			  &placement,
			  req->page_alignment,
			  true,
			  NULL,
			  acc_size,
			  sg,
			  &ttm_ub_bo_user_destroy);
#endif

	/*
	 * Note that the ttm_buffer_object_init function
	 * would've called the destroy function on failure!!
	 */
	ttm_read_unlock(lock);
	if (unlikely(ret != 0))
		goto out_err_kfree_bo;

	tmp = ttm_bo_reference(bo);
	ret = ttm_base_object_init(tfile, &user_bo->base,
				   flags & TTM_PL_FLAG_SHARED,
				   ttm_buffer_type,
				   &ttm_bo_user_release,
				   &ttm_bo_user_ref_release);
	if (unlikely(ret != 0))
		goto out_err_unref;

	ret = ttm_bo_reserve(bo, true, false, false, 0);
	if (unlikely(ret != 0))
		goto out_err_unref;
	ttm_pl_fill_rep(bo, rep);
	ttm_bo_unreserve(bo);
	ttm_bo_unref(&bo);
	return 0;
out_err_unref:
	ttm_bo_unref(&tmp);
	ttm_bo_unref(&bo);
out_err_kfree_bo:
	kfree(user_bo);
out_err_ttm_free:
	ttm_mem_global_free(mem_glob, acc_size);
	return ret;
}

int ttm_pl_reference_ioctl(struct ttm_object_file *tfile, void *data)
{
	union ttm_pl_reference_arg *arg = data;
	struct ttm_pl_rep *rep = &arg->rep;
	struct ttm_bo_user_object *user_bo;
	struct ttm_buffer_object *bo;
	struct ttm_base_object *base;
	int ret;

	user_bo = ttm_bo_user_lookup(tfile, arg->req.handle);
	if (unlikely(user_bo == NULL)) {
		printk(KERN_ERR "Could not reference buffer object.\n");
		return -EINVAL;
	}

	bo = &user_bo->bo;
	ret = ttm_ref_object_add(tfile, &user_bo->base, TTM_REF_USAGE, NULL);
	if (unlikely(ret != 0)) {
		printk(KERN_ERR
		       "Could not add a reference to buffer object.\n");
		goto out;
	}

	ret = ttm_bo_reserve(bo, true, false, false, 0);
	if (unlikely(ret != 0))
		goto out;
	ttm_pl_fill_rep(bo, rep);
	ttm_bo_unreserve(bo);

out:
	base = &user_bo->base;
	ttm_base_object_unref(&base);
	return ret;
}

int ttm_pl_unref_ioctl(struct ttm_object_file *tfile, void *data)
{
	struct ttm_pl_reference_req *arg = data;

	return ttm_ref_object_base_unref(tfile, arg->handle, TTM_REF_USAGE);
}

int ttm_pl_synccpu_ioctl(struct ttm_object_file *tfile, void *data)
{
	struct ttm_pl_synccpu_arg *arg = data;
	struct ttm_bo_user_object *user_bo;
	struct ttm_buffer_object *bo;
	struct ttm_base_object *base;
	bool existed;
	int ret;

	switch (arg->op) {
	case TTM_PL_SYNCCPU_OP_GRAB:
		user_bo = ttm_bo_user_lookup(tfile, arg->handle);
		if (unlikely(user_bo == NULL)) {
			printk(KERN_ERR
			       "Could not find buffer object for synccpu.\n");
			return -EINVAL;
		}
		bo = &user_bo->bo;
		base = &user_bo->base;
		ret = ttm_bo_synccpu_write_grab(bo,
						arg->access_mode &
						TTM_PL_SYNCCPU_MODE_NO_BLOCK);
		if (unlikely(ret != 0)) {
			ttm_base_object_unref(&base);
			goto out;
		}
		ret = ttm_ref_object_add(tfile, &user_bo->base,
					 TTM_REF_SYNCCPU_WRITE, &existed);
		if (existed || ret != 0)
			ttm_bo_synccpu_write_release(bo);
		ttm_base_object_unref(&base);
		break;
	case TTM_PL_SYNCCPU_OP_RELEASE:
		ret = ttm_ref_object_base_unref(tfile, arg->handle,
						TTM_REF_SYNCCPU_WRITE);
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	return ret;
}

int ttm_pl_setstatus_ioctl(struct ttm_object_file *tfile,
			   struct ttm_lock *lock, void *data)
{
	union ttm_pl_setstatus_arg *arg = data;
	struct ttm_pl_setstatus_req *req = &arg->req;
	struct ttm_pl_rep *rep = &arg->rep;
	struct ttm_buffer_object *bo;
	struct ttm_bo_device *bdev;
	struct ttm_placement placement = default_placement;
	uint32_t flags[2];
	int ret;

	bo = ttm_buffer_object_lookup(tfile, req->handle);
	if (unlikely(bo == NULL)) {
		printk(KERN_ERR
		       "Could not find buffer object for setstatus.\n");
		return -EINVAL;
	}

	bdev = bo->bdev;

	ret = ttm_read_lock(lock, true);
	if (unlikely(ret != 0))
		goto out_err0;

	ret = ttm_bo_reserve(bo, true, false, false, 0);
	if (unlikely(ret != 0))
		goto out_err1;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	ret = ttm_bo_wait_cpu(bo, false);
	if (unlikely(ret != 0))
		goto out_err2;
#endif

	flags[0] = req->set_placement;
	flags[1] = req->clr_placement;

	placement.num_placement = 2;
	placement.placement = flags;

	/* spin_lock(&bo->lock); */ /* Already get reserve lock */

	ret = psb_ttm_bo_check_placement(bo, &placement);
	if (unlikely(ret != 0))
		goto out_err2;

	placement.num_placement = 1;
	flags[0] = (req->set_placement | bo->mem.placement) & ~req->clr_placement;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
	ret = ttm_bo_validate(bo, &placement, true, false, false);
#else
	ret = ttm_bo_validate(bo, &placement, true, false);
#endif
	if (unlikely(ret != 0))
		goto out_err2;

	ttm_pl_fill_rep(bo, rep);
out_err2:
	/* spin_unlock(&bo->lock); */
	ttm_bo_unreserve(bo);
out_err1:
	ttm_read_unlock(lock);
out_err0:
	ttm_bo_unref(&bo);
	return ret;
}

static int psb_ttm_bo_block_reservation(struct ttm_buffer_object *bo, bool interruptible,
					bool no_wait)
{
	int ret;

	while (unlikely(atomic_cmpxchg(&bo->reserved, 0, 1) != 0)) {
		if (no_wait)
			return -EBUSY;
		else if (interruptible) {
			ret = wait_event_interruptible
			      (bo->event_queue, atomic_read(&bo->reserved) == 0);
			if (unlikely(ret != 0))
				return -ERESTART;
		} else {
			wait_event(bo->event_queue,
				   atomic_read(&bo->reserved) == 0);
		}
	}
	return 0;
}

static void psb_ttm_bo_unblock_reservation(struct ttm_buffer_object *bo)
{
	atomic_set(&bo->reserved, 0);
	wake_up_all(&bo->event_queue);
}

int ttm_pl_waitidle_ioctl(struct ttm_object_file *tfile, void *data)
{
	struct ttm_pl_waitidle_arg *arg = data;
	struct ttm_buffer_object *bo;
	int ret;

	bo = ttm_buffer_object_lookup(tfile, arg->handle);
	if (unlikely(bo == NULL)) {
		printk(KERN_ERR "Could not find buffer object for waitidle.\n");
		return -EINVAL;
	}

	ret =
		psb_ttm_bo_block_reservation(bo, true,
					     arg->mode & TTM_PL_WAITIDLE_MODE_NO_BLOCK);
	if (unlikely(ret != 0))
		goto out;
	spin_lock(&bo->bdev->fence_lock);
	ret = ttm_bo_wait(bo,
			  arg->mode & TTM_PL_WAITIDLE_MODE_LAZY,
			  true, arg->mode & TTM_PL_WAITIDLE_MODE_NO_BLOCK);
	spin_unlock(&bo->bdev->fence_lock);
	psb_ttm_bo_unblock_reservation(bo);
out:
	ttm_bo_unref(&bo);
	return ret;
}

int ttm_pl_verify_access(struct ttm_buffer_object *bo,
			 struct ttm_object_file *tfile)
{
	struct ttm_bo_user_object *ubo;

	/*
	 * Check bo subclass.
	 */

	if (unlikely(bo->destroy != &ttm_bo_user_destroy
		&& bo->destroy != &ttm_ub_bo_user_destroy))
		return -EPERM;

	ubo = container_of(bo, struct ttm_bo_user_object, bo);
	if (likely(ubo->base.shareable || ubo->base.tfile == tfile))
		return 0;

	return -EPERM;
}
