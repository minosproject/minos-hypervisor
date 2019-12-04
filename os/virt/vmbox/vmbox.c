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
#include <virt/vmbox.h>
#include <asm/svccc.h>
#include <virt/hypercall.h>
#include <virt/vmm.h>
#include <virt/vm.h>
#include <minos/sched.h>
#include <virt/virq.h>
#include <libfdt/libfdt.h>
#include <virt/virq_chip.h>
#include <minos/of.h>
#include <asm/io.h>

#define VMBOX_MAX_COUNT	16
#define VMBOX_MAX_VQS	4

#define BE_IDX		0
#define FE_IDX		1

#define VMBOX_DEV_VIRTQ_HEADER_SIZE	0x100

struct vmbox_info {
	int owner[2];
	uint32_t id[2];
	uint32_t vqs;
	uint32_t vring_num;
	uint32_t vring_size;
	uint32_t shmem_size;
	unsigned long flags;
	char type[32];
};

struct vmbox_hook {
	char name[32];
	struct list_head list;
	struct vmbox_hook_ops *ops;
};

static LIST_HEAD(vmbox_con_list);
static LIST_HEAD(vmbox_hook_list);

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
} __packed__;

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[];
} __packed__;

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
} __packed__;

struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __packed__;

typedef int (*vmbox_hvc_handler_t)(struct vm *vm,
		struct vmbox_device *vmbox, unsigned long arg);

static int vmbox_index = 0;
static struct vmbox *vmboxs[VMBOX_MAX_COUNT];

static inline struct vmbox_hook *vmbox_find_hook(char *name)
{
	struct vmbox_hook *hook;

	list_for_each_entry(hook, &vmbox_hook_list, list) {
		if (strcmp(hook->name, name) == 0)
			return hook;
	}

	return NULL;
}

int register_vmbox_hook(char *name, struct vmbox_hook_ops *ops)
{
	int len;
	struct vmbox_hook *hook;

	if (!ops)
		return -EINVAL;

	hook = vmbox_find_hook(name);
	if (hook) {
		pr_warn("vmbox hook [%s] areadly register\n", name);
		return -EEXIST;
	}

	hook = zalloc(sizeof(*hook));
	if (!hook)
		return -ENOMEM;

	len = strlen(name);
	len = MIN(len, 31);
	strncpy(hook->name, name, len);
	hook->ops = ops;
	list_add_tail(&vmbox_hook_list, &hook->list);

	return 0;
}

static void do_vmbox_hook(struct vmbox *vmbox)
{
	struct vmbox_hook *hook;

	hook = vmbox_find_hook(vmbox->name);
	if (!hook)
		return;

	if (hook->ops->vmbox_init)
		hook->ops->vmbox_init(vmbox);
}

static inline unsigned int
vmbox_virtq_vring_desc_size(unsigned int qsz, unsigned long align)
{
	int desc_size;

	desc_size = sizeof(struct vring_desc) * qsz + (align - 1);
	desc_size &= ~(align - 1);

	return desc_size;
}

static inline unsigned int
vmbox_virtq_vring_avail_size(unsigned int qsz, unsigned long align)
{
	int  avail_size;

	avail_size = sizeof(uint16_t) * (3 + qsz) + (align - 1);
	avail_size &= ~(align - 1);

	return avail_size;

}

static inline unsigned int
vmbox_virtq_vring_used_size(unsigned int qsz, unsigned long align)
{
	int used_size;

	used_size = sizeof(uint16_t) * 2 + sizeof(struct vring_used_elem) *
		(qsz + 1) + (align - 1);
	used_size &= ~(align - 1);

	return used_size;

}

static inline unsigned int
vmbox_virtq_vring_size(unsigned int qsz, unsigned long align)
{
	return vmbox_virtq_vring_desc_size(qsz, align) +
		vmbox_virtq_vring_avail_size(qsz, align) +
		vmbox_virtq_vring_used_size(qsz, align);
}

static inline size_t get_vmbox_iomem_header_size(struct vmbox_info *vinfo)
{
	size_t size = VMBOX_DEV_VIRTQ_HEADER_SIZE;

	/*
	 * calculate the vring desc size first, each vmbox will
	 * have 0x100 IPC region
	 */
	size += vmbox_virtq_vring_size(vinfo->vring_num,
			VMBOX_VRING_ALGIN_SIZE) * vinfo->vqs;

	return size;
}

static inline size_t get_vmbox_iomem_buf_size(struct vmbox_info *vinfo)
{
	return (vinfo->vring_num * vinfo->vring_size * vinfo->vqs);
}

static inline size_t get_vmbox_iomem_size(struct vmbox_info *vinfo)
{
	return get_vmbox_iomem_header_size(vinfo) +
				get_vmbox_iomem_buf_size(vinfo);
}

static struct vmbox_device *create_vmbox_device(struct vmbox *vmbox, int idx)
{
	struct vmbox_device *vdev;

	vdev = zalloc(sizeof(*vdev));
	if (!vdev)
		return NULL;

	vdev->vmbox_id = vmbox->id;
	vdev->is_backend = (idx == BE_IDX);
	vdev->vm = get_vm_by_id(vmbox->owner[idx]);

	return vdev;
}

static int create_vmbox_devices(struct vmbox *vmbox)
{
	struct vmbox_device *vdev_be;
	struct vmbox_device *vdev_fe;

	vdev_be = create_vmbox_device(vmbox, BE_IDX);
	vdev_fe = create_vmbox_device(vmbox, FE_IDX);

	if (!vdev_be || !vdev_fe)
		goto release_vdev;

	/* connect to each other */
	vdev_be->bro = vdev_fe;
	vdev_fe->bro = vdev_be;
	vmbox->devices[BE_IDX] = vdev_be;
	vmbox->devices[FE_IDX] = vdev_fe;

	return 0;

release_vdev:
	if (vdev_be)
		free(vdev_be);
	if (vdev_fe)
		free(vdev_fe);
	return -ENOMEM;
}

static int create_vmbox(struct vmbox_info *vinfo)
{
	struct vm *vm1, *vm2;
	struct vmbox *vmbox;
	size_t iomem_size = 0;
	int o1 = vinfo->owner[BE_IDX];
	int o2 = vinfo->owner[FE_IDX];

	vm1 = get_vm_by_id(o1);
	vm2 = get_vm_by_id(o2);
	if (!vm1 || !vm2) {
		pr_warn("no such VM %d %d\n", o1, o2);
		return -ENOENT;
	}

	vmbox = zalloc(sizeof(*vmbox));
	if (!vmbox)
		return -ENOMEM;

	vmbox->owner[BE_IDX] = o1;
	vmbox->owner[FE_IDX] = o2;
	memcpy(vmbox->devid, vinfo->id, sizeof(uint32_t) * 2);
	strcpy(vmbox->name, vinfo->type);
	vmbox->vqs = vinfo->vqs;
	vmbox->vring_num = vinfo->vring_num;
	vmbox->vring_size = vinfo->vring_size;
	vmbox->id = vmbox_index;
	vmbox->flags = vinfo->flags;
	vmboxs[vmbox_index++] = vmbox;

	/*
	 * the current memory allocation system has a limitation
	 * that get_io_pages can not get memory which bigger than
	 * 2M. if need to get memory bigger than 2M can use
	 * alloc_mem_block and map these memory to IO memory ?
	 */
	if (!vinfo->shmem_size) {
		iomem_size = get_vmbox_iomem_size(vinfo);
		iomem_size = PAGE_BALIGN(iomem_size);
	} else {
		iomem_size = PAGE_BALIGN(vinfo->shmem_size);
	}

	vmbox->shmem_size = iomem_size;
	vmbox->shmem = get_io_pages(PAGE_NR(iomem_size));
	if (!vmbox->shmem)
		panic("no more memory for %s\n", vinfo->type);

	/* init all the header memory to zero */
	if (!vinfo->shmem_size)
		memset(vmbox->shmem, 0, get_vmbox_iomem_header_size(vinfo));

	if (create_vmbox_devices(vmbox))
		pr_err("create vmbox device for %s failed\n", vmbox->name);

	do_vmbox_hook(vmbox);

	return 0;
}

int of_create_vmbox(struct device_node *node)
{
	int ret;
	struct vmbox_info vinfo;

	if (vmbox_index >= VMBOX_MAX_COUNT) {
		pr_err("vmbox count beyond the max size\n");
		return -ENOSPC;
	}

	memset(&vinfo, 0, sizeof(vinfo));

	/*
	 * vmbox-id	     - id of this vmbox dev_id and vendor_id
	 * vmbox-type	     - the type of this vmbox
	 * vmbox-owner	     - the owner of this vmbox, two owmer
	 * vmbox-vqs         - how many virtqueue for this vmbox
	 * vmbox-vrings      - how many vrings for each virtqueue
	 * vmbox-vring-size  - buffer size of each vring
	 * vmbox-shmem-size  - do not using virtq to transfer data between VM
	 */
	if (of_get_u32_array(node, "vmbox-owner", (uint32_t *)vinfo.owner, 2) < 2)
		return -EINVAL;

	of_get_string(node, "vmbox-type", vinfo.type, sizeof(vinfo.type) - 1);
	if (of_get_u32_array(node, "vmbox-id", vinfo.id, 2) <= 0)
		pr_warn("unknown vmbox id for %s\n", vinfo.type);

	if (of_get_bool(node, "platform-device"))
		vinfo.flags |= VMBOX_F_PLATFORM_DEV;

	ret = of_get_u32_array(node, "vmbox-shmem-size", &vinfo.shmem_size, 1);
	if (ret && vinfo.shmem_size > 0)
		goto out;

	if (of_get_u32_array(node, "vmbox-vqs", &vinfo.vqs, 1) <= 0)
		return -EINVAL;
	of_get_u32_array(node, "vmbox-vrings", &vinfo.vring_num, 1);
	of_get_u32_array(node, "vmbox-vring-size", &vinfo.vring_size, 1);

out:
	return create_vmbox(&vinfo);
}

struct vmbox_controller *vmbox_get_controller(struct vm *vm)
{
	struct vmbox_controller *vc;

	list_for_each_entry(vc, &vmbox_con_list, list) {
		if (vc->pdata == (void *)vm)
			return vc;
	}

	return NULL;
}

static void inline
__vmbox_device_online(struct vmbox_controller *vc, int id)
{
	uint32_t value;
	void *iomem;

	/* set the device online for the VM */
	iomem = vc->pa;
	value = ioread32(iomem + VMBOX_CON_DEV_STAT);
	value |= (1 << id);
	iowrite32(value, iomem + VMBOX_CON_DEV_STAT);

	/*
	 * if the controller of this vmbox_device is areadly
	 * online then send a virq to the VM
	 */
	if (vc->status) {
		iowrite32(VMBOX_CON_INT_TYPE_DEV_ONLINE,
			iomem + VMBOX_CON_INT_STATUS);
		send_virq_to_vm((struct vm *)vc->pdata, vc->virq);
	}
}

static int vmbox_device_attach(struct vmbox *vmbox, struct vmbox_device *vdev)
{
	void *iomem;
	struct vmm_area *va;
	struct vm *vm = vdev->vm;
	struct vmbox_controller *_vc;

	/*
	 * find the real vmbox which this vmbox device
	 * should connected to
	 */
	_vc = vmbox_get_controller(vm);
	if (!_vc) {
		pr_err("can not find vmbox_controller for vmbox dev\n");
		return -ENOENT;
	}

	vdev->vc = _vc;
	iomem = _vc->pa + VMBOX_CON_DEV_BASE + _vc->dev_cnt *
		VMBOX_CON_DEV_SIZE;

	vdev->dev_reg = iomem;
	vdev->vring_virq = alloc_vm_virq(vm);
	vdev->ipc_virq = alloc_vm_virq(vm);
	if (!vdev->vring_virq || !vdev->ipc_virq)
		return -ENOSPC;

	/* platform device has already alloc virtual memory */
	if (!vdev->iomem) {
		va = alloc_free_vmm_area(&vm->mm, vmbox->shmem_size,
				PAGE_MASK, VM_MAP_PT | VM_IO);
		if (!va)
			return -ENOMEM;
		vdev->iomem = va->start;
		vdev->iomem_size = vmbox->shmem_size;
		map_vmm_area(&vm->mm, va, (unsigned long)vmbox->shmem);
	}

	vdev->devid = _vc->dev_cnt++;
	_vc->devices[vdev->devid] = vdev;

	/* write the device information to the controller */
	memset(vdev->dev_reg, 0, VMBOX_CON_DEV_SIZE);
	iowrite32(vdev->devid | VMBOX_DEVICE_MAGIC, iomem + VMBOX_DEV_ID);
	iowrite32(vmbox->vqs, iomem + VMBOX_DEV_VQS);
	iowrite32(vmbox->vring_num, iomem + VMBOX_DEV_VRING_NUM);
	iowrite32(vmbox->vring_size, iomem + VMBOX_DEV_VRING_SIZE);
	iowrite32((unsigned long)vdev->iomem >> 32,
			iomem + VMBOX_DEV_VRING_BASE_HI);
	iowrite32((unsigned long)vdev->iomem & 0xffffffff,
			iomem + VMBOX_DEV_VRING_BASE_LOW);
	iowrite32(vdev->iomem_size, iomem + VMBOX_DEV_MEM_SIZE);

	/*
	 * client device and host device's device id, which
	 * is paired
	 */
	if (vdev->is_backend)
		iowrite32(vmbox->devid[0], iomem + VMBOX_DEV_DEVICE_ID);
	else
		iowrite32(vmbox->devid[0] + 1, iomem + VMBOX_DEV_DEVICE_ID);

	iowrite32(vmbox->devid[1], iomem + VMBOX_DEV_VENDOR_ID);
	iowrite32(vdev->vring_virq, iomem + VMBOX_DEV_VRING_IRQ);
	iowrite32(vdev->ipc_virq, iomem + VMBOX_DEV_IPC_IRQ);
	vdev->state = VMBOX_DEV_STAT_ONLINE;

	wmb();

	__vmbox_device_online(_vc, vdev->devid);

	return 0;
}

static void vmbox_con_online(struct vmbox_controller *vc)
{
	int i;
	struct vmbox *vmbox;
	struct vm *vm = vc->pdata;

	for (i = 0; i < vmbox_index; i++) {
		vmbox = vmboxs[i];

		/*
		 * when a vmbox controller is online, we first check all
		 * the clinet device which is attached to this device and
		 * report the client device to the VM. VM then load the
		 * driver for this client device.
		 *
		 * once the client device finish to setup, it will write
		 * it's status to power on, at this time, get the host
		 * device which the client device connected to, and
		 * report the host device to the VM
		 */
		if ((vmbox->owner[BE_IDX] == vm->vmid)) {
			if (vmbox_device_attach(vmbox, vmbox->devices[BE_IDX]))
				pr_err("vmbox device attached failed\n");
		}
	}
}

static int vmbox_con_read(struct vdev *vdev, gp_regs *regs,
		unsigned long address, unsigned long *value)
{
	panic("Can not trap read IO operation for vmbox controller\n");
	return 0;
}

static int vmbox_handle_con_request(struct vmbox_controller *vc,
		unsigned long offset, unsigned long *value)
{
	uint32_t v;

	switch (offset) {
	case VMBOX_CON_ONLINE:
		vc->status = 1;
		vmbox_con_online(vc);
		break;
	case VMBOX_CON_INT_STATUS:
		/* write value to clear the cosponding irq */
		v = ioread32(vc->pa + VMBOX_CON_INT_STATUS);
		v &= ~(*value);
		iowrite32(v, vc->pa + VMBOX_CON_INT_STATUS);
		break;
	default:
		break;
	}

	return 0;
}

static int vmbox_handle_dev_request(struct vmbox_controller *vc,
		unsigned long offset, unsigned long *value)
{
	int devid;
	uint32_t reg;
	struct vmbox_device *vdev;

	offset -= VMBOX_CON_DEV_BASE;
	devid = offset / VMBOX_CON_DEV_SIZE;
	reg = offset % VMBOX_CON_DEV_SIZE;

	if (devid >= sizeof(vc->devices)) {
		pr_err("vmbox devid invaild %d\n");
		return -EINVAL;
	}

	vdev = vc->devices[devid];
	if (!vdev) {
		pr_err("no such device %d\n", devid);
		return -ENOENT;
	}

	switch (reg) {
	case VMBOX_DEV_VRING_EVENT:
		send_virq_to_vm(vdev->bro->vm, vdev->bro->vring_virq);
		break;
	case VMBOX_DEV_IPC_EVENT:
		/*
		 * write the event_reg and send a virq to the vm
		 * wait last event finised
		 */
		if (!vdev->is_backend && !vdev->state)
			return 0;
		do {
			/* here will cause deadlock need to be fixed later */
			reg = ioread32(vdev->bro->dev_reg + VMBOX_DEV_IPC_TYPE);
			if (reg == 0)
				break;
			if (reg == *value)
				return 0;
			sched();
		} while (1);

		iowrite32(*value, vdev->bro->dev_reg + VMBOX_DEV_IPC_TYPE);
		send_virq_to_vm(vdev->bro->vm, vdev->bro->ipc_virq);
		break;
	case VMBOX_DEV_IPC_ACK:
		/* clear otherside's event type register */
		iowrite32(0, vdev->dev_reg + VMBOX_DEV_IPC_TYPE);
		break;
	case VMBOX_DEV_BACKEND_ONLINE:
		/*
		 * when host write this reg, it will send a event irq to
		 * the clien device to notice the client that need to do
		 * some thing for me
		 *
		 * when client write this register, need to update the same
		 * reg in host to inform host that the device is ready
		 */
		if (!vdev->is_backend)
			return 0;

		/* make the host device online */
		vmbox_device_attach(vmboxs[vdev->vmbox_id], vdev->bro);
		break;
	default:
		pr_err("unsupport reg 0x%x\n", reg);
		break;
	}

	return 0;
}

static int vmbox_con_write(struct vdev *vdev, gp_regs *regs,
		unsigned long address, unsigned long *value)
{
	struct vmbox_controller *vc = vdev_to_vmbox_con(vdev);
	unsigned long offset = address - (unsigned long)vc->va;

	if (offset < VMBOX_CON_DEV_BASE)
		return vmbox_handle_con_request(vc, offset, value);
	else
		return vmbox_handle_dev_request(vc, offset, value);
}

static void vmbox_con_deinit(struct vdev *vdev)
{
	/* will never called */
}

static void vmbox_con_reset(struct vdev *vdev)
{
	/* will never called */
}

static int __of_setup_vmbox_iomem
(void *dtb, int node, unsigned long iomem, size_t iomem_size)
{
	uint32_t tmp[4];
	uint32_t *args = tmp;
	int size = 0, size_cells, addr_cells;

	size_cells = fdt_size_cells(dtb, 0);
	addr_cells = fdt_address_cells(dtb, 0);

	if (addr_cells == 1) {
		*args++ = cpu_to_fdt32(iomem);
		size++;
	} else {
		*args++ = cpu_to_fdt32(iomem >> 32);
		*args++ = cpu_to_fdt32(iomem & 0xffffffff);
		size += 2;
	}

	if (size_cells == 1) {
		*args++ = cpu_to_fdt32(iomem_size);
		size++;
	} else {
		*args++ = cpu_to_fdt32(iomem_size >> 32);
		*args++ = cpu_to_fdt32(iomem_size & 0xffffffff);
		size += 2;
	}

	fdt_setprop(dtb, node, "reg", (void *)tmp, size * 4);
	return 0;
}

static int __of_setup_vmbox_con_virqs(struct vmbox_controller *vcon,
		void *dtb, int node)
{
	int size = 0;
	uint32_t tmp[10];
	struct vm *vm = vcon->pdata;
	struct virq_chip *vc = vm->virq_chip;

	if (!vc->generate_virq) {
		pr_err("no generate_virq in virq_chip\n");
		return -ENOENT;
	}

	size += vc->generate_virq(tmp + size, vcon->virq);
	fdt_setprop(dtb, node, "interrupts", (void *)tmp, size * 4);

	return 0;
}

static void add_vmbox_con_to_vm(struct vm *vm, struct vmbox_controller *vc)
{
	int node;
	char node_name[128];
	void *dtb = vm->setup_data;

	memset(node_name, 0, 128);
	sprintf(node_name, "vmbox-controller@%x", vc->va);

	node = fdt_add_subnode(dtb, 0, node_name);
	if (node < 0) {
		pr_err("failed to add vmbox device %s for vm-%d\n",
				node_name, vm->vmid);
		return;
	}

	fdt_setprop(dtb, node, "compatible", "minos,vmbox", 12);

	__of_setup_vmbox_iomem(dtb, node, (unsigned long)vc->va, PAGE_SIZE);
	__of_setup_vmbox_con_virqs(vc, dtb, node);
}

static int vm_create_vmbox_controller(struct vm *vm)
{
	struct vmbox_controller *vc;
	struct vmm_area *va;

	vc = zalloc(sizeof(*vc));
	if (!vc)
		return -ENOMEM;

	va = alloc_free_vmm_area(&vm->mm, PAGE_SIZE, PAGE_MASK,
			VM_MAP_PT | VM_IO | VM_RO);
	if (!va) {
		free(vc);
		return -ENOMEM;
	}

	vc->virq = alloc_vm_virq(vm);
	if (!vc->virq) {
		free(vc);
		return -ENOENT;
	}

	vc->pa = get_io_page();
	if (!vc->pa) {
		free(vc);
		return -ENOMEM;
	}

	vc->va = (void *)va->start;
	map_vmm_area(&vm->mm, va, (unsigned long)vc->pa);
	vc->pdata = vm;
	memset(vc->pa, 0, VMBOX_CON_DEV_BASE);

	host_vdev_init(vm, &vc->vdev, (unsigned long)vc->va, PAGE_SIZE);
	vc->vdev.read = vmbox_con_read;
	vc->vdev.write = vmbox_con_write;
	vc->vdev.deinit = vmbox_con_deinit;
	vc->vdev.reset = vmbox_con_reset;

	list_add_tail(&vmbox_con_list, &vc->list);

	add_vmbox_con_to_vm(vm, vc);

	return 0;
}

int vmbox_register_platdev(struct vmbox_device *vdev, void *dtb, char *type)
{
	int node;
	char node_name[128];

	memset(node_name, 0, 128);
	sprintf(node_name, "vmbox-%s@%x", type, vdev->iomem);

	node = fdt_add_subnode(dtb, 0, node_name);
	if (node < 0) {
		pr_err("failed to add platform device %s\n", type);
		return -ENOENT;
	}

	memset(node_name, 0, 128);
	sprintf(node_name, "minos,%s", type);
	fdt_setprop(dtb, node, "compatible",
			node_name, strlen(node_name) + 1);

	__of_setup_vmbox_iomem(dtb, node, (unsigned long)vdev->iomem,
			vdev->iomem_size);
	return 0;
}

static int vmbox_device_do_hooks(struct vm *vm)
{
	int i;
	struct vmbox_hook *hook;
	struct vmbox *vmbox;

	for (i = 0; i < vmbox_index; i++) {
		vmbox = vmboxs[i];
		hook = vmbox_find_hook(vmbox->name);
		if (!hook)
			continue;

		if (hook->ops->vmbox_be_init)
			hook->ops->vmbox_be_init(vm, vmbox, vmbox->devices[0]);
		if (hook->ops->vmbox_fe_init)
			hook->ops->vmbox_fe_init(vm, vmbox, vmbox->devices[1]);
	}

	return 0;
}

int of_setup_vm_vmbox(struct vm *vm)
{
	int ret;

	ret = vm_create_vmbox_controller(vm);
	if (ret)
		return ret;

	return vmbox_device_do_hooks(vm);
}