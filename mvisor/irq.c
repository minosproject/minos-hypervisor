#include <mvisor/mvisor.h>
#include <mvisor/irq.h>
#include <config/vm_config.h>
#include <mvisor/vcpu.h>
#include <mvisor/mm.h>
#include <config/config.h>
#include <mvisor/device_id.h>
#include <mvisor/module.h>
#include <mvisor/sched.h>

DEFINE_PER_CPU(struct vmm_irq **, local_irqs);

static struct irq_chip *irq_chip;
static struct irq_domain *irq_domains[IRQ_DOMAIN_MAX];

static int init_irq_desc(struct vmm_irq *vmm_irq,
		struct irq_resource *config)
{
	struct vcpu *vcpu;

	vmm_irq->hno = config->hno;

	if (config->vmid == 0xffff) {
		pr_info("irq %d is for vmm\n", config->hno);
		vmm_irq->flags |= IRQ_FLAG_OWNER_VMM;
		strncpy(vmm_irq->name, config->name,
			MIN(strlen(config->name), MAX_IRQ_NAME_SIZE - 1));
		return 0;
	}

	vcpu = get_vcpu_by_id(config->vmid, config->affinity);
	if (!vcpu) {
		pr_error("Vcpu:%d is not exist for this vm\n", config->affinity);
		return -EINVAL;;
	}

	vmm_irq->vno = config->vno;
	vmm_irq->vmid = config->vmid;

	vmm_irq->affinity_vcpu = config->affinity;
	vmm_irq->affinity_pcpu = get_pcpu_id(vcpu);
	strncpy(vmm_irq->name, config->name,
		MIN(strlen(config->name), MAX_IRQ_NAME_SIZE - 1));
	vmm_irq->flags |= config->type;
	vmm_irq->flags |= IRQ_FLAG_AFFINITY_VCPU;

	return 0;
}

static struct vmm_irq *alloc_vmm_irq(void)
{
	struct vmm_irq *irq;

	irq = (struct vmm_irq *)vmm_zalloc(sizeof(struct vmm_irq));
	if (!irq)
		return NULL;

	spin_lock_init(&irq->lock);
	return irq;
}

void send_sgi(uint32_t sgi, int cpu)
{
	cpumask_t mask;

	if ((cpu < 0) || (cpu >= CONFIG_NR_CPUS))
		return;

	if (sgi >= 16)
		return;

	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);

	irq_chip->send_sgi(sgi, SGI_TO_LIST, &mask);
}

static int __send_virq(struct vcpu *vcpu, uint32_t vno, uint32_t hno, int hw)
{
	struct irq_struct *irq_struct;
	struct virq *virq;
	int index;

	irq_struct = &vcpu->irq_struct;

	spin_lock(&irq_struct->lock);

	/*
	 * The following cases are considered software programming
	 * errors and result in UNPREDICTABLE behavior:
	 *
	 * • Having a List register entry with ICH_LR<n>_EL2.HW= 1
	 *   which is associated with a physical interrupt, inactive
	 *   state or in pending state in the List registers if the
	 *   Distributor does not have the corresponding physical
	 *   interrupt in either the active state or the active and
	 *   pending state.
	 * • If ICC_CTLR_EL1.EOImode == 0 or ICC_CTLR_EL3.EOImode_EL3 == 0
	 *   then either:
	 *   — Having an active interrupt in the List registers with a priorit
	 *   that is not set in the corresponding Active Priorities Register.
	 *   — Having two interrupts in the List registers in the active stat
	 *   with the same preemption priority.>
	 * • Having two or more interrupts with the same pINTID in the Lis
	 *   registers for a single virtual CPU interface.
	 */
	if (hw) {
		for_each_set_bit(index, irq_struct->irq_bitmap,
				CONFIG_VCPU_MAX_ACTIVE_IRQS) {
			virq = &irq_struct->virqs[index];
			if (virq->h_intno == hno) {
				pr_error("vcpu has same pirq:%d in pending/actvie state", hno);
				spin_unlock(&irq_struct->lock);
				return -EAGAIN;
			}
		}
	}

	index = find_first_zero_bit(irq_struct->irq_bitmap,
			CONFIG_VCPU_MAX_ACTIVE_IRQS);
	if (index == CONFIG_VCPU_MAX_ACTIVE_IRQS) {
		/*
		 * no empty resource to handle this virtual irq
		 * need to drop it ? TBD
		 */
		pr_error("Can not send this virq now\n");
		spin_unlock(&irq_struct->lock);
		return -EAGAIN;
	}

	virq = &irq_struct->virqs[index];
	virq->h_intno = hno;
	virq->v_intno = vno;
	virq->hw = hw;
	virq->id = index;
	virq->state = VIRQ_STATE_OFFLINE;
	set_bit(index, irq_struct->irq_bitmap);
	list_add_tail(&irq_struct->pending_list, &virq->list);
	irq_struct->irq_pending++;

	spin_unlock(&irq_struct->lock);

	return 0;
}

int _send_virq(struct vcpu *vcpu, uint32_t virq, uint32_t hirq, int hw)
{
	int ret = 0;
	struct vcpu *vcpu_sender = current_vcpu();

	ret = __send_virq(vcpu, virq, hirq, hw);
	if (ret)
		goto out;

	if (vcpu_sender->pcpu_affinity != vcpu->pcpu_affinity) {
		/*
		 * if the sender and the target are not
		 * the same pcpu, then send a hw sgi to the
		 * pcpu to do the sched work
		 */
		send_sgi(CONFIG_VMM_RESCHED_IRQ, vcpu->pcpu_affinity);
	} else {
		/*
		 * if the sender and the target are the same
		 * pcpu, but the vcpu is not the same, just
		 * update the sched information to decide whether
		 * need to reshched
		 */
		if (vcpu != vcpu_sender)
			sched_vcpu(vcpu, /* SCHED_REASON_IRQ_PENDING*/ 0);
	}

out:
	return ret;
}

static int do_handle_guest_irq(struct vmm_irq *vmm_irq)
{
	struct vm *vm;
	struct vcpu *vcpu;

	vm = get_vm_by_id(vmm_irq->vmid);
	if (!vm) {
		pr_error("Invaild vm for this irq %d\n", vmm_irq->hno);
		return -EINVAL;
	}

	vcpu = get_vcpu_in_vm(vm, vmm_irq->affinity_vcpu);
	if (vcpu == NULL) {
		pr_error("Invaild vcpu for this irq %d\n", vmm_irq->hno);
		return -EINVAL;
	}

	return _send_virq(vcpu, vmm_irq->vno, vmm_irq->hno, 1);
}

static int do_handle_vmm_irq(struct vmm_irq *vmm_irq)
{
	uint32_t cpuid = get_cpu_id();
	int ret;

	if (cpuid != vmm_irq->affinity_pcpu) {
		pr_info("irq %d do not belong tho this cpu\n", vmm_irq->hno);
		ret =  -EINVAL;
		goto out;
	}

	if (!vmm_irq->handler) {
		pr_error("Irq is not register by VMM\n");
		ret = -EINVAL;
		goto out;
	}

	ret = vmm_irq->handler(vmm_irq->hno, vmm_irq->pdata);
	if (ret)
		pr_error("handle irq:%d fail in vmm\n", vmm_irq->hno);

out:
	irq_chip->irq_dir(vmm_irq->hno);

	return ret;
}


int register_irq_domain(int type, struct irq_domain_ops *ops)
{
	struct irq_domain *domain;

	if (type >= IRQ_DOMAIN_MAX)
		return -EINVAL;

	domain = (struct irq_domain *)vmm_zalloc(sizeof(struct irq_domain));
	if (!domain)
		return -ENOMEM;

	domain->ops = ops;
	irq_domains[type] = domain;
}

static struct vmm_irq **spi_alloc_irqs(uint32_t start, uint32_t count)
{
	struct vmm_irq **irqs;
	uint32_t size;
	uint32_t i;

	size = count * sizeof(struct vmm_irq *);
	irqs = (struct vmm_irq **)vmm_zalloc(size);
	if (!irqs)
		return NULL;

	return irqs;
}

static struct vmm_irq *spi_get_irq_desc(struct irq_domain *d, uint32_t irq)
{
	if ((irq < d->start) || (irq >= (d->start + d->count)))
		return NULL;

	return (d->irqs[irq - d->start]);
}

static uint32_t spi_virq_to_irq(struct irq_domain *d, uint32_t virq)
{
	int i;
	struct vmm_irq *vmm_irq;

	for (i = 0; i < d->count; i++) {
		vmm_irq = d->irqs[i];
		if (!vmm_irq)
			continue;

		if (vmm_irq->vno == virq)
			return vmm_irq->hno;
	}

	return BAD_IRQ;
}

static int spi_register_irq(struct irq_domain *d, struct irq_resource *res)
{
	struct vmm_irq *irq;

	irq = alloc_vmm_irq();
	init_irq_desc(irq, res);

	d->irqs[irq->hno - d->start] = irq;
	return 0;
}

static void spi_setup_irqs(struct irq_domain *d)
{
	int i;
	struct vmm_irq *vmm_irq;

	for (i = 0; i < d->count; i++) {
		vmm_irq = d->irqs[i];
		if (!vmm_irq)
			continue;

		if (vmm_irq->flags & IRQ_FLAG_AFFINITY_VCPU) {
			irq_chip->irq_set_type(vmm_irq->hno,
					vmm_irq->flags & IRQ_FLAG_TYPE_MASK);
			irq_chip->irq_set_affinity(vmm_irq->hno,
					vmm_irq->affinity_pcpu);
		}
	}
}

static int spi_int_handler(struct irq_domain *d, struct vmm_irq *vmm_irq)
{
	int ret;

	if (vmm_irq->flags & IRQ_FLAG_OWNER_VMM)
		ret = do_handle_vmm_irq(vmm_irq);
	else
		ret = do_handle_guest_irq(vmm_irq);

	return ret;
}

struct irq_domain_ops spi_domain_ops = {
	.alloc_irqs = spi_alloc_irqs,
	.get_irq_desc = spi_get_irq_desc,
	.virq_to_irq = spi_virq_to_irq,
	.register_irq = spi_register_irq,
	.setup_irqs = spi_setup_irqs,
	.irq_handler = spi_int_handler,
};

static struct vmm_irq **local_alloc_irqs(uint32_t start, uint32_t count)
{
	struct vmm_irq **irqs;
	uint32_t size;
	uint32_t i;
	unsigned long addr;

	/*
	 * each cpu will have its local irqs
	 */
	size = count * sizeof(struct vmm_irq *) * CONFIG_NR_CPUS;
	irqs = (struct vmm_irq **)vmm_zalloc(size);
	if (!irqs)
		return NULL;

	addr = (unsigned long)irqs;
	for (i = 0; i < CONFIG_NR_CPUS; i++) {
		get_per_cpu(local_irqs, i) = (struct vmm_irq **)addr;
		addr += count * sizeof(struct vmm_irq *);
	}

	return irqs;
}

static struct vmm_irq *local_get_irq_desc(struct irq_domain *d, uint32_t irq)
{
	struct vmm_irq **irqs;

	if ((irq < d->start) || (irq >= (d->start + d->count)))
		return NULL;

	irqs = get_cpu_var(local_irqs);

	return irqs[irq - d->start];
}

static uint32_t local_virq_to_irq(struct irq_domain *d, uint32_t virq)
{
	/*
	 * vsgi and vppi and will never attach to a physical
	 * irq TBD
	 */
	return BAD_IRQ;
}

static int local_register_irq(struct irq_domain *d, struct irq_resource *res)
{
	int i;
	struct vmm_irq *irq;
	struct vmm_irq **local;

	for (i = 0; i < CONFIG_NR_CPUS; i++) {
		local = get_per_cpu(local_irqs, i);
		irq = alloc_vmm_irq();
		init_irq_desc(irq, res);
		irq->affinity_pcpu = i;
		local[irq->hno - d->start] = irq;
	}

	return 0;
}

static void local_setup_irqs(struct irq_domain *d)
{
	/*
	 * nothing to do now the trigger will
	 * be set when gicv3 chip init
	 */
}

static int local_int_handler(struct irq_domain *d, struct vmm_irq *vmm_irq)
{
	return do_handle_vmm_irq(vmm_irq);
}

struct irq_domain_ops local_domain_ops = {
	.alloc_irqs = local_alloc_irqs,
	.get_irq_desc = local_get_irq_desc,
	.virq_to_irq = local_virq_to_irq,
	.setup_irqs = local_setup_irqs,
	.irq_handler = local_int_handler,
	.register_irq = local_register_irq,
};

static int irq_domain_create_irqs(struct irq_domain *d,
		uint32_t start, uint32_t cnt)
{
	struct vmm_irq **irqs;

	if ((cnt == 0) || (cnt >= 1024)) {
		pr_error("%s: invaild irq cnt %d\n", __func__, cnt);
		return -EINVAL;
	}

	irqs = d->ops->alloc_irqs(start, cnt);
	if (!irqs)
		return -ENOMEM;

	d->start = start;
	d->count = cnt;
	d->irqs = irqs;

	return 0;
}

int irq_add_spi(uint32_t start, uint32_t cnt)
{
	struct irq_domain *domain = irq_domains[IRQ_DOMAIN_SPI];

	if (!domain)
		return -ENOENT;

	return irq_domain_create_irqs(domain, start, cnt);
}

int irq_add_local(uint32_t start, uint32_t cnt)
{
	struct irq_domain *domain = irq_domains[IRQ_DOMAIN_LOCAL];

	if (!domain)
		return -ENOENT;

	return irq_domain_create_irqs(domain, start, cnt);
}

static uint32_t virq_to_irq(uint32_t virq)
{
	int i;
	struct irq_domain *domain;
	uint32_t irq;

	for (i = 0; i < IRQ_DOMAIN_MAX; i++) {
		domain = irq_domains[i];
		irq = domain->ops->virq_to_irq(domain, virq);
		if (irq != BAD_IRQ)
			return irq;
	}

	return BAD_IRQ;
}

static struct irq_domain *get_irq_domain(uint32_t irq)
{
	int i;
	struct irq_domain *domain;

	for (i = 0; i < IRQ_DOMAIN_MAX; i++) {
		domain = irq_domains[i];
		if ((irq >= domain->start) &&
			(irq < domain->start + domain->count))
			return domain;
	}

	return NULL;
}

static struct vmm_irq *get_irq_desc(uint32_t irq)
{
	struct irq_domain *domain;

	domain = get_irq_domain(irq);
	if (!domain)
		return NULL;

	return domain->ops->get_irq_desc(domain, irq);
}

int vmm_register_irq_entry(void *res)
{
	struct irq_resource *config;
	struct vmm_irq *vmm_irq;
	struct vcpu *vcpu;
	struct irq_domain *domain;

	if (res == NULL)
		return -EINVAL;

	config = (struct irq_resource *)res;
	domain = get_irq_domain(config->hno);
	if (!domain) {
		pr_error("irq is not supported %d\n", config->hno);
		return -EINVAL;
	}

	return domain->ops->register_irq(domain, config);
}

void __irq_enable(uint32_t irq, int enable)
{
	unsigned long flag;
	struct vmm_irq *vmm_irq;

	vmm_irq = get_irq_desc(irq);
	if (!vmm_irq)
		return;

	spin_lock_irqsave(&vmm_irq->lock, flag);

	if (enable) {
		if (vmm_irq->flags & IRQ_FLAG_STATUS_MASK ==
				IRQ_FLAG_STATUS_MASKED)
			goto out;

		irq_chip->irq_unmask(irq);
		vmm_irq->flags &= ~IRQ_FLAG_STATUS_MASK;
		vmm_irq->flags |= IRQ_FLAG_STATUS_UNMASKED;
	} else {
		if (vmm_irq->flags & IRQ_FLAG_STATUS_MASK ==
				IRQ_FLAG_STATUS_UNMASKED)
			goto out;

		irq_chip->irq_mask(irq);
		vmm_irq->flags &= ~IRQ_FLAG_STATUS_MASK;
		vmm_irq->flags |= IRQ_FLAG_STATUS_MASKED;
	}

out:
	spin_unlock_irqrestore(&vmm_irq->lock, flag);
}

void __virq_enable(uint32_t virq, int enable)
{
	uint32_t irq;

	irq = virq_to_irq(virq);
	if (irq == BAD_IRQ)
		return;

	__irq_enable(irq, enable);
}

void vmm_setup_irqs(void)
{
	int i;
	struct irq_domain *d;

	for (i = 0; i < IRQ_DOMAIN_MAX; i++) {
		d = irq_domains[i];

		if (d->ops->setup_irqs)
			d->ops->setup_irqs(d);
	}
}

int send_virq_hw(uint32_t vmid, uint32_t virq, uint32_t hirq)
{
	struct vmm_irq *vmm_irq;
	struct vcpu *vcpu;

	vmm_irq = get_irq_desc(hirq);
	if (!vmm_irq)
		return -ENOENT;

	if (vmid != vmm_irq->vmid)
		return -EINVAL;

	vcpu = get_vcpu_by_id(vmid, vmm_irq->affinity_vcpu);
	if (!vcpu)
		return -ENOENT;

	return _send_virq(vcpu, virq, hirq, 1);
}

int send_virq(uint32_t vmid, uint32_t virq)
{
	/*
	 * default all the virq do not attached to
	 * the hardware irq will send to the vcpu0
	 * of a vm
	 */
	return _send_virq(get_vcpu_by_id(vmid, 0), virq, 0, 0);
}

void send_vsgi(struct vcpu *sender, uint32_t sgi, cpumask_t *cpumask)
{
	int cpu;
	struct vcpu *vcpu;
	struct vm *vm = sender->vm;

	for_each_set_bit(cpu, cpumask->bits, vm->vcpu_nr) {
		vcpu = vm->vcpus[cpu];
		_send_virq(vcpu, sgi, 0, 0);
	}
}


static int do_bad_int(uint32_t irq)
{
	pr_error("Handle bad irq do nothing %d\n", irq);
	irq_chip->irq_dir(irq);

	return 0;
}

int do_irq_handler(void)
{
	uint32_t irq;
	struct vmm_irq *vmm_irq;
	struct irq_domain *d;
	int ret = 0;

	if (!irq_chip)
		panic("irq_chip is Null when irq is triggered\n");

	irq = irq_chip->get_pending_irq();

	/*
	 * TBD - here we need deactive the irq
	 * for arm write the ICC_EOIR1_EL1 register
	 * to drop the priority
	 */
	irq_chip->irq_eoi(irq);

	d = get_irq_domain(irq);
	if (!d) {
		ret = -ENOENT;
		goto error;
	}

	vmm_irq = d->ops->get_irq_desc(d, irq);
	if (!vmm_irq) {
		pr_error("irq is not actived %d\n", irq);
		ret = -EINVAL;
		goto error;
	}

	return d->ops->irq_handler(d, vmm_irq);

error:
	do_bad_int(irq);
	return ret;
}

int request_irq(uint32_t irq, irq_handle_t handler, void *data)
{
	struct vmm_irq *vmm_irq;
	unsigned long flag;

	if ((!handler))
		return -EINVAL;

	vmm_irq = get_irq_desc(irq);
	if (!vmm_irq)
		return -ENOENT;

	/*
	 * whether the irq is belong to vmm
	 */
	if (!(vmm_irq->flags & IRQ_FLAG_OWNER_MASK))
		return -ENOENT;

	spin_lock_irqsave(&vmm_irq->lock, flag);
	vmm_irq->handler = handler;
	vmm_irq->pdata = data;
	spin_unlock_irqrestore(&vmm_irq->lock, flag);

	irq_unmask(irq);

	return 0;
}

static void irq_enter_to_guest(struct vcpu *vcpu, void *data)
{
	/*
	 * here we send the real virq to the vcpu
	 * before it enter to guest
	 */
	struct virq *virq;
	struct irq_struct *irq_struct = &vcpu->irq_struct;

	spin_lock(&irq_struct->lock);

	list_for_each_entry(virq, &irq_struct->pending_list, list) {
		if (virq->state != VIRQ_STATE_OFFLINE)
			pr_debug("something was wrong with this irq %d\n", virq->id);

		virq->state = VIRQ_STATE_PENDING;
		irq_chip->send_virq(virq);
		list_del(&virq->list);
		irq_struct->irq_pending--;
	}

	spin_unlock(&irq_struct->lock);
}

static void irq_exit_from_guest(struct vcpu *vcpu, void *data)
{
	/*
	 * here we update the states of the irq state
	 * which the vcpu is handles, since this is running
	 * on percpu and hanlde per_vcpu's data so do not
	 * need spinlock
	 */
	struct virq *virq;
	uint32_t set_bit;
	int status;
	struct irq_struct *irq_struct = &vcpu->irq_struct;

	spin_lock(&irq_struct->lock);

	for_each_set_bit(set_bit, irq_struct->irq_bitmap,
			CONFIG_VCPU_MAX_ACTIVE_IRQS) {
		virq = (struct virq *)&irq_struct->virqs[set_bit];

		if (virq->state == VIRQ_STATE_OFFLINE)
			continue;

		status = irq_chip->get_virq_state(virq);

		/*
		 * the virq has been handled by the VCPU
		 */
		if (status == VIRQ_STATE_INACTIVE) {
			irq_struct->count--;
			if (irq_struct->count < 0) {
				pr_error("irq count is error\n");
				break;
			}

			virq->h_intno = 0;
			virq->v_intno = 0;
			virq->state = VIRQ_STATE_INACTIVE;
			clear_bit(set_bit, irq_struct->irq_bitmap);
		}
	}

	spin_unlock(&irq_struct->lock);
}

void vcpu_irq_struct_init(struct irq_struct *irq_struct)
{
	int i;
	struct virq *virq;

	if (!irq_struct)
		return;

	irq_struct->count = 0;
	spin_lock_init(&irq_struct->lock);
	init_list(&irq_struct->pending_list);
	bitmap_clear(irq_struct->irq_bitmap, 0, CONFIG_VCPU_MAX_ACTIVE_IRQS);
	irq_struct->irq_pending = 0;

	for (i = 0; i < CONFIG_VCPU_MAX_ACTIVE_IRQS; i++) {
		virq = &irq_struct->virqs[i];
		virq->h_intno = 0;
		virq->v_intno = 0;
		virq->state = VIRQ_STATE_INACTIVE;
		virq->id = i;
		virq->hw = 0;
		init_list(&virq->list);
	}
}

int vcpu_has_irq_pending(struct vcpu *vcpu)
{
	return (!!vcpu->irq_struct.irq_pending);
}

int vmm_irq_init(void)
{
	uint32_t size;
	char *chip_name = CONFIG_IRQ_CHIP_NAME;

	irq_chip = (struct irq_chip *)vmm_get_module_pdata(chip_name,
			VMM_MODULE_NAME_IRQCHIP);
	if (!irq_chip)
		panic("can not find the irqchip for system\n");

	register_irq_domain(IRQ_DOMAIN_SPI, &spi_domain_ops);
	register_irq_domain(IRQ_DOMAIN_LOCAL, &local_domain_ops);

	/*
	 * now init the irqchip, and in the irq chip
	 * the chip driver need to alloc the irq it
	 * need used in the ssystem
	 */
	if (irq_chip->init)
		irq_chip->init();

	if (!irq_chip->get_pending_irq)
		panic("No function to get irq nr\n");

	vmm_register_hook(irq_exit_from_guest,
			NULL, VMM_HOOK_TYPE_EXIT_FROM_GUEST);
	vmm_register_hook(irq_enter_to_guest,
			NULL, VMM_HOOK_TYPE_ENTER_TO_GUEST);

	return 0;
}

int vmm_irq_secondary_init(void)
{
	if (irq_chip)
		irq_chip->secondary_init();

	return 0;
}