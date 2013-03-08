#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <linux/uio.h>

#include "trinity.h"	// page_size
#include "files.h"
#include "arch.h"
#include "sanitise.h"
#include "syscall.h"
#include "net.h"
#include "log.h"
#include "maps.h"
#include "shm.h"

char * filebuffer = NULL;
unsigned long filebuffersize = 0;


static bool within_page(void *addr, void *check)
{
	if (addr == check)
		return TRUE;
	if ((addr > check) && (addr < (check + page_size)))
		return TRUE;
	return FALSE;
}

bool validate_address(void *addr)
{
	if (within_page(addr, shm) == TRUE)
		return FALSE;
	if (within_page(addr, page_rand) == TRUE)
		return FALSE;
	if (within_page(addr, page_zeros) == TRUE)
		return FALSE;
	if (within_page(addr, page_0xff) == TRUE)
		return FALSE;
	if (within_page(addr, page_allocs) == TRUE)
		return FALSE;

	return TRUE;
}

static void * _get_address(unsigned char null_allowed)
{
	int i;
	void *addr = NULL;

	if (null_allowed == TRUE)
		i = rand() % 9;
	else
		i = (rand() % 8) + 1;


	switch (i) {
	case 0: addr = NULL;
		break;
	case 1:	addr = (void *) KERNEL_ADDR;
		break;
	case 2:	addr = page_zeros;
		break;
	case 3:	addr = page_0xff;
		break;
	case 4:	addr = page_rand;
		break;
	case 5: addr = page_allocs;
		break;
	case 6:	addr = (void *) get_interesting_value();
		break;
	case 7: addr = get_map();
		break;
	case 8: addr = malloc(page_size * 2);
		break;
	default:
		BUG("unreachable!\n");
		break;
	}

	/*
	 * Most of the time, we just return the address we got above unmunged.
	 * But sometimes, we return an address just before the end of the page.
	 * The idea here is that we might see some bugs that are caused by page boundary failures.
	 */
	i = rand() % 100;
	switch (i) {
	case 0:	addr += (page_size - sizeof(char));
		break;
	case 1:	addr += (page_size - sizeof(int));
		break;
	case 2:	addr += (page_size - sizeof(long));
		break;
	case 3:	addr += (page_size / 2);
		break;
	case 4 ... 99:
	default: break;
	}

	return addr;
}

void * get_address(void)
{
	return _get_address(TRUE);
}

void * get_non_null_address(void)
{
	return _get_address(FALSE);
}

unsigned long get_reg(void)
{
	if ((rand() % 2) == 0)
		return random();

	return get_interesting_value();
}

static unsigned int get_cpu(void)
{
	int i;
	i = rand() % 3;

	switch (i) {
	case 0: return -1;
	case 1: return rand() & 4095;
	case 2: return rand() & 15;
	default:
		BUG("unreachable!\n");
		break;
	}
	return 0;
}

unsigned long get_len(void)
{
	int i = 0;

	i = get_interesting_value();

	if (i == 0)
		return 0;

	switch(rand() % 6) {

	case 0:	i &= 0xff;
		break;
	case 1: i &= page_size;
		break;
	case 2:	i &= 0xffff;
		break;
	case 3:	i &= 0xffffff;
		break;
	case 4:	i &= 0xffffffff;
		break;
	default:
		// Pass through
		break;
	}

	/* we might get lucky if something is counting ints/longs etc. */
	if (rand() % 100 < 25) {
		switch (rand() % 3) {
		case 0:	i /= sizeof(int);
			break;
		case 1:	i /= sizeof(long);
			break;
		case 2:	i /= sizeof(long long);
			break;
		default:
			break;
		}
	}

	return i;
}

static struct iovec * alloc_iovec(unsigned int num)
{
	struct iovec *iov;
	unsigned int i;

	iov = malloc(num * sizeof(struct iovec));
	if (iov != NULL) {
		for (i = 0; i < num; i++) {
			iov[i].iov_base = malloc(page_size);
			iov[i].iov_len = page_size;
		}
	}
	return iov;
}

static unsigned long find_previous_arg_address(unsigned int argnum, unsigned int call, int childno)
{
	unsigned long addr = 0;

	if (argnum > 1)
		if ((syscalls[call].entry->arg1type == ARG_ADDRESS) ||
		    (syscalls[call].entry->arg1type == ARG_NON_NULL_ADDRESS))
			addr = shm->a1[childno];

	if (argnum > 2)
		if ((syscalls[call].entry->arg2type == ARG_ADDRESS) ||
		    (syscalls[call].entry->arg2type == ARG_NON_NULL_ADDRESS))
			addr = shm->a2[childno];

	if (argnum > 3)
		if ((syscalls[call].entry->arg3type == ARG_ADDRESS) ||
		    (syscalls[call].entry->arg3type == ARG_NON_NULL_ADDRESS))
			addr = shm->a3[childno];

	if (argnum > 4)
		if ((syscalls[call].entry->arg4type == ARG_ADDRESS) ||
		    (syscalls[call].entry->arg4type == ARG_NON_NULL_ADDRESS))
			addr = shm->a4[childno];

	if (argnum > 5)
		if ((syscalls[call].entry->arg5type == ARG_ADDRESS) ||
		    (syscalls[call].entry->arg5type == ARG_NON_NULL_ADDRESS))
			addr = shm->a5[childno];

	return addr;
}


static unsigned long fill_arg(int childno, int call, int argnum)
{
	unsigned long i;
	unsigned long mask = 0;
	unsigned long low = 0, high = 0;
	unsigned long addr = 0;
	unsigned int bits;
	unsigned int num = 0;
	const unsigned int *values = NULL;
	enum argtype argtype = 0;
	unsigned long sockaddr = 0, sockaddrlen = 0;

	switch (argnum) {
	case 1:	argtype = syscalls[call].entry->arg1type;
		break;
	case 2:	argtype = syscalls[call].entry->arg2type;
		break;
	case 3:	argtype = syscalls[call].entry->arg3type;
		break;
	case 4:	argtype = syscalls[call].entry->arg4type;
		break;
	case 5:	argtype = syscalls[call].entry->arg5type;
		break;
	case 6:	argtype = syscalls[call].entry->arg6type;
		break;
	default:
		BUG("unreachable!\n");
		return 0;
	}

	switch (argtype) {
	case ARG_UNDEFINED:
	case ARG_RANDOM_INT:
		return (unsigned long)rand();

	case ARG_FD:
		return get_random_fd();
	case ARG_LEN:
		return (unsigned long)get_len();

	case ARG_ADDRESS:
		if ((rand() % 2) == 0)
			return (unsigned long)get_address();

		/* Half the time, we look to see if earlier args were also ARG_ADDRESS,
		 * and munge that instead of returning a new one from get_address() */

		addr = find_previous_arg_address(argnum, call, childno);

		switch (rand() % 4) {
		case 0:	break;	/* return unmodified */
		case 1:	addr++;
			break;
		case 2:	addr+= sizeof(int);
			break;
		case 3:	addr+= sizeof(long);
			break;
		default: BUG("unreachable!\n");
			break;
		}

		return addr;

	case ARG_NON_NULL_ADDRESS:
		return (unsigned long)get_non_null_address();
	case ARG_PID:
		return (unsigned long)get_pid();
	case ARG_RANGE:
		switch (argnum) {
		case 1:	low = syscalls[call].entry->low1range;
			high = syscalls[call].entry->hi1range;
			break;
		case 2:	low = syscalls[call].entry->low2range;
			high = syscalls[call].entry->hi2range;
			break;
		case 3:	low = syscalls[call].entry->low3range;
			high = syscalls[call].entry->hi3range;
			break;
		case 4:	low = syscalls[call].entry->low4range;
			high = syscalls[call].entry->hi4range;
			break;
		case 5:	low = syscalls[call].entry->low5range;
			high = syscalls[call].entry->hi5range;
			break;
		case 6:	low = syscalls[call].entry->low6range;
			high = syscalls[call].entry->hi6range;
			break;
		default:
			BUG("Should never happen.\n");
			break;
		}

		if (high == 0) {
			printf("%s forgets to set hirange!\n", syscalls[call].entry->name);
			BUG("Fix syscall definition!\n");
			return 0;
		}

		i = random() % high;
		if (i < low) {
			i += low;
			i &= high;
		}
		return i;

	case ARG_OP:	/* Like ARG_LIST, but just a single value. */
		switch (argnum) {
		case 1:	num = syscalls[call].entry->arg1list.num;
			values = syscalls[call].entry->arg1list.values;
			break;
		case 2:	num = syscalls[call].entry->arg2list.num;
			values = syscalls[call].entry->arg2list.values;
			break;
		case 3:	num = syscalls[call].entry->arg3list.num;
			values = syscalls[call].entry->arg3list.values;
			break;
		case 4:	num = syscalls[call].entry->arg4list.num;
			values = syscalls[call].entry->arg4list.values;
			break;
		case 5:	num = syscalls[call].entry->arg5list.num;
			values = syscalls[call].entry->arg5list.values;
			break;
		case 6:	num = syscalls[call].entry->arg6list.num;
			values = syscalls[call].entry->arg6list.values;
			break;
		default: break;
		}
		mask |= values[rand() % num];
		return mask;

	case ARG_LIST:
		switch (argnum) {
		case 1:	num = syscalls[call].entry->arg1list.num;
			values = syscalls[call].entry->arg1list.values;
			break;
		case 2:	num = syscalls[call].entry->arg2list.num;
			values = syscalls[call].entry->arg2list.values;
			break;
		case 3:	num = syscalls[call].entry->arg3list.num;
			values = syscalls[call].entry->arg3list.values;
			break;
		case 4:	num = syscalls[call].entry->arg4list.num;
			values = syscalls[call].entry->arg4list.values;
			break;
		case 5:	num = syscalls[call].entry->arg5list.num;
			values = syscalls[call].entry->arg5list.values;
			break;
		case 6:	num = syscalls[call].entry->arg6list.num;
			values = syscalls[call].entry->arg6list.values;
			break;
		default: break;
		}
		bits = rand() % num;	/* num of bits to OR */
		for (i=0; i<bits; i++)
			mask |= values[rand() % num];
		return mask;

	case ARG_RANDPAGE:
		if ((rand() % 2) == 0)
			return (unsigned long) page_allocs;
		else
			return (unsigned long) page_rand;

	case ARG_CPU:
		return (unsigned long) get_cpu();

	case ARG_PATHNAME:
		if ((rand() % 100) > 10) {
fallback:
			return (unsigned long) get_filename();
		} else {
			/* Create a bogus filename with junk at the end of an existing one. */
			char *pathname = get_filename();
			char *suffix;
			int len = strlen(pathname);

			suffix = malloc(page_size);
			if (suffix == NULL)
				goto fallback;

			generate_random_page(suffix);

			(void) strcat(pathname, suffix);
			if ((rand() % 2) == 0)
				pathname[len] = '/';
			return (unsigned long) pathname;
		}

	case ARG_IOVEC:
		i = (rand() % 4) + 1;

		switch (argnum) {
		case 1:	if (syscalls[call].entry->arg2type == ARG_IOVECLEN)
				shm->a2[childno] = i;
			break;
		case 2:	if (syscalls[call].entry->arg3type == ARG_IOVECLEN)
				shm->a3[childno] = i;
			break;
		case 3:	if (syscalls[call].entry->arg4type == ARG_IOVECLEN)
				shm->a4[childno] = i;
			break;
		case 4:	if (syscalls[call].entry->arg5type == ARG_IOVECLEN)
				shm->a5[childno] = i;
			break;
		case 5:	if (syscalls[call].entry->arg6type == ARG_IOVECLEN)
				shm->a6[childno] = i;
			break;
		case 6:
		default: BUG("impossible\n");
		}
		return (unsigned long) alloc_iovec(i);

	case ARG_IOVECLEN:
	case ARG_SOCKADDRLEN:
		switch (argnum) {
		case 1:	return(shm->a1[childno]);
		case 2:	return(shm->a2[childno]);
		case 3:	return(shm->a3[childno]);
		case 4:	return(shm->a4[childno]);
		case 5:	return(shm->a5[childno]);
		case 6:	return(shm->a6[childno]);
		default: break;
		}
		;; // fallthrough

	case ARG_SOCKADDR:
		generate_sockaddr(&sockaddr, &sockaddrlen, PF_NOHINT);

		switch (argnum) {
		case 1:	if (syscalls[call].entry->arg2type == ARG_SOCKADDRLEN)
				shm->a2[childno] = sockaddrlen;
			break;
		case 2:	if (syscalls[call].entry->arg3type == ARG_SOCKADDRLEN)
				shm->a3[childno] = sockaddrlen;
			break;
		case 3:	if (syscalls[call].entry->arg4type == ARG_SOCKADDRLEN)
				shm->a4[childno] = sockaddrlen;
			break;
		case 4:	if (syscalls[call].entry->arg5type == ARG_SOCKADDRLEN)
				shm->a5[childno] = sockaddrlen;
			break;
		case 5:	if (syscalls[call].entry->arg6type == ARG_SOCKADDRLEN)
				shm->a6[childno] = sockaddrlen;
			break;
		case 6:
		default: BUG("impossible\n");
		}
		return (unsigned long) sockaddr;


	default:
		BUG("unreachable!\n");
		return 0;
	}

	BUG("unreachable!\n");
	return 0x5a5a5a5a;	/* Should never happen */
}

void generic_sanitise(int childno)
{
	unsigned int call = shm->syscallno[childno];

	if (syscalls[call].entry->arg1type != 0)
		shm->a1[childno] = fill_arg(childno, call, 1);
	if (syscalls[call].entry->arg2type != 0)
		shm->a2[childno] = fill_arg(childno, call, 2);
	if (syscalls[call].entry->arg3type != 0)
		shm->a3[childno] = fill_arg(childno, call, 3);
	if (syscalls[call].entry->arg4type != 0)
		shm->a4[childno] = fill_arg(childno, call, 4);
	if (syscalls[call].entry->arg5type != 0)
		shm->a5[childno] = fill_arg(childno, call, 5);
	if (syscalls[call].entry->arg6type != 0)
		shm->a6[childno] = fill_arg(childno, call, 6);
}
