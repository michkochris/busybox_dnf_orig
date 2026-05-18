/* vi: set sw=4 ts=4: */
/*
 * busybox_dnf - A lightweight package management frontend for BusyBox
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */

//applet:IF_DNF(APPLET(dnf, BB_DIR_USR_BIN, BB_SUID_DROP))

//usage:#define dnf_trivial_usage "[-y] COMMAND [PACKAGE...]"
//usage:#define dnf_full_usage "\n\n" \
//usage:       "High-level package management frontend\n" \
//usage:     "\nOptions:" \
//usage:     "\n	-y, --assumeyes	Answer yes for all questions" \
//usage:     "\n	-v, --verbose	Verbose output" \
//usage:     "\n\nCommands:" \
//usage:     "\n	check-update	Check for available package updates" \
//usage:     "\n	update		Update the system" \
//usage:     "\n	search		Search for a package" \
//usage:     "\n	info		Display details about a package"

#include "libbb.h"
#include <sys/utsname.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ANSI Colors */
#define CLR_GREEN  "\033[0;32m"
#define CLR_RED    "\033[0;31m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

/* State machine enum to track progress cleanly */
typedef enum {
	STATE_FETCH_ROUTER,
	STATE_PARSE_METALINK,
	STATE_SELECT_OPTIMUM,
	STATE_FETCH_REPOMD,
	STATE_PARSE_REPOMD,
	STATE_FETCH_PRIMARY,
	STATE_PARSE_PRIMARY,
	STATE_EXECUTE_PAYLOAD,
	STATE_ERROR
} dnf_state_t;

#define MAX_CANDIDATE_MIRRORS 5
#define URL_MAX_LEN 256
#define MAX_REPOS 16

/* Node structure to track candidate targets extracted from the XML stream */
typedef struct {
	char url[URL_MAX_LEN];
	int priority;
} mirror_node_t;

typedef struct {
	char *id;
	char *metalink;
	int enabled;
} repo_t;

/* Context tracking structure to eliminate variable leakage */
typedef struct {
	char *releasever;
	char *basearch;
	char *search_term;
	char *info_target;
	int verbose;
	repo_t repos[MAX_REPOS];
	int repo_count;
	int current_repo_idx;

	char master_url[URL_MAX_LEN];
	char target_mirror_url[URL_MAX_LEN];
	char primary_xml_url[URL_MAX_LEN];
	mirror_node_t candidates[MAX_CANDIDATE_MIRRORS];
	int candidate_count;
} dnf_context_t;

static void update_progress(int current, int total, const char *msg UNUSED_PARAM)
{
	unsigned width, height;
	int i, pos, percent;
	int bar_space;

	get_terminal_width_height(STDOUT_FILENO, &width, &height);
	if (width < 30) return;

	percent = (current * 100) / total;
	/* Width minus overhead for "Progress: [100%] [" (18) and "]" (1) and safety (5) */
	bar_space = width - 24;
	if (bar_space < 10) bar_space = 10;
	pos = (percent * bar_space) / 100;

	/* Move to the absolute bottom line (outside scrolling region) */
	printf("\033[%d;1H\033[K", height);

	printf("Progress: [%3d%%] [", percent);
	for (i = 0; i < bar_space; i++) {
		if (i < pos) printf("#");
		else printf(".");
	}
	printf("]");
	fflush(stdout);
}

static char *str_replace_vars(const char *str, const char *releasever, const char *basearch)
{
	char *res = xstrdup(str);
	char *p;

	while ((p = strstr(res, "$releasever")) != NULL) {
		char *tmp = xasprintf("%.*s%s%s", (int)(p - res), res, releasever, p + 11);
		free(res);
		res = tmp;
	}
	while ((p = strstr(res, "$basearch")) != NULL) {
		char *tmp = xasprintf("%.*s%s%s", (int)(p - res), res, basearch, p + 9);
		free(res);
		res = tmp;
	}
	return res;
}

static char *get_releasever(void)
{
	char *version = NULL;
	FILE *fp = fopen_for_read("/etc/fedora-release");
	if (fp) {
		char *line = xmalloc_fgetline(fp);
		if (line) {
			char *p = strstr(line, "release ");
			if (p) {
				p += 8;
				char *end = strpbrk(p, " \t\n");
				if (end) *end = '\0';
				version = xstrdup(p);
			}
			free(line);
		}
		fclose(fp);
	}
	if (!version) {
		fp = fopen_for_read("/etc/os-release");
		if (fp) {
			char *line;
			while ((line = xmalloc_fgetline(fp)) != NULL) {
				if (strncmp(line, "VERSION_ID=", 11) == 0) {
					char *v = line + 11;
					if (*v == '"') {
						v++;
						char *end = strchr(v, '"');
						if (end) *end = '\0';
					}
					version = xstrdup(v);
					free(line);
					break;
				}
				free(line);
			}
			fclose(fp);
		}
	}
	return version ? version : xstrdup("41");
}

static char *get_basearch(void)
{
	struct utsname uts;
	uname(&uts);
	if (strcmp(uts.machine, "i686") == 0) return xstrdup("i386");
	return xstrdup(uts.machine);
}

static void load_repos(dnf_context_t *ctx)
{
	DIR *dir = opendir("/etc/yum.repos.d");
	if (!dir) return;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		if (!strrstr(entry->d_name, ".repo")) continue;

		char *path = xasprintf("/etc/yum.repos.d/%s", entry->d_name);
		parser_t *p = config_open(path);
		free(path);
		if (!p) continue;

		char *tokens[2];
		repo_t *curr_repo = NULL;

		/*
		 * The delimiter string "#= " sets '#' as the comment character. 
		 * Using "= " (as before) caused the parser to incorrectly treat '=' 
		 * as a comment, silently dropping crucial keys like 'metalink='.
		 */
		while (config_read(p, tokens, 2, 0, "#= ", PARSE_NORMAL | PARSE_GREEDY)) {
			if (tokens[0][0] == '[') {
				char *id = xstrdup(tokens[0] + 1);
				char *end = strchr(id, ']');
				if (end) *end = '\0';

				if (ctx->repo_count < MAX_REPOS) {
					curr_repo = &ctx->repos[ctx->repo_count++];
					curr_repo->id = id;
					/* 
					 * DNF/YUM defaults to treating a repo as enabled if the 'enabled=' 
					 * key is omitted from the .repo file. Defaulting to 1 matches 
					 * expected behavior and prevents initialization failures.
					 */
					curr_repo->enabled = 1;
					curr_repo->metalink = NULL;
				} else {
					free(id);
					curr_repo = NULL;
				}
			} else if (curr_repo && tokens[1]) {
				if (strcmp(tokens[0], "enabled") == 0) {
					curr_repo->enabled = atoi(tokens[1]);
				} else if (strcmp(tokens[0], "metalink") == 0) {
					curr_repo->metalink = str_replace_vars(tokens[1], ctx->releasever, ctx->basearch);
				}
			}
		}
		config_close(p);
	}
	closedir(dir);
}

/* Lightweight stream-scanning helper to isolate mirror strings from XML */
static void parse_metalink_stream(dnf_context_t *ctx, const char *cache_path)
{
	FILE *fp = fopen_for_read(cache_path);
	if (!fp)
		return;

	char line[512];
	ctx->candidate_count = 0;

	/* Fast pointer-scanning matching standard BusyBox token logic */
	while (fgets(line, sizeof(line), fp) && ctx->candidate_count < MAX_CANDIDATE_MIRRORS) {
		char *url_start = strstr(line, "<url type=\"http");
		if (url_start) {
			url_start = strchr(url_start, '>') + 1; /* Move past closing bracket */
			char *url_end = strchr(url_start, '<'); /* Find closing tag */

			if (url_end && (url_end - url_start) < URL_MAX_LEN) {
				mirror_node_t *node = &ctx->candidates[ctx->candidate_count];
				memcpy(node->url, url_start, url_end - url_start);
				node->url[url_end - url_start] = '\0';
				node->priority = ctx->candidate_count + 1;
				ctx->candidate_count++;
			}
		}
	}
	fclose(fp);
}

static void parse_repomd_stream(dnf_context_t *ctx, const char *cache_path)
{
	FILE *fp = fopen_for_read(cache_path);
	if (!fp)
		return;

	char line[1024];
	int in_primary = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "<data type=\"primary\">")) {
			in_primary = 1;
		}
		if (in_primary) {
			char *loc = strstr(line, "<location href=\"");
			if (loc) {
				loc += 16;
				char *end = strchr(loc, '\"');
				if (end && (end - loc) < URL_MAX_LEN) {
					int tlen = strlen(ctx->target_mirror_url);
					while (tlen > 0 && ctx->target_mirror_url[tlen-1] == '/')
						tlen--;
					while (loc[0] == '/')
						loc++;
					snprintf(ctx->primary_xml_url, URL_MAX_LEN, "%.*s/%s", tlen, ctx->target_mirror_url, loc);
					in_primary = 0;
					break;
				}
			}
		}
		if (strstr(line, "</data>")) {
			in_primary = 0;
		}
	}
	fclose(fp);
}

static void dnf_debug_context(dnf_context_t *ctx, dnf_state_t state)
{
	if (!ctx->verbose) return;

	printf("\n--- FSM DEBUG [State: %d] ---\n", state);
	printf("  Release: %s, Arch: %s\n", ctx->releasever, ctx->basearch);
	printf("  Current Repo Index: %d (%s)\n", ctx->current_repo_idx,
		   ctx->repo_count > 0 ? ctx->repos[ctx->current_repo_idx].id : "none");
	printf("  Master URL: %s\n", ctx->master_url);
	printf("  Target Mirror: %s\n", ctx->target_mirror_url[0] ? ctx->target_mirror_url : "none");
	printf("  Primary XML: %s\n", ctx->primary_xml_url[0] ? ctx->primary_xml_url : "none");
	printf("  Candidates: %d cached\n", ctx->candidate_count);
	printf("----------------------------\n\n");
}

static void parse_primary_stream(dnf_context_t *ctx, const char *cache_path)
{
	char *cmd = xasprintf("zcat %s 2>/dev/null || xzcat %s 2>/dev/null || cat %s", cache_path, cache_path, cache_path);
	FILE *fp = popen(cmd, "r");
	free(cmd);

	if (!fp)
		return;

	char line[4096];
	char name[256] = {0}, arch[32] = {0}, summary[512] = {0};
	char version[64] = {0}, release[64] = {0}, license[128] = {0};
	char url[256] = {0}, description[2048] = {0};
	int in_package = 0, in_description = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "<package type=\"rpm\">")) {
			in_package = 1;
			name[0] = arch[0] = summary[0] = version[0] = release[0] = 0;
			license[0] = url[0] = description[0] = 0;
		}

		if (in_package) {
			char *p;
			if ((p = strstr(line, "<name>")) != NULL) {
				p += 6; char *end = strchr(p, '<');
				if (end) { memcpy(name, p, end - p); name[end - p] = '\0'; }
			} else if ((p = strstr(line, "<arch>")) != NULL) {
				p += 6; char *end = strchr(p, '<');
				if (end) { memcpy(arch, p, end - p); arch[end - p] = '\0'; }
			} else if ((p = strstr(line, "<summary>")) != NULL) {
				p += 9; char *end = strchr(p, '<');
				if (end) { memcpy(summary, p, end - p); summary[end - p] = '\0'; }
			} else if ((p = strstr(line, "<version")) != NULL) {
				char *v = strstr(p, " ver=\"");
				char *r = strstr(p, " rel=\"");
				if (v) {
					v += 6; char *end = strchr(v, '\"');
					if (end) { memcpy(version, v, end - v); version[end - v] = '\0'; }
				}
				if (r) {
					r += 6; char *end = strchr(r, '\"');
					if (end) { memcpy(release, r, end - r); release[end - r] = '\0'; }
				}
			} else if ((p = strstr(line, "<license>")) != NULL) {
				p += 9; char *end = strchr(p, '<');
				if (end) { memcpy(license, p, end - p); license[end - p] = '\0'; }
			} else if ((p = strstr(line, "<url>")) != NULL) {
				p += 5; char *end = strchr(p, '<');
				if (end) { memcpy(url, p, end - p); url[end - p] = '\0'; }
			} else if (strstr(line, "<description>")) {
				in_description = 1;
				description[0] = '\0';
			} else if (in_description) {
				if (strstr(line, "</description>")) {
					in_description = 0;
				} else {
					strncat(description, line, sizeof(description) - strlen(description) - 1);
				}
			}
		}

		if (strstr(line, "</package>")) {
			in_package = 0;
			if (ctx->search_term && (strstr(name, ctx->search_term) || strstr(summary, ctx->search_term))) {
				printf("%s.%s : %s\n", name, arch, summary);
			} else if (ctx->info_target && strcmp(name, ctx->info_target) == 0) {
				printf("\033[1mName         \033[0m: %s\n", name);
				printf("\033[1mVersion      \033[0m: %s\n", version);
				printf("\033[1mRelease      \033[0m: %s\n", release);
				printf("\033[1mArchitecture \033[0m: %s\n", arch);
				printf("\033[1mLicense      \033[0m: %s\n", license);
				printf("\033[1mURL          \033[0m: %s\n", url);
				printf("\033[1mSummary      \033[0m: %s\n", summary);
				printf("\033[1mDescription  \033[0m: %s\n", trim(description));
				printf("\n");
			}
		}
	}
	pclose(fp);
}

static int run_dnf_update_cycle(dnf_context_t *ctx, int repo_idx)
{
	dnf_state_t state = STATE_FETCH_ROUTER;
	char *cache_file = xasprintf("/var/cache/dnf/%s_metalink.xml", ctx->repos[repo_idx].id);
	char *repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[repo_idx].id);
	char *primary_file = xasprintf("/var/cache/dnf/%s_primary.xml.gz", ctx->repos[repo_idx].id);
	int success = 0;

	ctx->current_repo_idx = repo_idx;
	safe_strncpy(ctx->master_url, ctx->repos[repo_idx].metalink, URL_MAX_LEN);
	ctx->primary_xml_url[0] = '\0';

	while (state != STATE_EXECUTE_PAYLOAD && state != STATE_ERROR) {
		dnf_debug_context(ctx, state);
		switch (state) {
		case STATE_FETCH_ROUTER:
			printf("STATE: FETCH_ROUTER (%s) ... ", ctx->repos[ctx->current_repo_idx].id);
			{
				mkdir("/var/cache/dnf", 0755);
				char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", cache_file, ctx->master_url);
				int rc = system(wget_cmd);
				free(wget_cmd);

				if (rc == 0 || access(cache_file, R_OK) == 0) {
					printf("PASS\n");
					state = STATE_PARSE_METALINK;
				} else {
					printf("FAIL\n");
					state = STATE_ERROR;
				}
			}
			break;

		case STATE_PARSE_METALINK:
			printf("STATE: PARSE_METALINK ... ");
			parse_metalink_stream(ctx, cache_file);
			if (ctx->candidate_count > 0) {
				printf("PASS (%d mirrors found)\n", ctx->candidate_count);
				state = STATE_SELECT_OPTIMUM;
			} else {
				printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_SELECT_OPTIMUM:
			printf("STATE: SELECT_OPTIMUM ... ");
			if (ctx->candidate_count > 0) {
				safe_strncpy(ctx->target_mirror_url, ctx->candidates[0].url, URL_MAX_LEN);
				printf("PASS (%s)\n", ctx->target_mirror_url);
				state = STATE_FETCH_REPOMD;
			} else {
				printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_REPOMD:
		{
			char *url = xasprintf("%s/repodata/repomd.xml", ctx->target_mirror_url);
			printf("STATE: FETCH_REPOMD ... ");
			char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", repomd_file, url);
			int rc = system(wget_cmd);
			free(wget_cmd);
			free(url);

			if (rc == 0 || access(repomd_file, R_OK) == 0) {
				printf("PASS\n");
				state = STATE_PARSE_REPOMD;
			} else {
				printf("FAIL\n");
				state = STATE_ERROR;
			}
		}
		break;

		case STATE_PARSE_REPOMD:
			printf("STATE: PARSE_REPOMD ... ");
			parse_repomd_stream(ctx, repomd_file);
			if (ctx->primary_xml_url[0] != '\0') {
				printf("PASS\n");
				state = STATE_FETCH_PRIMARY;
			} else {
				printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_PRIMARY:
			printf("STATE: FETCH_PRIMARY ... ");
			{
				char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", primary_file, ctx->primary_xml_url);
				int rc = system(wget_cmd);
				free(wget_cmd);

				if (rc == 0 || access(primary_file, R_OK) == 0) {
					printf("PASS\n");
					state = STATE_PARSE_PRIMARY;
				} else {
					printf("FAIL\n");
					state = STATE_ERROR;
				}
			}
			break;

		case STATE_PARSE_PRIMARY:
			printf("STATE: PARSE_PRIMARY ... ");
			/* We parse during search or if we need to populate a database */
			if (ctx->search_term) {
				printf("PASS\n");
				parse_primary_stream(ctx, primary_file);
			} else {
				printf("PASS (skipped)\n");
			}
			state = STATE_EXECUTE_PAYLOAD;
			success = 1;
			break;

		default:
			state = STATE_ERROR;
			break;
		}
	}

	free(cache_file);
	free(repomd_file);
	free(primary_file);
	return success;
}

static int sync_repos(dnf_context_t *ctx)
{
	int synced = 0;
	int total_enabled = 0;
	unsigned width, height;

	for (int i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && ctx->repos[i].metalink)
			total_enabled++;
	}

	if (total_enabled == 0) return 0;

	get_terminal_width_height(STDOUT_FILENO, &width, &height);
	/* Set scrolling region to leave the bottom line for the progress bar */
	printf("\033[1;%dr", height - 1);

	for (int i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && ctx->repos[i].metalink) {
			update_progress(synced, total_enabled, ctx->repos[i].id);
			/* Move cursor to the line above the progress bar */
			printf("\033[%d;1H", height - 1);
			if (run_dnf_update_cycle(ctx, i))
				synced++;
		}
	}
	update_progress(total_enabled, total_enabled, "Done");
	/* Reset scrolling region and move to bottom */
	printf("\033[r\033[%d;1H\n", height);

	return synced;
}

int dnf_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int dnf_main(int argc UNUSED_PARAM, char **argv)
{
	dnf_context_t ctx;
	unsigned opts;

	/* Initialize context */
	memset(&ctx, 0, sizeof(ctx));

	/* Parse options */
	opts = getopt32(argv, "yv");
	if (opts & 2) ctx.verbose = 1;
	argv += optind;

	if (!argv[0])
		bb_show_usage();

	const char *command = argv[0];

	if (strcmp(command, "update") == 0 || strcmp(command, "check-update") == 0) {
		printf("STATE: INIT ... ");
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		printf("PASS (Fedora %s, %s)\n", ctx.releasever, ctx.basearch);

		printf("STATE: LOAD_REPOS ... ");
		load_repos(&ctx);
		printf("PASS (%d repos found)\n", ctx.repo_count);

		if (sync_repos(&ctx) > 0) {
			printf("\033[1;32mUpdating and loading repositories:\033[0m\n");
			printf("\033[1;32mRepositories loaded.\033[0m\n");
			/* Further logic for update would go here */
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "search") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.search_term = argv[1];
		load_repos(&ctx);

		if (sync_repos(&ctx) > 0) {
			printf("\033[1;32mUpdating and loading repositories:\033[0m\n");
			printf("\033[1;32mRepositories loaded.\033[0m\n");
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "info") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.info_target = argv[1];
		if (!ctx.info_target) bb_show_usage();
		load_repos(&ctx);

		if (sync_repos(&ctx) > 0) {
			printf("\033[1;32mUpdating and loading repositories:\033[0m\n");
			printf("\033[1;32mRepositories loaded.\033[0m\n");
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else {
		bb_error_msg_and_die("command '%s' not yet implemented", command);
	}

	return EXIT_SUCCESS;
}
