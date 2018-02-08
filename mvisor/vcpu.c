#include <mvisor/mvisor.h>
#include <mvisor/vm.h>
#include <mvisor/vcpu.h>
#include <mvisor/pcpu.h>
#include <config/vm_config.h>
#include <config/config.h>
#include <mvisor/module.h>

extern unsigned char __vmm_vm_start;
extern unsigned char __vmm_vm_end;

static vm_t *vms[CONFIG_MAX_VM];
static uint32_t total_vms = 0;

struct list_head shared_mem_list;
struct list_head vm_list;

static int vmm_add_vm(vm_entry_t *vme)
{
	int i;
	vm_t *vm;

	if (!vme)
		return -EINVAL;

	vm = (vm_t *)vmm_malloc(sizeof(vm_t));
	if (!vm)
		return -ENOMEM;

	memset((char *)vm, 0, sizeof(vm_t));
	vm->vmid = vme->vmid;
	strncpy(vm->name, vme->name,
		MIN(strlen(vme->name), VMM_VM_NAME_SIZE - 1));
	vm->vcpu_nr = MIN(vme->nr_vcpu, CONFIG_VM_MAX_VCPU);
	init_list(&vm->mem_list);
	vm->boot_vm = (boot_vm_t)vme->boot_vm;
	vm->mmu_on = vme->mmu_on;
	vm->entry_point = vme->entry_point;
	memcpy(vm->vcpu_affinity, vme->vcpu_affinity,
			sizeof(uint32_t) * CONFIG_VM_MAX_VCPU);

	vm->index = total_vms;
	vms[total_vms] = vm;
	total_vms++;
	list_add_tail(&vm_list, &vm->vm_list);

	return 0;
}

static int parse_all_vms(void)
{
	int i;
	vm_entry_t *vme;
	size_t size = (&__vmm_vm_end) - (&__vmm_vm_start);
	phy_addr_t *start = (phy_addr_t *)(&__vmm_vm_start);

	if (size == 0)
		panic("No VM is found\n");

	size = size / sizeof(vm_entry_t *);
	pr_debug("Found %d VMs config\n", size);

	for (i = 0; i < size; i++) {
		vme = (vm_entry_t *)(*start);
		vmm_add_vm(vme);
		start++;
	}

	return 0;
}

vm_t *vmm_get_vm(uint32_t vmid)
{
	int i;
	vm_t *vm;

	for (i = 0; i < total_vms; i++) {
		vm = vms[i];
		if (vm->vmid == vmid)
			return vm;
	}

	return NULL;
}

vcpu_t *vmm_get_vcpu(uint32_t vmid, uint32_t vcpu_id)
{
	vm_t *vm;

	vm = vmm_get_vm(vmid);
	if (!vm)
		return NULL;

	if (vcpu_id >= vm->vcpu_nr)
		return NULL;

	return vm->vcpus[vcpu_id];
}

static int parse_vm_memory(void)
{
	uint32_t size;
	int i;
	struct vmm_memory_region *regions;
	struct vmm_memory_region *tmp;
	struct memory_region *m_reg;
	vm_t *vm;

	size = get_mem_config_size();
	regions = (struct vmm_memory_region *)get_mem_config_data();
	init_list(&shared_mem_list);

	if (size == 0)
		panic("Please get the memory config for system\n");

	for (i = 0; i < size; i++) {
		tmp = &regions[i];
		m_reg = (struct memory_region *)vmm_malloc(sizeof(struct memory_region));
		if (!m_reg)
			panic("No memory to parse the memory config");

		pr_debug("find memory region: 0x%x 0x%x %d %d %s\n",
				tmp->mem_base, tmp->mem_end, tmp->type,
				tmp->vmid, tmp->name);
		memset((char *)m_reg, 0, sizeof(struct memory_region));
		m_reg->mem_base = tmp->mem_base;
		m_reg->size = tmp->mem_end - tmp->mem_base;
		strncpy(m_reg->name, tmp->name,
			MIN(strlen(tmp->name), MEM_REGION_NAME_SIZE - 1));

		/*
		 * shared memory is for all vm to ipc
		 */
		if (tmp->type == 0x2) {
			m_reg->type = MEM_TYPE_NORMAL;
			list_add(&shared_mem_list, &m_reg->mem_region_list);
		} else {
			if (tmp->type == 0x0)
				m_reg->type = MEM_TYPE_NORMAL;
			else
				m_reg->type = MEM_TYPE_IO;

			vm = vmm_get_vm(tmp->vmid);
			if (!vm) {
				pr_error("Can not find the vm for the vmid:%d\n", tmp->vmid);
				continue;
			}

			list_add(&vm->mem_list, &m_reg->mem_region_list);
		}
	}

	return 0;
}

static int vm_create_vcpus(vm_t *vm)
{
	int i;
	vcpu_t *vcpu;

	if (!vm)
		return -EINVAL;

	for (i = 0; i < vm->vcpu_nr; i++) {
		vcpu = (vcpu_t *)vmm_malloc(sizeof(vcpu_t));
		if (vcpu == NULL)
			return -ENOMEM;

		memset((char *)vcpu, 0, sizeof(vcpu_t));

		vcpu->vcpu_id = i;
		vcpu->vm_belong_to = vm;
		vcpu->entry_point = vm->entry_point;
		vcpu->pcpu_affinity = pcpu_affinity(vcpu, vm->vcpu_affinity[i]);
		if (vcpu->pcpu_affinity == PCPU_AFFINITY_FAIL) {
			pr_fatal("%s Can not affinity for vcpu %d\n",
					vm->name, vcpu->vcpu_id);
			panic(NULL);
		} else {
			pr_info("Affinity the vcpu %d to pcpu %d for %s\n",
				vcpu->vcpu_id, vcpu->pcpu_affinity, vm->name);
			vm->vcpu_affinity[i] = vcpu->pcpu_affinity;
		}
		vm->vcpus[i] = vcpu;
	}

	return 0;
}

static int vm_map_memory(vm_t *vm)
{
	phy_addr_t ttb2_addr;
	uint64_t tcr_el2;

	if (!vm)
		return -EINVAL;

	ttb2_addr = mmu_map_vm_memory(&vm->mem_list);
	mmu_map_memory_region_list(ttb2_addr, &shared_mem_list);
	tcr_el2 = mmu_generate_vtcr_el2();

	//vm->vttbr_el2_addr = mmu_get_vttbr_el2_base(vm->vmid, ttb2_addr);
	//vm->vtcr_el2 = tcr_el2;

	return 0;
}

static void vm_config_hcrel2(vm_t *vm)
{
	uint64_t value = 0;

	/*
	 * AMO FMO IMO to control the irq fiq and serror trap
	 * to EL2
	 * VM to control to enable stage2 transtion for el1 and el0
	 */
	value = HCR_EL2_HVC | HCR_EL2_TWI | HCR_EL2_TWE | \
		HCR_EL2_TIDCP | HCR_EL2_IMO | HCR_EL2_FMO | \
		HCR_EL2_AMO | HCR_EL2_RW | HCR_EL2_VM;
	//vm->hcr_el2 = value;
}

static int vm_state_init(vm_t *vm)
{
	int i;
	vcpu_t *vcpu = NULL;

	if (!vm)
		return -EINVAL;

	/*
	 * find the boot cpu for each vm and
	 * mark its status, boot cpu will set
	 * to ready to run state then other vcpu
	 * is set to STOP state to wait for bootup
	 */
	for (i = 0; i < vm->vcpu_nr; i++) {
		vcpu = vm->vcpus[i];
		//set_up_vcpu_env(vm, vcpu);
		if (vmm_get_vcpu_id(vcpu) == 0)
			vmm_set_vcpu_state(vcpu, VCPU_STATE_READY);
		else
			vmm_set_vcpu_state(vcpu, VCPU_STATE_STOP);
	}

	return 0;
}

static int vm_arch_init(vm_t *vm)
{
	return arch_vm_init(vm);
}

static int vm_modules_init(vm_t *vm)
{
	int i;

	for (i = 0; i < vm->vcpu_nr; i++)
		vcpu_modules_init(vm->vcpus[i]);

	return 0;
}

static int vm_do_init_vms(void)
{
	int i;
	vm_t *vm;

	for (i = 0; i < total_vms; i++) {
		vm = vms[i];
		vm_create_vcpus(vm);
		//vm_map_memory(vm);
		//vm_config_hcrel2(vm);
		vm_state_init(vm);
		vm_arch_init(vm);
		vm_modules_init(vm);
	}

	return 0;
}

int init_vms(void)
{
	int ret = 0;

	init_list(&vm_list);
	ret = parse_all_vms();
	if (ret)
		panic("parsing the vm fail\n");

	parse_vm_memory();
	vm_do_init_vms();

	return 0;
}
