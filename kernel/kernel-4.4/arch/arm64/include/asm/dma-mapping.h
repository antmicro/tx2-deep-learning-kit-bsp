/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_DMA_MAPPING_H
#define __ASM_DMA_MAPPING_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/vmalloc.h>

#include <asm-generic/dma-coherent.h>

#include <asm/dma-iommu.h>

#include <xen/xen.h>
#include <asm/xen/hypervisor.h>

#define DMA_ERROR_CODE	(~(dma_addr_t)0)
extern struct dma_map_ops dummy_dma_ops;

extern struct dma_map_ops *dma_ops;
extern struct dma_map_ops coherent_swiotlb_dma_ops;
extern struct dma_map_ops noncoherent_swiotlb_dma_ops;
extern struct dma_map_ops arm_dma_ops;

#define PG_PROT_KERNEL PAGE_KERNEL
#define FLUSH_TLB_PAGE(addr) flush_tlb_kernel_range(addr, PAGE_SIZE)
#define FLUSH_DCACHE_AREA __flush_dcache_area

static inline struct dma_map_ops *__generic_dma_ops(struct device *dev)
{
	if (unlikely(!dev) || !dev->archdata.dma_ops)
		return dma_ops;
	else
		return dev->archdata.dma_ops;
}

static inline struct dma_map_ops *get_dma_ops(struct device *dev)
{
	if (xen_initial_domain())
		return xen_dma_ops;
	else
		return __generic_dma_ops(dev);
}

static inline void set_dma_ops(struct device *dev, struct dma_map_ops *ops)
{
	dev->archdata.dma_ops = ops;
}

static inline int set_arch_dma_coherent_ops(struct device *dev)
{
#ifdef CONFIG_SWIOTLB
	set_dma_ops(dev, &coherent_swiotlb_dma_ops);
#else
	BUG();
#endif
	return 0;
}
#define set_arch_dma_coherent_ops	set_arch_dma_coherent_ops

void set_dummy_dma_ops(struct device *dev);

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			struct iommu_ops *iommu, bool coherent);
#define arch_setup_dma_ops	arch_setup_dma_ops

#ifdef CONFIG_IOMMU_DMA
void arch_teardown_dma_ops(struct device *dev);
#define arch_teardown_dma_ops	arch_teardown_dma_ops
#endif

/* do not use this function in a driver */
static inline bool is_device_dma_coherent(struct device *dev)
{
	if (!dev)
		return false;
	return dev->archdata.dma_coherent;
}

#include <asm-generic/dma-mapping-common.h>

static inline dma_addr_t phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	return (dma_addr_t)paddr;
}

static inline phys_addr_t dma_to_phys(struct device *dev, dma_addr_t dev_addr)
{
	return (phys_addr_t)dev_addr;
}

static inline bool dma_capable(struct device *dev, dma_addr_t addr, size_t size)
{
	if (!dev->dma_mask)
		return 0;

	return addr + size - 1 <= *dev->dma_mask;
}

static inline void dma_mark_clean(void *addr, size_t size)
{
}

#define dma_alloc_coherent(d, s, h, f)	dma_alloc_attrs(d, s, h, f, NULL)
#define dma_alloc_at_coherent(d, s, h, f) dma_alloc_at_attrs(d, s, h, f, NULL)
#define dma_free_coherent(d, s, h, f)	dma_free_attrs(d, s, h, f, NULL)

static inline void *dma_alloc_at_attrs(struct device *dev, size_t size,
				       dma_addr_t *dma_handle, gfp_t flags,
				       struct dma_attrs *attrs)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	void *vaddr;

	if (dma_alloc_from_coherent_attr(dev, size, dma_handle, &vaddr, attrs))
		return vaddr;

	vaddr = ops->alloc(dev, size, dma_handle, flags, attrs);
	debug_dma_alloc_coherent(dev, size, *dma_handle, vaddr);
	return vaddr;
}

/* FIXME: copied from arch/arm
 *
 * This can be called during boot to increase the size of the consistent
 * DMA region above it's default value of 2MB. It must be called before the
 * memory allocator is initialised, i.e. before any core_initcall.
 */
static inline void init_consistent_dma_size(unsigned long size) { }

int arm_dma_supported(struct device *dev, u64 mask);

/* FIXME: copied from arch/arm */

extern int arm_dma_mapping_error(struct device *dev, dma_addr_t dev_addr);

extern int arm_dma_set_mask(struct device *dev, u64 dma_mask);
/**
 * arm_dma_alloc - allocate consistent memory for DMA
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @size: required memory size
 * @handle: bus-specific DMA address
 * @attrs: optinal attributes that specific mapping properties
 *
 * Allocate some memory for a device for performing DMA.  This function
 * allocates pages, and will return the CPU-viewed address, and sets @handle
 * to be the device-viewed address.
 */
extern void *arm_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
			gfp_t gfp, struct dma_attrs *attrs);

/**
 * arm_dma_free - free memory allocated by arm_dma_alloc
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @size: size of memory originally requested in dma_alloc_coherent
 * @cpu_addr: CPU-view address returned from dma_alloc_coherent
 * @handle: device-view address returned from dma_alloc_coherent
 * @attrs: optinal attributes that specific mapping properties
 *
 * Free (and unmap) a DMA buffer previously allocated by
 * arm_dma_alloc().
 *
 * References to memory and mappings associated with cpu_addr/handle
 * during and after this call executing are illegal.
 */
extern void arm_dma_free(struct device *dev, size_t size, void *cpu_addr,
			dma_addr_t handle, struct dma_attrs *attrs);

static inline dma_addr_t
dma_map_linear_attrs(struct device *dev, phys_addr_t pa, size_t size,
			enum dma_data_direction dir, struct dma_attrs *attrs)
{
	void *va = phys_to_virt(pa);
	struct dma_map_ops *ops = get_dma_ops(dev);
	dma_addr_t addr = DMA_ERROR_CODE;

	if (!ops || !ops->map_at) {
		WARN(1, "map_at is not supported\n");
		return DMA_ERROR_CODE;
	}

	addr = ops->map_at(dev, pa, pa, size, dir, attrs);

	if (addr != DMA_ERROR_CODE) {
		kmemcheck_mark_initialized(va, size);
		debug_dma_map_page(dev, virt_to_page(va),
				   (unsigned long)va & ~PAGE_MASK, size,
				   dir, addr, true);
	}
	return addr;
}

/**
 * arm_dma_mmap - map a coherent DMA allocation into user space
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @vma: vm_area_struct describing requested user mapping
 * @cpu_addr: kernel CPU-view address returned from dma_alloc_coherent
 * @handle: device-view address returned from dma_alloc_coherent
 * @size: size of memory originally requested in dma_alloc_coherent
 * @attrs: optinal attributes that specific mapping properties
 *
 * Map a coherent DMA buffer previously allocated by dma_alloc_coherent
 * into user space.  The coherent DMA buffer must not be freed by the
 * driver until the user space mapping has been released.
 */
extern int arm_dma_mmap(struct device *dev, struct vm_area_struct *vma,
			void *cpu_addr, dma_addr_t dma_addr, size_t size,
			struct dma_attrs *attrs);


extern int arm_dma_map_sg(struct device *, struct scatterlist *, int,
		enum dma_data_direction, struct dma_attrs *attrs);
extern void arm_dma_unmap_sg(struct device *, struct scatterlist *, int,
		enum dma_data_direction, struct dma_attrs *attrs);
extern void arm_dma_sync_sg_for_cpu(struct device *, struct scatterlist *, int,
		enum dma_data_direction);
extern void arm_dma_sync_sg_for_device(struct device *, struct scatterlist *,
		int, enum dma_data_direction);
extern int arm_dma_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		struct dma_attrs *attrs);


#ifdef __arch_page_to_dma
#error Please update to __arch_pfn_to_dma
#endif

/*
 * dma_to_pfn/pfn_to_dma/dma_to_virt/virt_to_dma are architecture private
 * functions used internally by the DMA-mapping API to provide DMA
 * addresses. They must not be used by drivers.
 */
#ifndef __arch_pfn_to_dma
static inline dma_addr_t pfn_to_dma(struct device *dev, unsigned long pfn)
{
	return (dma_addr_t)__pfn_to_bus(pfn);
}

static inline unsigned long dma_to_pfn(struct device *dev, dma_addr_t addr)
{
	return __bus_to_pfn(addr);
}

static inline void *dma_to_virt(struct device *dev, dma_addr_t addr)
{
	return (void *)__bus_to_virt((unsigned long)addr);
}

static inline dma_addr_t virt_to_dma(struct device *dev, void *addr)
{
	return (dma_addr_t)__virt_to_bus((unsigned long)(addr));
}
#else
static inline dma_addr_t pfn_to_dma(struct device *dev, unsigned long pfn)
{
	return __arch_pfn_to_dma(dev, pfn);
}

static inline unsigned long dma_to_pfn(struct device *dev, dma_addr_t addr)
{
	return __arch_dma_to_pfn(dev, addr);
}

static inline void *dma_to_virt(struct device *dev, dma_addr_t addr)
{
	return __arch_dma_to_virt(dev, addr);
}

static inline dma_addr_t virt_to_dma(struct device *dev, void *addr)
{
	return __arch_virt_to_dma(dev, addr);
}
#endif

#ifdef CONFIG_ARM_DMA_USE_IOMMU
extern bool device_is_iommuable(struct device *dev);
#else
static inline bool device_is_iommuable(struct device *dev)
{
	return false;
}
#endif

#endif	/* __KERNEL__ */
#endif	/* __ASM_DMA_MAPPING_H */
