#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "server.h"

bool spawn_client(struct cg_server *server, char *const argv[], pid_t *pid_out,
		struct wl_event_source **sigchld_source); // cage-present.c

static size_t n_commands = 0;
static char **cmds = NULL;
static char default_shell[] = "/bin/sh";
static char *custom_shell = NULL;

int
configure_shell(char *path_str)
{
	custom_shell = path_str;
	struct stat sb;
	/* Check that file exists and is executable */
	if (stat(path_str, &sb) == 0 && sb.st_mode & S_IXUSR) {
		return 0;
	} else {
		return 1;
	}
}

void
present_read_cmds(FILE *cmds_file)
{
	wlr_log(WLR_DEBUG, "Reading commands from file\n");
	size_t len = 0;
	ssize_t nread;
	char *line = NULL;
	size_t cmds_len = 0;
	while ((nread = getline(&line, &len, cmds_file)) != -1) {
		fwrite(line, nread, 1, stdout);
		line[nread-1] = '\0';
		wlr_log(WLR_DEBUG, "%s\n", line);
		size_t cmds_write_index = n_commands;
		n_commands++;

		if (cmds_len < n_commands) {
			size_t cmds_len_new = n_commands ;
			cmds = realloc(cmds, cmds_len_new*sizeof(*cmds));
			cmds_len = cmds_len_new;
		}
		cmds[cmds_write_index] = malloc(nread);
		strncpy(cmds[cmds_write_index], line, nread);
	};

	free(line);
	return;
}

void
present_direction_keypress(struct cg_server *server, int n)
{
	static struct wl_event_source *sigchld_source = NULL;

	if (sigchld_source) {
		wlr_log(WLR_DEBUG, "Removing previous application event source\n");
		wl_event_source_remove(sigchld_source);
	}
	
	static int cmds_index = 0;
	cmds_index += n;
	cmds_index = (cmds_index < 0) ? 0 : cmds_index;
	if ((size_t)cmds_index >= n_commands) {
		kill(server->active_pid, SIGHUP);
		int status;
		waitpid(server->active_pid, &status, 0);
		wl_display_terminate(server->wl_display);
		return;
	}

	char *shell = custom_shell ? custom_shell : default_shell;
	char *const argv[] = {shell, "-c", cmds[cmds_index], NULL};

	if (!spawn_client(server, argv, &server->active_pid, &sigchld_source)) {
		fprintf(stderr, "Couldn't spawn client\n");
		abort();
	}

}
