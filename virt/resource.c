/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
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

#include <minos/minos.h>
#include <minos/list.h>
#include <minos/of.h>
#include <virt/vm.h>
#include <virt/vdev.h>
#include <virt/virq_chip.h>
#include <minos/mm.h>
#include <virt/vmm.h>
#include <minos/irq.h>
#include <virt/vmbox.h>
#include <minos/platform.h>

static void *virqchip_start;
static void *virqchip_end;
static void *vdev_start;
static void *vdev_end;

static int create_vm_vdev_of(struct vm *vm, struct device_node *node)
{
	vdev_init_t func;

	pr_info("%s %s\n", __func__, node->name);

	if (!node->compatible)
		return -EINVAL;

	func = (vdev_init_t)of_device_node_match(node,
			vdev_start, vdev_end);
	if (!func) {
		pr_warn("can not find vdev-%s\n", node->compatible);
		return -ENOENT;
	}

	/* create the vdev */
	func(vm, node);

	return 0;
}

static void *create_vm_irqchip_of(struct device_node *node, void *arg)
{
	struct vm *vm = (struct vm *)arg;
	vdev_init_t func;

	if (node->class != DT_CLASS_IRQCHIP)
		return NULL;
	if (vm->virq_chip != NULL) {
		pr_warn("virq chip has been created\n");
		return NULL;
	}

	func = (vdev_init_t)of_device_node_match(node,
			virqchip_start, virqchip_end);
	if (!func) {
		pr_warn("virq-chip :%s not found\n", node->compatible ?
				node->compatible : "Null");
		return NULL;
	}

	vm->virq_chip = func(vm, (void *)node);
	if (!vm->virq_chip)
		return NULL;

	node->class = DT_CLASS_VIRQCHIP;
	pr_info("create virq_chip %s successfully\n", node->compatible);

	return node;
}

int parse_vm_info_of(struct device_node *node, struct vmtag *vmtag)
{
	/* 64bit virtual machine default */
	memset(vmtag, 0, sizeof(*vmtag));
	vmtag->flags |= VM_FLAGS_64BIT;

	of_get_u32_array(node, "vmid", &vmtag->vmid, 1);
	of_get_string(node, "vm_name", vmtag->name, 32);
	of_get_string(node, "type", vmtag->os_type, 16);
	of_get_u32_array(node, "vcpus", (uint32_t *)&vmtag->nr_vcpu, 1);
	of_get_u64_array(node, "entry", (uint64_t *)&vmtag->entry, 1);
	of_get_u32_array(node, "vcpu_affinity",
			vmtag->vcpu_affinity, vmtag->nr_vcpu);
	of_get_u64_array(node, "setup_data", (uint64_t *)&vmtag->setup_data, 1);
	of_get_u64_array(node, "image_copy", vmtag->image_copy,
			 ARRAY_SIZE(vmtag->image_copy));

	if (of_get_bool(node, "vm_32bit"))
		vmtag->flags &= ~VM_FLAGS_64BIT;

	if (of_get_bool(node, "native_wfi"))
		vmtag->flags |= VM_FLAGS_NATIVE_WFI;

	if (of_get_bool(node, "no_of_resource"))
		vmtag->flags |= VM_FLAGS_NO_OF_RESOURCE;

	return 0;
}

int vm_get_device_irq_index(struct vm *vm, struct device_node *node,
		uint32_t *irq, unsigned long *flags, int index)
{
	int irq_cells, len, i;
	of32_t *value;
	uint32_t irqv[4];

	if (!node)
		return -EINVAL;

	value = (of32_t *)of_getprop(node, "interrupts", &len);
	if (!value || (len < sizeof(of32_t)))
		return -ENOENT;

	irq_cells = of_n_interrupt_cells(node);
	if (irq_cells == 0) {
		pr_err("bad irqcells - %s\n", node->name);
		return -ENOENT;
	}

	pr_debug("interrupt-cells %d\n", irq_cells);

	len = len / sizeof(of32_t);
	if (index >= len)
		return -ENOENT;

	value += (index * irq_cells);
	for (i = 0; i < irq_cells; i++)
		irqv[i] = of32_to_cpu(*value++);

	if (vm && vm->virq_chip)
		return vm->virq_chip->xlate(node, irqv, irq_cells, irq, flags);
	else
		return irq_xlate(node, irqv, irq_cells, irq, flags);

	return -EINVAL;
}

static int create_pdev_virq_of(struct vm *vm, struct device_node *node)
{
	of32_t *val;
	of32_t *virq_affinity = NULL;
	uint32_t irqs[4], irq, hw_irq = 0;
	int i, j, nr_icells, hw = 0;
	int len, len_aff, ret;
	struct virq_chip *vc = vm->virq_chip;
	unsigned long type;

	if (node->class == DT_CLASS_VIRQCHIP)
		return 0;

	val = (of32_t *)of_getprop(node, "interrupts", &len);
	if (!val || (len < 4))
		return 0;

	/* get the irq count for the device */
	nr_icells = of_n_interrupt_cells(node);
	if (nr_icells == 0)
		return 0;

	len = len / 4;
	if (vm_is_native(vm))
		hw = 1;
	else {
		virq_affinity = (of32_t *)of_getprop(node, "virq_affinity", &len_aff);
		if (virq_affinity && (len_aff == len))
			hw = 1;
	}

	for (i = 0; i < len; i += nr_icells) {
		for (j = 0; j < nr_icells; j++)
			irqs[j] = of32_to_cpu(val[i + j]);

		ret = vc->xlate(node, irqs, nr_icells, &irq, &type);
		if (ret)
			continue;

		if (hw) {
			if (vm_is_native(vm))
				hw_irq = irq;
			else
				hw_irq = of32_to_cpu(virq_affinity[i / nr_icells]);
		}

		/* register the hardware irq for vm */
		pr_info("[VM%d VIRQ] %d->%d %s\n", vm->vmid,
				irq, hw_irq, node->name);
		request_hw_virq(vm, irq, hw_irq, 0);
	}

	return 0;
}

static int create_pdev_iomem_of(struct vm *vm, struct device_node *node)
{
	uint64_t addr, size;
	int i, nr_addr, ret;

	/* get the count of memory region for the device */
	nr_addr = of_n_addr_count(node);

	for (i = 0; i < nr_addr; i++) {
		ret = of_translate_address_index(node, &addr, &size, i);
		if (ret) {
			pr_warn("bad address index %d for %s\n", i, node->name);
			continue;
		}

		if (size == 0)
			continue;

		if (vm_is_hvm(vm)) {
			if (!platform_iomem_valid(addr))
				continue;
		}

		/* map the physical memory for vm */
		pr_info("[VM%d IOMEM] 0x%x->0x%x 0x%x %s\n", vm->vmid,
				addr, addr, size, node->name);
		split_vmm_area(&vm->mm, addr, size, VM_IO | VM_MAP_PT);

		/* virqchip do not map the virtual address */
		if (node->class != DT_CLASS_VIRQCHIP)
			create_guest_mapping(&vm->mm, addr,
					addr, size, VM_IO | VM_MAP_PT);
	}

	return 0;
}

static int create_vm_pdev_of(struct vm *vm, struct device_node *node)
{
	int ret = 0;

	ret += create_pdev_iomem_of(vm, node);
	ret += create_pdev_virq_of(vm, node);

	if (ret)
		pr_notice("create %s fail\n", node->name);

	return ret;
}

static int create_vm_res_of(struct vm *vm, struct device_node *node)
{
	return 0;
}

static void *__create_vm_resource_of(struct device_node *node, void *arg)
{
	struct vm *vm = (struct vm *)arg;

	switch(node->class) {
	case DT_CLASS_IRQCHIP:
	case DT_CLASS_VIRQCHIP:
	case DT_CLASS_PCI_BUS:
	case DT_CLASS_PDEV:
		create_vm_pdev_of(vm, node);
		break;
	case DT_CLASS_TIMER:
	case DT_CLASS_VDEV:
		create_vm_vdev_of(vm, node);
		break;
	case DT_CLASS_VM:
		create_vm_res_of(vm, node);
		break;
	default:
		break;
	}

	return NULL;
}

int create_vm_resource_of(struct vm *vm, void *data)
{
	struct device_node *node;

	if (!vm || (vm->flags & VM_FLAGS_NO_OF_RESOURCE))
		return -EINVAL;

	node = of_parse_device_tree(data);
	if (!node) {
		pr_err("invaild setup data for vm-%d\n", vm->vmid);
		return -EINVAL;
	}

	/*
	 * first of all need to create the virq controller
	 * of this vm
	 */
	of_iterate_all_node(node, create_vm_irqchip_of, vm);
	if (!vm->virq_chip) {
		if (vm_is_native(vm))
			panic("can not create virq chip for vm\n");
		else
			pr_err("create virq chip failed for vm\n");
		return -ENOENT;
	}

	of_iterate_all_node_loop(node, __create_vm_resource_of, vm);

	/* here we can free all the device node to save memory */
	of_release_all_node(node);

	return 0;
}

static int create_vm_virqchip_common(struct vm *vm, struct device_node *node)
{
	char name[32];
	struct device_node *virq_node;

	memset(name, 0, 32);
	sprintf(name, "virq_chip_vm%d", vm->vmid);
	virq_node = of_find_node_by_name(node, name);
	if (!virq_node)
		return -ENOENT;

	/* set the class to DT_CLASS_IRQCHIP temp */
	virq_node->class = DT_CLASS_IRQCHIP;
	create_vm_irqchip_of(virq_node, vm);
	if (!vm->virq_chip)
		pr_err("no virq chip for %s\n", vm_name(vm));

	virq_node->class = DT_CLASS_OTHER;

	return 0;
}

static int inline
create_vm_vtimer_common(struct vm *vm, struct device_node *node)
{
	of_get_u32_array(node, "vtimer_irq", &vm->vtimer_virq, 1);
	if ((vm->vtimer_virq > 31) || (vm->vtimer_virq < 16))
		pr_warn("wrong vtimer virq for vm\n");

	return 0;
}

#define IOMEM_SIZE_CELLS	2
#define IOMEM_ADDR_CELLS	2
#define IOMEM_ENTRY_CNT		6
#define IOMEM_ENTRY_SIZE	24

static int create_vm_iomem_common(struct vm *vm, struct device_node *node)
{
	fdt32_t *data;
	int len, i;
	unsigned long vaddr, paddr, size;

	/* vaddr paddr size */
	data = (fdt32_t *)of_getprop(node, "iomem", &len);
	if (!data || (len == 0) || (len % IOMEM_ENTRY_SIZE))
		return -EINVAL;

	len = len / IOMEM_ENTRY_SIZE;

	for (i = 0; i < len; i++) {
		vaddr = fdt32_to_cpu64(data[0], data[1]);
		paddr = fdt32_to_cpu64(data[2], data[3]);
		size = fdt32_to_cpu64(data[4], data[5]);

		split_vmm_area(&vm->mm, vaddr, size, VM_IO);
		create_guest_mapping(&vm->mm, vaddr,
				paddr, size, VM_IO | VM_MAP_PT);

		data += IOMEM_ENTRY_CNT;
	}

	return 0;
}

static int create_vm_virqs_common(struct vm *vm, struct device_node *node)
{
	fdt32_t *data;
	int len, i;
	uint32_t phy, vir;

	data = (fdt32_t *)of_getprop(node, "virqs", &len);
	if (!data || (len == 0) || (len % 8))
		return -EINVAL;

	len = len / (2 * sizeof(uint32_t));

	for (i = 0; i < len; i++) {
		phy = fdt32_to_cpu(data[0]);
		vir = fdt32_to_cpu(data[1]);
		request_hw_virq(vm, vir, phy, 0);
		data += 2;
	}

	return 0;
}

static void *create_vm_vdev_common(struct device_node *node, void *vm)
{
	if (of_get_bool(node, "virtual_device"))
		create_vm_vdev_of(vm, node);

	return NULL;
}

int create_native_vm_resource_common(struct vm *vm)
{
	int ret = 0;
	char name[32];
	struct device_node *node;

	sprintf(name, "vm%d_bdi", vm->vmid);

	node = of_find_node_by_name(vm->dev_node, name);
	if (!node)
		return -ENOENT;

	if (!vm->virq_chip) {
		ret = create_vm_virqchip_common(vm, node);
		if (ret)
			pr_warn("virqchip is not found in hv dts\n");
	}

	if (vm->vtimer_virq <= 0)
		ret += create_vm_vtimer_common(vm, node);

	ret += create_vm_iomem_common(vm, node);
	ret += create_vm_virqs_common(vm, node);

	of_iterate_all_node_loop(node, create_vm_vdev_common, vm);

	return ret;
}

static int __init_text resource_init(void)
{
	extern unsigned char __virqchip_start;
	extern unsigned char __virqchip_end;
	extern unsigned char __vdev_start;
	extern unsigned char __vdev_end;

	virqchip_start = (void *)&__virqchip_start;
	virqchip_end = (void *)&__virqchip_end;
	vdev_start = (void *)&__vdev_start;
	vdev_end = (void *)&__vdev_end;

	return 0;
}
arch_initcall(resource_init);
