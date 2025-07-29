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
#include <stdint.h>

#include <unordered_set>
#include <string>
#include <utility>
#include <functional>
#include <vector>

static bool debug_output = false;

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

using file_id = std::pair<uint64_t, uint64_t>;
inline void hash_combine(size_t *seed, size_t hash)
{
	/* Magic constant from Boost (fractional part of the golden ratio) */
	const size_t golden = 0x9e3779b9;

	/* Combine the new hash into the seed */
	*seed ^= hash + golden + (*seed << 6) + (*seed >> 2);
}
/* Specialize std::hash for file_id */
namespace std
{
	template <>
	struct hash<file_id> {
		size_t operator()(const file_id &fid) const
		{
			size_t seed = 0;
			hash_combine(&seed, hash<uint64_t>{}(fid.first));
			hash_combine(&seed, hash<uint64_t>{}(fid.second));
			return seed;
		}
	};
}

file_id extract_id(const char *buf)
{
	unsigned long long int subvolid, inode;
	char subvolid_str[21];
	int ret = sscanf(buf, "%*[^)]) extent data backref root %s objectid %llu", &subvolid_str, &inode);
	if (ret != 2) {
		if (debug_output)
			fprintf(stderr, "Failed to read subvolume ID from string \"%s\"", buf);
		return {0,0};
	}
	if (!strcmp(subvolid_str, "FS_ROOT")) {
		subvolid = 5;
	}
	else {
		ret = sscanf(subvolid_str, "%llu", &subvolid);
		if (ret != 1) {
			if (debug_output)
				fprintf(stderr, "Failed to read subvolume ID from string \"%s\"", buf);
			return {0,0};
		}
	}
	return {inode, subvolid};
}

std::unordered_set<std::string> handle_tree_dump(int fd, file_id target, char *dirpath)
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
	#define BACKREF_SPECULATIVE 3
	#define BACKREF 4
	int state = PRESTART;
	char logical_addr[128];	/* Captured as string to pass it to btrfs command */
	int refcount;
	std::unordered_set<std::string> result;
	std::vector<file_id> speculative_visited_ids;
	std::unordered_set<file_id> visited_ids;
	size_t old_size = 0;
	while (fgets(buf, len, fp)) {
		if (strstr(buf, "item")) {
			if (state == BACKREF && visited_ids.size() > old_size) {
				/* There was new ID spotted */
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
			}
			old_size = visited_ids.size();
			speculative_visited_ids.clear();
			state = START;
		}
		if (state == START) {
			int ret = sscanf(buf, "%*[ \t]item %*llu key (%s", logical_addr);
			if (ret == 1)
				state = REFLINE;
			else
				state = PRESTART;
			continue;
		}
		if (state == REFLINE) {
			int ret = sscanf(buf, "%*[ \t]refs %d", &refcount);
			if (refcount > 1 && ret == 1)
				state = BACKREF_SPECULATIVE;
			else
				state = PRESTART;
			continue;
		}
		if (state == BACKREF_SPECULATIVE) {
			file_id this_extent = extract_id(buf);
			if (this_extent == file_id{0,0})
				continue;
			if (this_extent == target) {
				for (const file_id &id : speculative_visited_ids) {
					visited_ids.insert(id);
				}
				state = BACKREF;
			}
			else {
				speculative_visited_ids.push_back(this_extent);
			}
		}
		if (state == BACKREF) {
			file_id this_extent = extract_id(buf);
			if (this_extent == file_id{0,0})
				continue;
			visited_ids.insert(this_extent);
		}
	}
	fclose(fp);
	free(buf);
	
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
	if (argc == 3) {
		if (!strcmp(argv[1], "-d")) {
			debug_output = true;
			argv[1] = argv[2];
		}
		else {
			fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
			return 1;
		}
	}
	if (argc < 2) {
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

	char dev[PATH_MAX];
	char *dirpath = dirname(argv[1]);
	find_device(dev, dirpath);

	const char *newargv[] = {"btrfs", "inspect-internal", "dump-tree", "-t", "extent", dev, NULL};
	struct process btrfsproc = run_program(newargv);
	auto result = handle_tree_dump(btrfsproc.pipefd, {info.stx_ino, info.stx_subvol}, dirpath);
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
			printf("%s", str.c_str());
		}
	}

	return 0;
}