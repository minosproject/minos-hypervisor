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

#include <minos/app.h>
#include <minos/console.h>
#include <minos/shell_command.h>
#include <minos/bootarg.h>

#ifdef CONFIG_VIRT
#include <virt/virt.h>
#endif

int vmm_task(void *data);

static int init_task(void *main)
{
#ifdef CONFIG_SHELL
	int i;
	char *tty = NULL;
	int skip_vm_boot = 0;
	uint32_t wait = 0;
	unsigned long timeout;
#endif

	/*
	 * first check whether need to stop to start all
	 * VM automaticly if the shell is enabled, if the
	 * shell is enabled, provide a debug mode to do some
	 * debuging for VM
	 */
#ifdef CONFIG_SHELL
#ifdef CONFIG_VIRT
	bootarg_parse_uint("bootwait", &wait);
	if (wait > 0) {
		printf("\nPress any key to stop vm startup: %d ", wait);
		for (i = 0; i < wait; i++) {
			timeout = NOW() + SECONDS(1);

			printf("\b\b%d ", wait - i);

			while (NOW() < timeout) {
				if (console_getc() > 0) {
					skip_vm_boot = 1;
					break;
				}
			}

			if (skip_vm_boot)
				break;
		}
	}

	if (!skip_vm_boot) {
		printf("\b\b0 ");
		printf("\n");
		start_all_vm();
	}
#endif
	if (!skip_vm_boot)
		bootarg_parse_string("tty", &tty);

	create_realtime_task("shell_task", shell_task,
			tty, CONFIG_SHELL_TASK_PRIO, 4096, 0);
	create_migrating_task("vmm_task", vmm_task, NULL, OS_PRIO_DEFAULT, 4096,
			      0);
#else
#ifdef CONFIG_VIRT
	start_all_vm();
#endif
#endif

	return 0;
}
DEFINE_TASK("init", init_task, NULL, OS_PRIO_DEFAULT, 4096, 0);
