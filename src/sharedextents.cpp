#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/mount.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>

#include <unordered_set>
#include <string>

void statx_info(struct statx *info, int fd, const char *filename)
{
	unsigned int mask = STATX_TYPE | STATX_INO | STATX_SUBVOL;
	if (statx(fd, "", AT_EMPTY_PATH, mask, info)) {
		perror("statx");
		exit(1);
	}
	// TODO: Implement fallback to ioctl for subvolid
	if (info->stx_mask & mask != mask) {
		fprintf(stderr, "statx() didn't return all needed information\n");
		exit(1);
	}
	info->stx_mode &= S_IFMT;
	if (info->stx_mode != S_IFREG) {
		fprintf(stderr, "%s not a regular file\n", filename);
		exit(1);
	}
}

struct process {
	pid_t pid;
	int pipefd;
};

struct process run_program(const char *argv[])
{
	struct process ret;
	
	int pipefd[2];
	if (pipe(pipefd)) {
		perror("pipe");
		exit(1);
	}

	ret.pid = fork();
	if (ret.pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
			perror("dup2");
			exit(1);
		}
		close(pipefd[1]);
		execvp(argv[0], (char**)argv);
		exit(errno);
	}
	close(pipefd[1]);
	ret.pipefd = pipefd[0];
	return ret;
}

std::unordered_set<std::string> handle_tree_dump(int fd, char *pattern, char *dirpath)
{
	const int buf_size = 16*1024;
	char *buf = (char*) malloc(buf_size);
	int len = buf_size - 1;
	buf[len] = '\0';
	
	FILE *fp = fdopen(fd, "r");
	if (!fp) {
		perror("fdopen");
		exit(1);
	}
	
	#define PRESTART 0
	#define START 1
	#define REFLINE 2
	#define BACKREF 3
	int state = PRESTART;
	char logical_addr[128];	/* Captured as string to pass it to btrfs command */
	int refcount;
	std::unordered_set<std::string> result;
	while (fgets(buf, len, fp)) {
		if (strstr(buf, "item"))
			state = START;
		if (state == START) {
			sscanf(buf, "%*[ \t]item %*llu key (%s", logical_addr);
			state = REFLINE;
			continue;
		}
		if (state == REFLINE) {
			sscanf(buf, "%*[ \t]refs %d", &refcount);
			if (refcount > 1)
				state = BACKREF;
			else
				state = PRESTART;
			continue;
		}
		if (state == BACKREF) {
			/* Found our extent */
			if (strstr(buf, pattern)) {
				const char *argv[] = {"btrfs", "inspect-internal", "logical-resolve", logical_addr, dirpath, NULL};
				struct process btrfsproc = run_program(argv);
				FILE *filepath_stream = fdopen(btrfsproc.pipefd, "r");
				if (!filepath_stream) {
					perror("fdopen");
					exit(1);
				}

				while (fgets(buf, len, filepath_stream))
					result.insert(buf);

				siginfo_t cmd_state;
				if (waitid(P_PID, btrfsproc.pid, &cmd_state, WEXITED)) {
					perror("waitid");
					exit(1);
				}
				if (cmd_state.si_code == CLD_KILLED) {
					fprintf(stderr, "btrfs logical-resolve command was killed\n");
					exit(1);
				}
				if (cmd_state.si_status != 0) {
					fprintf(stderr, "btrfs logical-resolve command exited with status %i\n", cmd_state.si_status);
					exit(1);
				}
				fclose(filepath_stream);
				state = PRESTART;
			}
		}
	}
	fclose(fp);
	free(buf);
	free(pattern);
	
	return result;
}

void find_device(char *dev, char *dirpath)
{
	const char *argv[] = {"btrfs", "device", "stats", dirpath, NULL};
	struct process btrfsproc = run_program(argv);
	FILE *fp = fdopen(btrfsproc.pipefd, "r");
	if (!fp) {
		perror("fdopen");
		exit(1);
	}
	/* Read from [ until ] */
	int ret = fscanf(fp, "[%[^]]", dev);
	if (ret == 0 || ret == EOF) {
		fprintf(stderr, "Cannot find device that contains the filesystem\n");
		exit(1);
	}
	fclose(fp);
	siginfo_t cmd_state;
	waitid(P_PID, btrfsproc.pid, &cmd_state, WEXITED);
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		if (argc == 1)
			fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
		else
			fprintf(stderr, "Usage: sharedextent <filename>\n");
		return 1;
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	syncfs(fd);
	struct statx info;
	statx_info(&info, fd, argv[1]);
	close(fd);
	
	char *pattern;
	int ret;
	if (info.stx_subvol == 5)
		ret = asprintf(&pattern, "extent data backref root FS_ROOT objectid %llu", info.stx_ino);
	else
		ret = asprintf(&pattern, "extent data backref root %llu objectid %llu", info.stx_subvol, info.stx_ino);
	if (ret < 0)
		return ENOMEM;

	char dev[PATH_MAX];
	char *dirpath = dirname(argv[1]);
	find_device(dev, dirpath);

	const char *newargv[] = {"btrfs", "inspect-internal", "dump-tree", "-t", "extent", dev, NULL};
	struct process btrfsproc = run_program(newargv);
	/* Takes ownership of pattern */
	auto result = handle_tree_dump(btrfsproc.pipefd, pattern, dirpath);
	siginfo_t cmd_state;
	if (waitid(P_PID, btrfsproc.pid, &cmd_state, WEXITED)) {
		perror("waitid");
		return 1;
	}
	if (cmd_state.si_code == CLD_KILLED) {
		fprintf(stderr, "btrfs dump-tree command was killed\n");
		return 1;
	}
	if (cmd_state.si_status != 0) {
		fprintf(stderr, "btrfs dump-tree command exited with status %i\n", cmd_state.si_status);
		return 1;
	}

	if (result.size() == 0) {
		printf("This file has only exclusive extents.\n");
	}
	else {
		printf("Extents shared among:\n");
		for (const std::string &str : result) {
			printf("%s\n", str.c_str());
		}
	}

	return 0;
}