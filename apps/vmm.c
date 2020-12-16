#include <virt/vm.h>

int vmm_task(void *data)
{
	struct vm *vm;

	for (;;) {
		msleep(10);

		for_each_vm(vm)
		{
			if (!vm_get_reset(vm))
				continue;

			vm_set_reset(vm, false);
			vm_power_up(vm->vmid);
		}
	}

	return 0;
}
