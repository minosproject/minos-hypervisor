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
#include <minos/sched.h>
#include <minos/irq.h>
#include <config/config.h>
#include <minos/mm.h>
#include <minos/bitmap.h>
#include <virt/os.h>
#include <virt/vm.h>
#include <virt/vmodule.h>
#include <virt/virq.h>
#include <virt/vmm.h>
#include <virt/vdev.h>
#include <virt/vmcs.h>
#include <minos/task.h>
#include <minos/pm.h>
#include <minos/of.h>
#include <virt/resource.h>
#include <common/gvm.h>
#include <virt/vmbox.h>
#include <minos/shell_command.h>
#include <virt/virt.h>

extern void virqs_init(void);
extern int vmodules_init(void);

struct vm *vms[CONFIG_MAX_VM];
static int total_vms = 0;
LIST_HEAD(vm_list);

DEFINE_SPIN_LOCK(vms_lock);
static DECLARE_BITMAP(vmid_bitmap, CONFIG_MAX_VM);

static int aff_current;
static int native_vcpus;
DECLARE_BITMAP(vcpu_aff_bitmap, NR_CPUS);
DEFINE_SPIN_LOCK(affinity_lock);

#define VM_NR_CPUS_CLUSTER	256

static inline void set_vcpu_ready(struct vcpu *vcpu)
{
	unsigned long flags;

	task_lock_irqsave(vcpu->task, flags);
	vcpu->task->stat = TASK_STAT_RDY;
	set_task_ready(vcpu->task, 0);
	task_unlock_irqrestore(vcpu->task, flags);
}

static inline void set_vcpu_stop(struct vcpu *vcpu)
{
	unsigned long flags;

	task_lock_irqsave(vcpu->task, flags);
	vcpu->task->stat = TASK_STAT_STOPPED;
	set_task_sleep(vcpu->task, 0);
	task_unlock_irqrestore(vcpu->task, flags);
}

static inline void set_vcpu_suspend(struct vcpu *vcpu)
{
	unsigned long flags;

	task_lock_irqsave(vcpu->task, flags);
	vcpu->task->stat = TASK_STAT_SUSPEND;
	set_task_sleep(vcpu->task, 0);
	task_unlock_irqrestore(vcpu->task, flags);
}

void vcpu_online(struct vcpu *vcpu)
{
	set_vcpu_ready(vcpu);
}

static int inline affinity_to_vcpuid(struct vm *vm, unsigned long affinity)
{
	int aff0, aff1;

	/*
	 * how to handle bit-little soc ? usually the hvm's
	 * cpu map is as same as the true hardware, so here
	 * if the VM is the VM0, the affinity is as same as
	 * the real hardware
	 */
	if (vm_is_hvm(vm))
		return affinity_to_cpuid(affinity);

	aff1 = (affinity >> 8) & 0xff;
	aff0 = affinity & 0xff;

	return (aff1 * VM_NR_CPUS_CLUSTER) + aff0;
}

int vcpu_power_on(struct vcpu *caller, unsigned long affinity,
		unsigned long entry, unsigned long unsed)
{
	int cpuid;
	struct vcpu *vcpu;

	cpuid = affinity_to_vcpuid(caller->vm, affinity);

	/*
	 * resched the pcpu since it may have in the
	 * wfi or wfe state, or need to sched the new
	 * vcpu as soon as possible
	 *
	 * vcpu belong the the same vm will not
	 * at the same pcpu
	 */
	vcpu = get_vcpu_in_vm(caller->vm, cpuid);
	if (!vcpu) {
		pr_err("no such:%d->0x%x vcpu for this VM %s\n",
				cpuid, affinity, caller->vm->name);
		return -ENOENT;
	}

	if (vcpu->task->stat == TASK_STAT_SUSPEND) {
		pr_notice("vcpu-%d of vm-%d power on from vm suspend 0x%p\n",
				vcpu->vcpu_id, vcpu->vm->vmid, entry);
		os_vcpu_power_on(vcpu, entry);
		vcpu_online(vcpu);
	} else {
		pr_err("vcpu_power_on : invalid vcpu state\n");
		return -EINVAL;
	}

	return 0;
}

void save_vcpu_context(struct task *task)
{
	save_vcpu_vmodule_state(task_to_vcpu(task));
}

void restore_vcpu_context(struct task *task)
{
	restore_vcpu_vmodule_state(task_to_vcpu(task));
}

int vcpu_can_idle(struct vcpu *vcpu)
{
	if (vcpu_has_irq(vcpu))
		return 0;

	if (vcpu->task->stat != TASK_STAT_RUNNING)
		return 0;

	return 1;
}

void vcpu_idle(struct vcpu *vcpu)
{
	unsigned long flags;

	if (vcpu_can_idle(vcpu)) {
		task_lock_irqsave(vcpu->task, flags);
		if (!vcpu_can_idle(vcpu)) {
			task_unlock_irqrestore(vcpu->task, flags);
			return;
		}

		vcpu->task->stat = TASK_STAT_SUSPEND;
		set_task_sleep(vcpu->task, 0);
		task_unlock_irqrestore(vcpu->task, flags);

		sched();
	}
}

int vcpu_suspend(struct vcpu *vcpu, gp_regs *c,
		uint32_t state, unsigned long entry)
{
	/*
	 * just call vcpu idle to put vcpu to suspend state
	 * and ignore the wake up entry, since the vcpu will
	 * not really powered off
	 */
	vcpu_idle(vcpu);

	return 0;
}

int vcpu_off(struct vcpu *vcpu)
{
	/*
	 * force set the vcpu to suspend state then sched
	 * out
	 */
	set_vcpu_stop(vcpu);
	sched();

	return 0;
}

static int vm_check_vcpu_affinity(int vmid, uint32_t *aff, int nr)
{
	int i;
	uint64_t mask = 0;

	for (i = 0; i < nr; i++) {
		if (aff[i] >= VM_MAX_VCPU)
			return -EINVAL;

		if (mask & (1 << aff[i]))
			return -EINVAL;
		else
			mask |= (1 << aff[i]);
	}

	return 0;
}

struct vcpu *get_vcpu_in_vm(struct vm *vm, uint32_t vcpu_id)
{
	if (vcpu_id >= vm->vcpu_nr)
		return NULL;

	return vm->vcpus[vcpu_id];
}

struct vcpu *get_vcpu_by_id(uint32_t vmid, uint32_t vcpu_id)
{
	struct vm *vm;

	vm = get_vm_by_id(vmid);
	if (!vm)
		return NULL;

	return get_vcpu_in_vm(vm, vcpu_id);
}

void kick_vcpu(struct vcpu *vcpu, int preempt)
{
	unsigned long flags;

	spin_lock_irqsave(&vcpu->task->lock, flags);

	/*
	 * if vcpu need preempt, ususally it will caused
	 * by a hardware irq arrive
	 */
	if (!task_is_ready(vcpu->task)) {
		vcpu->task->stat = TASK_STAT_RDY;
		set_task_ready(vcpu->task, preempt);
	} else if (preempt && (current->affinity != vcpu_affinity(vcpu)))
		pcpu_resched(vcpu_affinity(vcpu));

	spin_unlock_irqrestore(&vcpu->task->lock, flags);
}

static void inline release_vcpu(struct vcpu *vcpu)
{
	/*
	 * need to make sure that when free the memory resource
	 * can not be done in the interrupt context, so the
	 * destroy a task will done in the idle task, here
	 * just call vmodule_stop call back, then set the
	 * task to the stop list of the pcpu, when the idle
	 * task is run, the idle task will release this task
	 */
	if (vcpu->context)
		stop_vcpu_vmodule_state(vcpu);

	if (vcpu->task)
		release_task(vcpu->task);

	if (vcpu->vmcs_irq >= 0)
		release_hvm_virq(vcpu->vmcs_irq);

	free(vcpu->virq_struct);
	free(vcpu);
}

static struct vcpu *alloc_vcpu(void)
{
	struct vcpu *vcpu;

	vcpu = zalloc(sizeof(*vcpu));
	if (!vcpu)
		return NULL;

	vcpu->virq_struct = zalloc(sizeof(struct virq_struct));
	if (!vcpu->virq_struct)
		goto free_vcpu;

	vcpu->vmcs_irq = -1;
	return vcpu;

free_vcpu:
	free(vcpu);

	return NULL;
}

static struct vcpu *create_vcpu(struct vm *vm, uint32_t vcpu_id)
{
	char name[64];
	struct vcpu *vcpu;
	struct task *task;

	/* generate the name of the vcpu task */
	memset(name, 0, 64);
	sprintf(name, "%s-vcpu-%d", vm->name, vcpu_id);
	task = create_vcpu_task(name, vm->entry_point, NULL,
			vm->vcpu_affinity[vcpu_id], 0);
	if (task == NULL)
		return NULL;

	vcpu = alloc_vcpu();
	if (!vcpu) {
		release_task(task);
		return NULL;
	}

	task->pdata = vcpu;
	vcpu->task = task;
	vcpu->vcpu_id = vcpu_id;
	vcpu->vm = vm;

	if (!(vm->flags & VM_FLAGS_64BIT))
		task->flags |= TASK_FLAGS_32BIT;

	init_list(&vcpu->list);

	vcpu_virq_struct_init(vcpu);
	vm->vcpus[vcpu_id] = vcpu;
	spin_lock_init(&vcpu->idle_lock);

	vcpu->next = NULL;
	if (vcpu_id != 0)
		vm->vcpus[vcpu_id - 1]->next = vcpu;

	return vcpu;
}

int vcpu_reset(struct vcpu *vcpu)
{
	if (!vcpu)
		return -EINVAL;

	vcpu_vmodules_reset(vcpu);
	vcpu_virq_struct_reset(vcpu);

	return 0;
}

static void inline __vcpu_power_off_call(struct vcpu *vcpu, int stop)
{
	if (vcpu_affinity(vcpu) != smp_processor_id()) {
		pr_err("vcpu-%s do not belong to this pcpu\n",
				vcpu->task->name);
		return;
	}

	if (stop)
		set_vcpu_stop(vcpu);
	else
		set_vcpu_suspend(vcpu);

	pr_notice("%s vcpu-%d-%d done\n",
			stop ? "stop" : "suspend",
			get_vmid(vcpu), get_vcpu_id(vcpu));

	/*
	 * *********** Note ****************
	 * if the vcpu is the current running vcpu
	 * need to resched another vcpu, since the vcpu
	 * may in el2 and el2/el0, force to sched to the
	 * new vcpu
	 */
	if (vcpu == get_current_vcpu()) {
		if (!preempt_allowed())
			pr_err("%s preempt is not allowed\n", __func__);

		set_need_resched();
	}
}

static void vcpu_power_off_call(void *data)
{
	__vcpu_power_off_call(data, 1);
}

static void vcpu_suspend_call(void *data)
{
	__vcpu_power_off_call(data, 0);
}

int vcpu_enter_poweroff(struct vcpu *vcpu, int timeout)
{
	/*
	 * since the vcpu will not sched on other
	 * cpus which its affinity, so the this
	 * function can called directly
	 */
	int cpuid = smp_processor_id();

	if (vcpu_affinity(vcpu) != cpuid) {
		pr_debug("call vcpu_power_off_call for vcpu-%s\n",
				vcpu->task->name);
		return smp_function_call(vcpu->task->affinity,
				vcpu_power_off_call, (void *)vcpu, 1);
	} else {
		/* just set it stat then force sched to another task */
		set_vcpu_stop(vcpu);
		pr_notice("power off vcpu-%d-%d done\n", get_vmid(vcpu),
				get_vcpu_id(vcpu));
	}

	return 0;
}

int vcpu_enter_suspend(struct vcpu *vcpu, int timeout)
{
	int cpuid = smp_processor_id();

	if (vcpu_affinity(vcpu) != cpuid) {
		pr_debug("call vcpu_suspend_call for vcpu-%s\n",
				vcpu->task->name);
		return smp_function_call(vcpu->task->affinity,
				vcpu_suspend_call, (void *)vcpu, 1);
	} else {
		/* just set it stat then force sched to another task */
		set_vcpu_suspend(vcpu);
		pr_notice("suspend vcpu-%d-%d done\n", get_vmid(vcpu),
				get_vcpu_id(vcpu));
	}

	return 0;
}

static int alloc_new_vmid(void)
{
	int vmid, start = total_vms;

	spin_lock(&vms_lock);
	vmid = find_next_zero_bit_loop(vmid_bitmap, CONFIG_MAX_VM, start);
	if (vmid >= CONFIG_MAX_VM)
		goto out;

	set_bit(vmid, vmid_bitmap);
out:
	spin_unlock(&vms_lock);

	return vmid;
}

static int vcpu_affinity_init(void)
{
	int i;
	struct vm *vm;

	bitmap_clear(vcpu_aff_bitmap, 0, NR_CPUS);

	for_each_vm(vm) {
		for (i = 0; i < vm->vcpu_nr; i++)
			set_bit(vm->vcpu_affinity[i], vcpu_aff_bitmap);
	}

	aff_current = find_first_zero_bit(vcpu_aff_bitmap, NR_CPUS);
	if (aff_current >= NR_CPUS)
		aff_current = (NR_CPUS - 1);

	for (i = 0; i < NR_CPUS; i++) {
		if (test_bit(i, vcpu_aff_bitmap))
			native_vcpus++;
	}

	return 0;
}

void get_vcpu_affinity(uint32_t *aff, int nr)
{
	int i = 0;
	int vm0_vcpu0_ok = 0;
	int vm0_vcpus_ok = 0;
	struct vm *vm0 = get_vm_by_id(0);
	int vm0_vcpu0 = vm0->vcpu_affinity[0];

	if (nr == NR_CPUS)
		vm0_vcpu0_ok = 1;
	else if (nr > (NR_CPUS - native_vcpus))
		vm0_vcpus_ok = 1;

	spin_lock(&affinity_lock);

	do {
		if (!test_bit(aff_current, vcpu_aff_bitmap)) {
			aff[i] = aff_current;
			i++;
		} else {
			if ((aff_current == vm0_vcpu0) && vm0_vcpu0_ok) {
				aff[i] = aff_current;
				i++;
			} else if ((aff_current != vm0_vcpu0) && vm0_vcpus_ok) {
				aff[i] = aff_current;
				i++;
			}
		}

		if (++aff_current >= NR_CPUS)
			aff_current = 0;
	} while (i < nr);

	spin_unlock(&affinity_lock);
}

static int vmtag_check_and_config(struct vmtag *tag)
{
	size_t size;

	/*
	 * first check whether there are enough memory for
	 * this vm and the vm's memory base need to be start
	 * at 0x80000000 or higher, if the mem_base is 0,
	 * then set it to default 0x80000000
	 */
	size = tag->mem_size;

	if (tag->mem_base == 0)
		tag->mem_base = GVM_NORMAL_MEM_START;

	if (!has_enough_memory(size))
		return -EINVAL;

	if (tag->nr_vcpu > NR_CPUS)
		return -EINVAL;

	/* for the dynamic need to get the affinity dynamicly */
	if (tag->flags & VM_FLAGS_DYNAMIC_AFF) {
		memset(tag->vcpu_affinity, 0, sizeof(tag->vcpu_affinity));
		get_vcpu_affinity(tag->vcpu_affinity, tag->nr_vcpu);
	}

	return 0;
}

int request_vm_virqs(struct vm *vm, int base, int nr)
{
	if (!vm || (base < GVM_IRQ_BASE) || (nr <= 0) ||
			(base + nr >= GVM_IRQ_END))
		return -EINVAL;

	while (nr > 0) {
		request_virq(vm, base, 0);
		base++;
		nr--;
	}

	return 0;
}

int vm_power_up(int vmid)
{
	struct vm *vm = get_vm_by_id(vmid);

	if (vm == NULL)
		return -ENOENT;

	vm_vcpus_init(vm);
	vm->state = VM_STAT_ONLINE;

	/*
	 * start the vm now
	 */
	start_vm(vmid);

	return 0;
}

static void inline wait_other_vcpu_offline(struct vm *vm)
{
	while (atomic_read(&vm->vcpu_online_cnt) > 1) {
		cpu_relax();
		mb();
	}
}

static void inline wait_all_vcpu_offline(struct vm *vm)
{
	while (atomic_read(&vm->vcpu_online_cnt) != 0) {
		cpu_relax();
		mb();
	}
}

static int __vm_power_off(struct vm *vm, void *args, int byself)
{
	int ret = 0;
	struct vcpu *vcpu;

	if (vm_is_native(vm))
		panic("native can not call power_off_vm\n");

	/* set the vm to offline state */
	pr_notice("power off vm-%d by %s\n", vm->vmid,
			byself ? "itself" : "mvm");

	preempt_disable();
	vm->state = VM_STAT_OFFLINE;

	/*
	 * just set all the vcpu of this vm to idle
	 * state, then send a virq to host to notify
	 * host that this vm need to be reset
	 */
	vm_for_each_vcpu(vm, vcpu) {
		ret = vcpu_enter_poweroff(vcpu, 1000);
		if (ret)
			pr_warn("power off vcpu-%d failed\n", vcpu->vcpu_id);
	}

	if (byself)
		wait_other_vcpu_offline(vm);
	else
		wait_all_vcpu_offline(vm);

	if (byself) {
		trap_vcpu_nonblock(VMTRAP_TYPE_COMMON,
				VMTRAP_REASON_SHUTDOWN, 0, NULL);
		preempt_enable();

		/* called by itself need to sched out */
		sched();
	} else
		preempt_enable();

	return 0;
}

int vm_power_off(int vmid, void *arg, int byself)
{
	struct vm *vm = NULL;

	if (vmid == 0)
		system_shutdown();

	vm = get_vm_by_id(vmid);
	if (!vm)
		return -EINVAL;

	return __vm_power_off(vm, arg, byself);
}

static int guest_mm_init(struct vm *vm, uint64_t base, uint64_t size)
{
	if (split_vmm_area(&vm->mm, base, size, VM_NORMAL) == NULL) {
		pr_err("invalid memory config for guest VM\n");
		return -EINVAL;
	}

	if (alloc_vm_memory(vm)) {
		pr_err("allocate memory for vm-%d failed\n", vm->vmid);
		return -ENOMEM;
	}

	return 0;
}

int create_vm_mmap(int vmid,  unsigned long offset,
		unsigned long size, unsigned long *addr)
{
	struct vm *vm = get_vm_by_id(vmid);
	struct vmm_area *va;

	va = vm_mmap(vm, offset, size);
	if (!va)
		return -EINVAL;

	*addr = va->start;
	return 0;
}

int create_guest_vm(struct vmtag *tag)
{
	int ret = VMID_INVALID;
	struct vm *vm;
	struct vmtag *vmtag;

	vmtag = (struct vmtag *)map_vm_mem((unsigned long)tag,
			sizeof(struct vmtag));
	if (!vmtag)
		return VMID_INVALID;

	ret = vmtag_check_and_config(vmtag);
	if (ret)
		goto unmap_vmtag;

	vm = create_vm(vmtag);
	if (!vm)
		goto unmap_vmtag;

	ret = guest_mm_init(vm, vmtag->mem_base, vmtag->mem_size);
	if (ret)
		goto release_vm;

	ret = vm->vmid;
	goto unmap_vmtag;

release_vm:
	destroy_vm(vm);
unmap_vmtag:
	unmap_vm_mem((unsigned long)tag, sizeof(struct vmtag));
	return ret;
}

bool vm_get_reset(struct vm *vm)
{
	return vm->pending_reset;
}

void vm_set_reset(struct vm *vm, bool reset)
{
	unsigned long flags;
	spin_lock_irqsave(&vm->lock, flags);
	vm->pending_reset = reset;
	spin_unlock_irqrestore(&vm->lock, flags);
}

static int __vm_reset(struct vm *vm, void *args, int byself)
{
	int ret;
	struct vdev *vdev;
	struct vcpu *vcpu;

#if 0
	if (vm_is_native(vm))
		panic("native vm can not call reset vm\n");
#endif

	/* set the vm to offline state */
	pr_notice("reset vm-%d by %s\n",
			vm->vmid, byself ? "itself" : "mvm");

	preempt_disable();
	vm->state = VM_STAT_REBOOT;

	/*
	 * if the args is NULL, then this reset is requested by
	 * iteself, otherwise the reset is called by vm0
	 */
	vm_for_each_vcpu(vm, vcpu) {
		ret = vcpu_enter_suspend(vcpu, 1000);
		if (ret) {
			pr_err("vm-%d vcpu-%d power off failed\n",
					vm->vmid, vcpu->vcpu_id);
			goto out;
		}
	}

	/*
	 * call vcpu_enter_suspend can not ensure that all the
	 * vcpu is sched out or not, here need wait again, if
	 * the reset is triggeried by itself, then wait other
	 * vcpu sched out, otherwise wait for all vcpu
	 */
	if (byself)
		wait_other_vcpu_offline(vm);
	else
		wait_all_vcpu_offline(vm);

	vm_for_each_vcpu(vm, vcpu) {
		ret = vcpu_reset(vcpu);
		if (ret) {
			pr_err("vcpu reset failed\n");
			goto out;
		}
	}

	/* reset the vdev for this vm */
	list_for_each_entry(vdev, &vm->vdev_list, list) {
		if (vdev->reset)
			vdev->reset(vdev);
	}

	vm_virq_reset(vm);

	if (vm_is_native(vm)) {
		preempt_enable();
		vm_set_reset(vm, true);
	} else if (byself) {
		trap_vcpu_nonblock(VMTRAP_TYPE_COMMON,
				VMTRAP_REASON_REBOOT, 0, NULL);
		preempt_enable();
		sched();
	} else
		preempt_enable();

	return 0;

out:
	preempt_enable();
	return ret;
}

int vm_reset(int vmid, void *args, int byself)
{
	struct vm *vm;

	/*
	 * if the vmid is 0, means the host request a
	 * hardware reset
	 */
	if (vmid == 0)
		system_reboot();

	vm = get_vm_by_id(vmid);
	if (!vm)
		return -ENOENT;

	return __vm_reset(vm, args, byself);
}

static int vm_resume(struct vm *vm)
{
	struct vcpu *vcpu;

	pr_notice("vm-%d resumed\n", vm->vmid);

	vm_for_each_vcpu(vm, vcpu) {
		if (get_vcpu_id(vcpu) == 0)
			continue;

		resume_vcpu_vmodule_state(vcpu);
	}

	do_hooks((void *)vm, NULL, OS_HOOK_RESUME_VM);
	trap_vcpu_nonblock(VMTRAP_TYPE_COMMON,
			VMTRAP_REASON_VM_RESUMED, 0, NULL);

	return 0;
}

static int __vm_suspend(struct vm *vm)
{
	struct vcpu *vcpu = get_current_vcpu();

	pr_notice("suspend vm-%d\n", vm->vmid);
	if (get_vcpu_id(vcpu) != 0) {
		pr_err("vm suspend can only called by vcpu0\n");
		return -EPERM;
	}

	vm_for_each_vcpu(vm, vcpu) {
		if (get_vcpu_id(vcpu) == 0)
			continue;

		if (vcpu->task->stat != TASK_STAT_STOPPED) {
			pr_err("vcpu-%d is not suspend vm suspend fail\n",
					get_vcpu_id(vcpu));
			return -EINVAL;
		}

		suspend_vcpu_vmodule_state(vcpu);
	}

	vm->state = VM_STAT_SUSPEND;
	trap_vcpu_nonblock(VMTRAP_TYPE_COMMON,
			VMTRAP_REASON_VM_SUSPEND, 0, NULL);

	/* call the hooks for suspend */
	do_hooks((void *)vm, NULL, OS_HOOK_SUSPEND_VM);

	set_task_suspend(0);
	sched();

	/* vm is resumed */
	vm->state = VM_STAT_ONLINE;
	vm_resume(vm);

	return 0;
}

int vm_suspend(int vmid)
{
	struct vm *vm = get_vm_by_id(vmid);

	if (vm == NULL) {
		return -EINVAL;
	}

	if (vm_is_hvm(vm))
		return system_suspend();

	return __vm_suspend(vm);
}

static void vm_setup(struct vm *vm)
{
	/*
	 * here need first map the setup data into the
	 * hypervisor memory space, in case data abort, sine
	 * there may by many place use the setup memory
	 */
	if (vm->setup_data) {
		create_host_mapping((vir_addr_t)vm->setup_data,
				(phy_addr_t)vm->setup_data,
				MEM_BLOCK_SIZE, 0);
	}

	if (vm->image_copy) {
		int i;
		for (i = 0; i < ARRAY_SIZE(vm->image_copy) / 3 &&
			    vm->image_copy[i + 2] > 0;
		     i++) {
			create_host_mapping(vm->image_copy[i],
					    vm->image_copy[i],
					    vm->image_copy[i + 2], 0);
			create_host_mapping(vm->image_copy[i + 1],
					    vm->image_copy[i + 1],
					    vm->image_copy[i + 2], 0);
		}
	}

	/* 
	 * here need to create the resource based on the vm's
	 * os, when the os is a linux system, usually it will
	 * used device tree, if the os is rtos, need to write
	 * the iomem and virqs in the hypervisor's device tree
	 *
	 * here to check whether there are information in the
	 * hypervisor's dts, if not then try to parsing the dtb
	 * of the VM
	 *
	 * first map the dtb address to the hypervisor, here
	 * map these native VM's memory as read only
	 */
	os_create_native_vm_resource(vm);

	create_vmbox_controller(vm);

	os_setup_vm(vm);
	do_hooks(vm, NULL, OS_HOOK_SETUP_VM);

#if 0
	if (vm->setup_data) {
		destroy_host_mapping((vir_addr_t)vm->setup_data,
				MEM_BLOCK_SIZE);
	}
#endif
}

void destroy_vm(struct vm *vm)
{
	int i;
	unsigned long flags;
	struct vdev *vdev, *n;
	struct vcpu *vcpu;

	if (!vm)
		return;

	if (vm_is_native(vm))
		panic("can not destory native VM\n");

	/*
	 * 1 : release the vdev
	 * 2 : do hooks for each modules
	 * 3 : release the vcpu allocated to this vm
	 * 4 : free the memory for this VM
	 * 5 : update the vmid bitmap
	 * 6 : do vmodule deinit
	 */
	list_for_each_entry_safe(vdev, n, &vm->vdev_list, list) {
		list_del(&vdev->list);
		if (vdev->deinit)
			vdev->deinit(vdev);
	}

	do_hooks((void *)vm, NULL, OS_HOOK_DESTROY_VM);

	if (vm->vcpus) {
		for (i = 0; i < vm->vcpu_nr; i++) {
			vcpu = vm->vcpus[i];
			if (!vcpu)
				continue;
			release_vcpu(vcpu);
		}

		free(vm->vcpus);
	}

	release_vm_memory(vm);

	i = vm->vmid;
	spin_lock_irqsave(&vms_lock, flags);
	clear_bit(i, vmid_bitmap);
	list_del(&vm->vm_list);
	vms[i] = NULL;
	total_vms--;
	spin_unlock_irqrestore(&vms_lock, flags);

	free(vm);
}

int vm_vcpus_init(struct vm *vm)
{
	struct vcpu *vcpu;

	vm_for_each_vcpu(vm, vcpu) {
		pr_notice("vm-%d vcpu-%d affnity to pcpu-%d\n",
				vm->vmid, vcpu->vcpu_id, vcpu_affinity(vcpu));

		vcpu_vmodules_init(vcpu);

		if (!vm_is_native(vm)) {
			vcpu->vmcs->host_index = 0;
			vcpu->vmcs->guest_index = 0;
		}
	}

	/* some task will excuted after this function */
	vm_for_each_vcpu(vm, vcpu) {
		do_hooks(vcpu, NULL, OS_HOOK_VCPU_INIT);
		os_vcpu_init(vcpu);
	}

	return 0;
}

static int create_vcpus(struct vm *vm)
{
	int i, j;
	struct vcpu *vcpu;

	for (i = 0; i < vm->vcpu_nr; i++) {
		vcpu = create_vcpu(vm, i);
		if (!vcpu) {
			pr_err("create vcpu:%d for %s failed\n", i, vm->name);
			for (j = 0; j < vm->vcpu_nr; j++) {
				vcpu = vm->vcpus[j];
				if (!vcpu)
					continue;

				release_vcpu(vcpu);
			}

			return -ENOMEM;
		}
	}

	return 0;
}

static struct vm *__create_vm(struct vmtag *vme)
{
	struct vm *vm;

	if (vm_check_vcpu_affinity(vme->vmid, vme->vcpu_affinity,
				vme->nr_vcpu)) {
		pr_err("vcpu affinit for vm not correct\n");
		return NULL;
	}

	vm = malloc(sizeof(*vm));
	if (!vm)
		return NULL;

	vme->nr_vcpu = MIN(vme->nr_vcpu, VM_MAX_VCPU);

	memset(vm, 0, sizeof(struct vm));
	vm->vcpus = malloc(sizeof(struct vcpu *) * vme->nr_vcpu);
	if (!vm->vcpus) {
		free(vm);
		return NULL;
	}

	vm->vmid = vme->vmid;
	strncpy(vm->name, vme->name, sizeof(vm->name) - 1);
	vm->vcpu_nr = vme->nr_vcpu;
	vm->entry_point = (void *)vme->entry;
	vm->setup_data = (void *)vme->setup_data;
	vm->state = VM_STAT_OFFLINE;
	init_list(&vm->vdev_list);
	memcpy(vm->vcpu_affinity, vme->vcpu_affinity,
			sizeof(uint32_t) * VM_MAX_VCPU);
	memcpy(vm->image_copy, vme->image_copy, sizeof(vm->image_copy));
	vm->flags |= vme->flags;

	vms[vme->vmid] = vm;

	spin_lock(&vms_lock);
	list_add_tail(&vm_list, &vm->vm_list);
	total_vms++;
	spin_unlock(&vms_lock);

	vm->os = get_vm_os((char *)vme->os_type);

	return vm;
}

struct vm *create_vm(struct vmtag *vme)
{
	int ret = 0;
	struct vm *vm;
	int native = 0;

	if (!vme)
		return NULL;

	if ((vme->vmid < 0) || (vme->vmid >= CONFIG_MAX_VM)) {
		vme->vmid = alloc_new_vmid();
		if (vme->vmid == VMID_INVALID)
			return NULL;
	} else {
		if (test_and_set_bit(vme->vmid, vmid_bitmap))
			return NULL;

		native = 1;
	}

	vm = __create_vm(vme);
	if (!vm)
		return NULL;

	if (native)
		vm->flags |= VM_FLAGS_NATIVE;

	vm_mm_struct_init(vm);

	ret = create_vcpus(vm);
	if (ret) {
		pr_err("create vcpus for vm failded\n");
		ret = VMID_INVALID;
		goto release_vm;
	}

	if (do_hooks((void *)vm, NULL, OS_HOOK_CREATE_VM)) {
		pr_err("create vm failed in hook function\n");
		goto release_vm;
	}

	return vm;

release_vm:
	destroy_vm(vm);

	return NULL;
}

static void *create_native_vm_of(struct device_node *node, void *arg)
{
	int ret, i;
	struct vm *vm;
	struct vmtag vmtag;
	uint64_t meminfo[2 * VM_MAX_MEM_REGIONS];

	if (node->class != DT_CLASS_VM)
		return NULL;

	ret = parse_vm_info_of(node, &vmtag);
	if (ret)
		return NULL;

	pr_notice("**** create new vm ****\n");
	pr_notice("    vmid: %d\n", vmtag.vmid);
	pr_notice("    name: %s\n", vmtag.name);
	pr_notice("    os_type: %s\n", vmtag.os_type);
	pr_notice("    nr_vcpu: %d\n", vmtag.nr_vcpu);
	pr_notice("    entry: 0x%p\n", vmtag.entry);
	pr_notice("    setup_data: 0x%p\n", vmtag.setup_data);
	pr_notice("    %s-bit vm\n", vmtag.flags & VM_FLAGS_64BIT ? "64" : "32");
	pr_notice("    flags: 0x%x\n", vmtag.flags);
	pr_notice("    affinity: %d %d %d %d %d %d %d %d\n",
			vmtag.vcpu_affinity[0], vmtag.vcpu_affinity[1],
			vmtag.vcpu_affinity[2], vmtag.vcpu_affinity[3],
			vmtag.vcpu_affinity[4], vmtag.vcpu_affinity[5],
			vmtag.vcpu_affinity[6], vmtag.vcpu_affinity[7]);

	vm = (void *)create_vm(&vmtag);
	if (!vm) {
		pr_err("create vm-%d failed\n", vmtag.vmid);
		return NULL;
	}

	vm->dev_node = node;

	/* parse the memory information of the vm from dtb */
	ret = of_get_u64_array(node, "memory", meminfo, 2 * VM_MAX_MEM_REGIONS);
	if ((ret <= 0) || ((ret % 2) != 0)) {
		pr_err("get wrong memory information for vm-%d", vmtag.vmid);
		destroy_vm(vm);

		return NULL;
	}

	ret = ret / 2;

	for (i = 0; i < ret; i ++) {
		split_vmm_area(&vm->mm, meminfo[i * 2],
				meminfo[i * 2 + 1], VM_NORMAL);
	}

	return vm;
}

static void parse_and_create_vms(void)
{
#ifdef CONFIG_DEVICE_TREE
	struct device_node *node;

	node = of_find_node_by_name(hv_node, "vms");
	if (node)
		of_iterate_all_node_loop(node, create_native_vm_of, NULL);
#endif
}

static int of_create_vmboxs(void)
{
	struct device_node *mailboxes;
	struct device_node *child;

	mailboxes = of_find_node_by_name(hv_node, "vmboxs");
	if (!mailboxes)
		return -ENOENT;

	/* parse each mailbox entry and create it */
	of_node_for_each_child(mailboxes, child) {
		if (of_create_vmbox(child))
			pr_err("create vmbox [%s] fail\n", child->name);
		else
			pr_notice("create vmbox [%s] successful\n", child->name);
	}

	return 0;
}

int virt_init(void)
{
	struct vm *vm;

	vmodules_init();

	/* parse the vm information from dtb */
	parse_and_create_vms();

	/* check whether VM0 has been create correctly */
	vm = get_vm_by_id(0);
	if (!vm) {
		pr_err("vm0 has not been create correctly\n");
		return -ENOENT;
	}

	vcpu_affinity_init();

#ifdef CONFIG_DEVICE_TREE
	/* here create all the mailbox for all native vm */
	of_create_vmboxs();
#endif

	/*
	 * parsing all the memory/irq and resource
	 * from the setup data and create the resource
	 * for the vm
	 */
	for_each_vm(vm) {
		/*
		 * - map the vm's memory
		 * - create the vcpu for vm's each vcpu
		 * - init the vmodule state for each vcpu
		 * - prepare the vcpu for bootup
		 */
		os_vm_init(vm);
		vm_setup(vm);
		vm_mm_init(vm);
		vm->state = VM_STAT_ONLINE;

		/* need after all the task of the vm setup is finished */
		vm_vcpus_init(vm);
	}

	return 0;
}

void start_vm(int vmid)
{
	struct vcpu *vcpu = get_vcpu_by_id(vmid, 0);
	struct vm *vm = get_vm_by_id(vmid);

	if (vm->image_copy) {
		int i;
		for (i = 0; i < ARRAY_SIZE(vm->image_copy) / 3 &&
			    vm->image_copy[i + 2] > 0;
		     i++) {
			memcpy(vm->image_copy[i], vm->image_copy[i + 1],
			       vm->image_copy[i + 2]);
		}
	}

	if (vcpu)
		vcpu_online(vcpu);
	else
		pr_err("vm create with error, vm%d not exist\n", vmid);
}

void start_all_vm(void)
{
	struct vm *vm;

	list_for_each_entry(vm, &vm_list, vm_list)
		start_vm(vm->vmid);
}

/*
 * vm start 0 - start the vm which vmid is 0
 */
static int vm_command_hdl(int argc, char **argv)
{
	uint32_t vmid;

	if (argc > 2 && strcmp(argv[1], "start") == 0) {
		vmid = atoi(argv[2]);
		if (vmid == 0xff)
			start_all_vm();
		else
			start_vm(vmid);
	} else if (argc > 2 && strcmp(argv[1], "reset") == 0) {
		vmid = atoi(argv[2]);
		vm_reset(vmid, NULL, 0);
	}

	return 0;
}
DEFINE_SHELL_COMMAND(vm, "vm", "virtual machine cmd",
		vm_command_hdl, 2);
