// The ARM CPU option. MEASUREMENT BUILD -- see docs/arm-cpu-plan.md.
//
// tools/armcpu runs the 68000 family on the ARM (Musashi) and drives the
// Amiga's bus through the core's bus proxy; Main only decides when it runs.
// It is a separate process, not a thread of Main, for two reasons: it takes
// ARM core 0 for itself (-R moves interrupts and tasks to core 1, where Main
// lives anyway), and whatever way it ends it gives the bus back to the fx68k
// on its own (atexit and fatal-signal handlers in armcpu.c), so a Main that
// crashes or is replaced cannot leave the Amiga held.
//
// The core is told "68000" while the ARM CPU is selected (minimig_ConfigCPU):
// the proxy replaces the fx68k's pins, so it only works on that path.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "minimig_armcpu.h"
#include "minimig_config.h"
#include "../../file_io.h"
#include "../../user_io.h"

static const char *const ARMCPU_BIN = "/media/fat/armcpu";
static const char *const ARMCPU_PID = "/tmp/armcpu.pid";
static const char *const ARMCPU_LOG = "/tmp/armcpu.log";

bool armcpu_core_has_proxy()
{
	// The proxy core is a measurement build installed under _Test/lwbridge/,
	// and an access to the bridge with nothing behind it can hang the ARM, so
	// the name is the gate -- the same one armcpu applies to itself.
	const char *rbf = get_rbf_path();
	return is_minimig() && rbf && strstr(rbf, "lwbridge") && !access(ARMCPU_BIN, X_OK);
}

bool armcpu_wanted()
{
	return minimig_config.armcpu == 1 && armcpu_core_has_proxy();
}

static pid_t armcpu_pid()
{
	FILE *f = fopen(ARMCPU_PID, "r");
	if (!f) return 0;
	int pid = 0;
	if (fscanf(f, "%d", &pid) != 1) pid = 0;
	fclose(f);
	if (pid <= 0) return 0;

	// still that process, and still armcpu
	char path[64], cmd[256] = {};
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	FILE *c = fopen(path, "r");
	if (!c) return 0;
	size_t n = fread(cmd, 1, sizeof(cmd) - 1, c);
	fclose(c);
	cmd[n] = 0;
	return strstr(cmd, "armcpu") ? (pid_t)pid : 0;
}

void armcpu_stop()
{
	pid_t pid = armcpu_pid();
	if (pid)
	{
		// SIGTERM: armcpu finishes the slice, gives the bus back and undoes
		// -R. It is never killed outright, which would leave the Amiga held.
		kill(pid, SIGTERM);
		for (int i = 0; i < 300 && !kill(pid, 0); i++)
		{
			struct timespec ts = { 0, 10 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		if (!kill(pid, 0)) printf("armcpu: pid %d did not stop on SIGTERM\n", pid);
		else printf("armcpu: stopped\n");
	}
	unlink(ARMCPU_PID);
}

static void armcpu_start()
{
	char rom[1024];
	snprintf(rom, sizeof(rom), "%s", getFullPath(minimig_config.kickstart));

	// Two forks, so the process armcpu runs in is no child of Main's to reap.
	pid_t first = fork();
	if (first < 0) { printf("armcpu: fork: %s\n", strerror(errno)); return; }
	if (first == 0)
	{
		setsid();
		pid_t second = fork();
		if (second != 0) _exit(second < 0);

		FILE *p = fopen(ARMCPU_PID, "w");
		if (p) { fprintf(p, "%d\n", getpid()); fclose(p); }
		int fd = open(ARMCPU_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
		execl(ARMCPU_BIN, "armcpu", "-c", "0", "-R", "-P", "-m", "6", "-F", "8",
		      "-C", "68020", "-t", "0", "-q", "-r", rom, (char *)NULL);
		_exit(127);
	}
	waitpid(first, NULL, 0);
	printf("armcpu: started on %s\n", rom);
}

void armcpu_sync()
{
	bool running = armcpu_pid() != 0;
	if (armcpu_wanted() && !running) armcpu_start();
	else if (!armcpu_wanted() && running) armcpu_stop();
}

void armcpu_restart()
{
	armcpu_stop();
	if (armcpu_wanted()) armcpu_start();
}
