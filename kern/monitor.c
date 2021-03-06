// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display information about the kernel", mon_backtrace },
	{ "showva2pa", "Display the physical pages information corresponding to the designated virtual addresses", mon_showva2pa },
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// Your code here.
	uint32_t *ebp = (uint32_t *)read_ebp();
	struct Eipdebuginfo eipdebuginfo;
	while (ebp != 0)
	{
		//打印ebp, eip, 最近的五个参数
		uint32_t eip = *(ebp + 1);
		cprintf("ebp %08x eip %08x args %08x %08x %08x %08x %08x\n", ebp, eip, *(ebp + 2), *(ebp + 3), *(ebp + 4), *(ebp + 5), *(ebp + 6));
		//打印文件名等信息
		debuginfo_eip((uintptr_t)eip, &eipdebuginfo);
		cprintf("%s:%d", eipdebuginfo.eip_file, eipdebuginfo.eip_line);
		cprintf(": %.*s+%d\n", eipdebuginfo.eip_fn_namelen, eipdebuginfo.eip_fn_name, eipdebuginfo.eip_fn_addr);
		//更新ebp
		ebp = (uint32_t *)(*ebp);
	}
	return 0;
}

int
mon_showva2pa(int argc, char **argv, struct Trapframe *tf)
{
	uintptr_t *va_low = NULL;
	uintptr_t *va_high = NULL;
	uintptr_t *va = NULL;
	struct PageInfo *va_page = NULL;
	pte_t *entry = NULL;
	char *ptr;
	unsigned int w, u;

	if (argc == 1){
		cprintf("At least one argument.\n");
		return 0;
	}
	else if(argc >=4){
		cprintf("Too many arguments (max %d)\n", 2);
		return 0;
	}
	else if(argc == 2){
		va_low = (uintptr_t *)strtol(argv[1], &ptr, 16);
		va_high = (uintptr_t *)strtol(argv[1], &ptr, 16);
	}
	else if(argc == 3){
		va_low = (uintptr_t *)strtol(argv[1], &ptr, 16);
		va_high = (uintptr_t *)strtol(argv[2], &ptr, 16);
		if (va_low > va_high){
			va_low = (uintptr_t *)strtol(argv[2], &ptr, 16);
			va_high = (uintptr_t *)strtol(argv[1], &ptr, 16);
		}
	}

	for(va = va_low; va<=va_high; va+=1024){
		entry = pgdir_walk(kern_pgdir, (void *)va, 0);
		if (entry == NULL){
			cprintf("VA: %x does not have a mapped physical page!\n", va);
			continue;
		}

		va_page = page_lookup(kern_pgdir, (void *)va, &entry);
		if (va_page == NULL){
			cprintf("VA: %x does not have a mapped physical page!\n", va);
			continue;
		}
		else{
			if (((*entry) & (PTE_W)) == PTE_W){
				w = 1;
			}
			else w = 0;

			if (((*entry) & (PTE_U)) == PTE_U){
				u = 1;
			}
			else u = 0;

			cprintf("VA: 0x%x, PA: 0x%x, pp_ref: %d, PTE_W: %d, PTE_U: %d\n", va, page2pa(va_page), va_page->pp_ref, w, u);
		}
	}
	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
