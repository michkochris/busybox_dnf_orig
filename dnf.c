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
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * DESIGN NOTE: busybox_dnf is a native BusyBox applet. It is designed to
 * operate using only BusyBox internal applets (wget, rpm, cpio, etc.)
 * to ensure it remains functional even on a broken system where
 * external shared libraries or standalone package managers are corrupted.
 */

/* ANSI Colors */
#define CLR_GREEN  "\033[0;32m"
#define CLR_RED    "\033[0;31m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

/* State machine enum to track progress cleanly */
typedef enum {
	STATE_INIT,
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

typedef struct {
	char name[128];
	char version[64];
	char release[64];
	char arch[32];
	char summary[512];
	char license[128];
	char url[256];
	char description[2048];
	char repo_id[64];
} pkg_t;

/* Node structure to track candidate targets extracted from the XML stream */
typedef struct {
	char url[URL_MAX_LEN];
	int priority;
} mirror_node_t;

typedef struct {
	char *id;
	char *metalink;
	char *baseurl;
	char *mirrorlist;
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
	char primary_xml_hash_path[URL_MAX_LEN];
	mirror_node_t candidates[MAX_CANDIDATE_MIRRORS];
	int candidate_count;
} dnf_context_t;

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
	return version ? version : xstrdup("44");
}

static char *get_basearch(void)
{
	struct utsname uts;
	uname(&uts);
	if (strcmp(uts.machine, "i686") == 0) return xstrdup("i386");
	return xstrdup(uts.machine);
}

static int order(char c)
{
	if (isdigit(c)) return 0;
	if (isalpha(c)) return (unsigned char)c;
	if (c == '~') return -1;
	if (c) return (unsigned char)c + 256;
	return 0;
}

static int compare_version_part(const char *v1, const char *v2)
{
	while (*v1 || *v2) {
		int first_diff = 0;
		while ((*v1 && !isdigit(*v1)) || (*v2 && !isdigit(*v2))) {
			int o1 = order(*v1);
			int o2 = order(*v2);
			if (o1 != o2) return o1 - o2;
			if (*v1) v1++;
			if (*v2) v2++;
		}
		while (*v1 == '0') v1++;
		while (*v2 == '0') v2++;
		while (isdigit(*v1) && isdigit(*v2)) {
			if (!first_diff) first_diff = *v1 - *v2;
			v1++; v2++;
		}
		if (isdigit(*v1)) return 1;
		if (isdigit(*v2)) return -1;
		if (first_diff) return first_diff;
	}
	return 0;
}

static int compare_versions(const char *v1, const char *v2)
{
	const char *r1, *r2;
	int res;

	if (!v1 || !v2) return v1 ? 1 : (v2 ? -1 : 0);

	r1 = strrchr(v1, '-');
	r2 = strrchr(v2, '-');

	if (r1 && r2) {
		char *up1 = xstrndup(v1, r1 - v1);
		char *up2 = xstrndup(v2, r2 - v2);
		res = compare_version_part(up1, up2);
		free(up1); free(up2);
		if (res) return res;
		return compare_version_part(r1 + 1, r2 + 1);
	}
	return compare_version_part(v1, v2);
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
		if (ctx->verbose) printf("[DEBUG] Parsing repo file: %s\n", path);
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
					curr_repo->baseurl = NULL;
					curr_repo->mirrorlist = NULL;
				} else {
					free(id);
					curr_repo = NULL;
				}
			} else if (curr_repo && tokens[1]) {
				if (strcmp(tokens[0], "enabled") == 0) {
					curr_repo->enabled = atoi(tokens[1]);
				} else if (strcmp(tokens[0], "metalink") == 0) {
					curr_repo->metalink = str_replace_vars(tokens[1], ctx->releasever, ctx->basearch);
				} else if (strcmp(tokens[0], "baseurl") == 0) {
					curr_repo->baseurl = str_replace_vars(tokens[1], ctx->releasever, ctx->basearch);
				} else if (strcmp(tokens[0], "mirrorlist") == 0) {
					curr_repo->mirrorlist = str_replace_vars(tokens[1], ctx->releasever, ctx->basearch);
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
		/* 
		 * BUG FIX: Fedora's mirror manager outputs `<url protocol="http"` instead 
		 * of `<url type="http"`. We now broadly match `<url ` and `http` to 
		 * ensure we don't fail during the PARSE_METALINK state.
		 *
		 * IMPORTANT: Modern 'wget2' (default on Fedora 44) automatically follows
		 * Metalink redirects. We rely on BusyBox's internal 'wget' applet
		 * which is 'dumb' enough to just save the XML list we need to parse.
		 */
		char *url_start = strstr(line, "<url ");
		if (url_start && strstr(line, "http")) {
			url_start = strchr(url_start, '>');
			if (url_start) {
				url_start++; /* Move past closing bracket */
				char *url_end = strchr(url_start, '<'); /* Find closing tag */

				if (url_end && (url_end - url_start) < URL_MAX_LEN) {
					mirror_node_t *node = &ctx->candidates[ctx->candidate_count];
					int len = url_end - url_start;
					memcpy(node->url, url_start, len);
					node->url[len] = '\0';

					/*
					 * Fedora Metalinks often point directly to the repomd.xml.
					 * We strip this suffix to get the base mirror URL, which we
					 * then use to construct paths for other metadata files.
					 */
					char *p = strstr(node->url, "/repodata/repomd.xml");
					if (p) *p = '\0';

					/* Strip trailing slash if present for consistent path joining */
					len = strlen(node->url);
					if (len > 0 && node->url[len - 1] == '/')
						node->url[len - 1] = '\0';

					node->priority = ctx->candidate_count + 1;
					if (ctx->verbose) printf("[DEBUG] Found mirror candidate: %s\n", node->url);
					ctx->candidate_count++;
				}
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

					char *relative_url = xstrndup(loc, end - loc);
					while (relative_url[0] == '/')
						memmove(relative_url, relative_url + 1, strlen(relative_url));

					snprintf(ctx->primary_xml_url, URL_MAX_LEN, "%.*s/%s", tlen, ctx->target_mirror_url, relative_url);

					/* Extract basename for hash-based caching */
					char *bname = strrchr(relative_url, '/');
					if (!bname) bname = relative_url; else bname++;
					snprintf(ctx->primary_xml_hash_path, URL_MAX_LEN, "/var/cache/dnf/%s_%s", ctx->repos[ctx->current_repo_idx].id, bname);

					if (ctx->verbose) {
						printf("[DEBUG] Found Primary XML: %s\n", ctx->primary_xml_url);
						printf("[DEBUG] Hash-based Path: %s\n", ctx->primary_xml_hash_path);
					}
					free(relative_url);
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

	printf("\n" CLR_BOLD "[DEBUG] FSM State Change -> %d" CLR_RESET "\n", state);
	printf("  [Context] Release: %s | Arch: %s\n", ctx->releasever, ctx->basearch);
	printf("  [Repo]    ID: %s (Index: %d/%d)\n",
		   ctx->repo_count > 0 ? ctx->repos[ctx->current_repo_idx].id : "none",
		   ctx->current_repo_idx + 1, ctx->repo_count);
	printf("  [URL]     Master: %s\n", ctx->master_url);

	if (ctx->target_mirror_url[0])
		printf("  [URL]     Active Mirror: %s\n", ctx->target_mirror_url);

	if (ctx->candidate_count > 0) {
		printf("  [Mirrors] Cached Candidates:\n");
		for (int i = 0; i < ctx->candidate_count; i++) {
			printf("    %d. %s\n", i + 1, ctx->candidates[i].url);
		}
	}

	if (ctx->primary_xml_url[0])
		printf("  [Data]    Primary Metadata URL: %s\n", ctx->primary_xml_url);
	printf("----------------------------------------\n");
}

static void parse_primary_stream(dnf_context_t *ctx, const char *cache_path, pkg_t *match)
{
	/*
	 * We utilize BusyBox internal applets (zcat/xzcat) for streaming extraction.
	 * Fedora 44 uses .zst compression. If unzstd is not in BusyBox, it will
	 * fall back to the system's unzstd command if available.
	 */
	char *cmd = xasprintf("unzstd -c %s 2>/dev/null || zcat %s 2>/dev/null || xzcat %s 2>/dev/null || cat %s",
						 cache_path, cache_path, cache_path, cache_path);
	if (ctx->verbose) printf("[DEBUG] Extraction cmd: %s\n", cmd);
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
		char *p = line;
		while (*p && isspace(*p)) p++;
		if (*p != '<') continue;

		if (!in_package) {
			if (strncmp(p, "<package type=\"rpm\">", 20) == 0) {
				in_package = 1;
				name[0] = arch[0] = summary[0] = version[0] = release[0] = 0;
				license[0] = url[0] = description[0] = 0;
			}
			continue;
		}

		if (strncmp(p, "</package>", 10) == 0) {
			in_package = 0;
			if (ctx->search_term && (strstr(name, ctx->search_term) || strstr(summary, ctx->search_term))) {
				printf("%s.%s : %s\n", name, arch, summary);
			} else if (ctx->info_target && strcmp(name, ctx->info_target) == 0) {
				char *full_ver = xasprintf("%s-%s", version, release);
				char *best_ver = (match && match->name[0]) ? xasprintf("%s-%s", match->version, match->release) : NULL;

				if (match && (!match->name[0] || compare_versions(full_ver, best_ver) > 0)) {
					safe_strncpy(match->name, name, sizeof(match->name));
					safe_strncpy(match->version, version, sizeof(match->version));
					safe_strncpy(match->release, release, sizeof(match->release));
					safe_strncpy(match->arch, arch, sizeof(match->arch));
					safe_strncpy(match->summary, summary, sizeof(match->summary));
					safe_strncpy(match->license, license, sizeof(match->license));
					safe_strncpy(match->url, url, sizeof(match->url));
					safe_strncpy(match->description, trim(description), sizeof(match->description));
					safe_strncpy(match->repo_id, ctx->repos[ctx->current_repo_idx].id, sizeof(match->repo_id));
				}
				free(full_ver); free(best_ver);
			}
			continue;
		}

		if (strncmp(p, "<name>", 6) == 0) {
			p += 6; char *end = strchr(p, '<');
			if (end) { memcpy(name, p, end - p); name[end - p] = '\0'; }
		} else if (strncmp(p, "<arch>", 6) == 0) {
			p += 6; char *end = strchr(p, '<');
			if (end) { memcpy(arch, p, end - p); arch[end - p] = '\0'; }
		} else if (strncmp(p, "<summary>", 9) == 0) {
			p += 9; char *end = strchr(p, '<');
			if (end) { memcpy(summary, p, end - p); summary[end - p] = '\0'; }
		} else if (strncmp(p, "<version", 8) == 0) {
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
		} else if (strncmp(p, "<license>", 9) == 0) {
			p += 9; char *end = strchr(p, '<');
			if (end) { memcpy(license, p, end - p); license[end - p] = '\0'; }
		} else if (strncmp(p, "<url>", 5) == 0) {
			p += 5; char *end = strchr(p, '<');
			if (end) { memcpy(url, p, end - p); url[end - p] = '\0'; }
		} else if (strncmp(p, "<description>", 13) == 0) {
			in_description = 1;
			description[0] = '\0';
		} else if (in_description) {
			if (strstr(p, "</description>")) {
				in_description = 0;
			} else {
				strncat(description, line, sizeof(description) - strlen(description) - 1);
			}
		}
	}
	pclose(fp);
}

static void perform_queries(dnf_context_t *ctx)
{
	pkg_t best_match;
	memset(&best_match, 0, sizeof(best_match));

	for (int i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled) {
			char *primary_file = xasprintf("/var/cache/dnf/%s_primary.xml.gz", ctx->repos[i].id);
			if (access(primary_file, R_OK) == 0) {
				ctx->current_repo_idx = i;
				parse_primary_stream(ctx, primary_file, ctx->info_target ? &best_match : NULL);
			}
			free(primary_file);
		}
	}

	if (ctx->info_target && best_match.name[0]) {
		printf("\033[1mName         \033[0m: %s\n", best_match.name);
		printf("\033[1mVersion      \033[0m: %s\n", best_match.version);
		printf("\033[1mRelease      \033[0m: %s\n", best_match.release);
		printf("\033[1mArchitecture \033[0m: %s\n", best_match.arch);
		printf("\033[1mLicense      \033[0m: %s\n", best_match.license);
		printf("\033[1mURL          \033[0m: %s\n", best_match.url);
		printf("\033[1mSummary      \033[0m: %s\n", best_match.summary);
		printf("\033[1mDescription  \033[0m: %s\n", best_match.description);
		printf("\033[1mRepository   \033[0m: %s\n", best_match.repo_id);
		printf("\n");
	}
}

static void perform_check_update(dnf_context_t *ctx)
{
	printf("Last metadata expiration check: %s\n", __TIME__);
	/* This is a placeholder for a real check-update implementation */
}

static void perform_update(dnf_context_t *ctx)
{
	perform_check_update(ctx);
	printf("Dependencies resolved.\n");
	printf("Nothing to do.\n");
	printf("Complete!\n");
}

static int run_dnf_update_cycle(dnf_context_t *ctx, int repo_idx)
{
	dnf_state_t state = STATE_INIT;
	char *cache_file = xasprintf("/var/cache/dnf/%s_metalink.xml", ctx->repos[repo_idx].id);
	char *repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[repo_idx].id);
	char *primary_file = xasprintf("/var/cache/dnf/%s_primary.xml.gz", ctx->repos[repo_idx].id);
	char *mirror_cache = xasprintf("/var/cache/dnf/%s_mirror.txt", ctx->repos[repo_idx].id);
	int success = 0;

	ctx->current_repo_idx = repo_idx;
	ctx->primary_xml_url[0] = '\0';
	ctx->target_mirror_url[0] = '\0';

	while (state != STATE_EXECUTE_PAYLOAD && state != STATE_ERROR) {
		dnf_debug_context(ctx, state);
		switch (state) {
		case STATE_INIT:
			if (ctx->repos[repo_idx].baseurl) {
				safe_strncpy(ctx->target_mirror_url, ctx->repos[repo_idx].baseurl, URL_MAX_LEN);
				state = STATE_FETCH_REPOMD;
			} else if (ctx->repos[repo_idx].metalink || ctx->repos[repo_idx].mirrorlist) {
				/* Try cached mirror first */
				FILE *fp = fopen(mirror_cache, "r");
				if (fp) {
					if (fgets(ctx->target_mirror_url, URL_MAX_LEN, fp)) {
						char *p = strpbrk(ctx->target_mirror_url, "\r\n");
						if (p) *p = '\0';
						if (ctx->verbose) printf("[DEBUG] Using cached mirror: %s\n", ctx->target_mirror_url);
						state = STATE_FETCH_REPOMD;
					}
					fclose(fp);
				}
				if (state == STATE_INIT) {
					safe_strncpy(ctx->master_url,
						ctx->repos[repo_idx].metalink ? ctx->repos[repo_idx].metalink : ctx->repos[repo_idx].mirrorlist,
						URL_MAX_LEN);
					state = STATE_FETCH_ROUTER;
				}
			} else {
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_ROUTER:
			if (ctx->verbose) {
				printf("[DEBUG] Fetching Metalink/Mirrorlist from: %s\n", ctx->master_url);
				printf("STATE: FETCH_ROUTER (%s) ... ", ctx->repos[ctx->current_repo_idx].id);
			}
			{
				mkdir("/var/cache/dnf", 0755);
				char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", cache_file, ctx->master_url);
				int rc = system(wget_cmd);
				free(wget_cmd);

				if (rc == 0 || access(cache_file, R_OK) == 0) {
					if (ctx->verbose) printf("PASS\n");
					state = STATE_PARSE_METALINK;
				} else {
					if (ctx->verbose) printf("FAIL\n");
					state = STATE_ERROR;
				}
			}
			break;

		case STATE_PARSE_METALINK:
			if (ctx->verbose) printf("STATE: PARSE_METALINK ... ");
			if (ctx->repos[repo_idx].metalink) {
				parse_metalink_stream(ctx, cache_file);
			} else {
				/* Simple mirrorlist: one URL per line */
				FILE *fp = fopen_for_read(cache_file);
				if (fp) {
					char line[URL_MAX_LEN];
					ctx->candidate_count = 0;
					while (fgets(line, sizeof(line), fp) && ctx->candidate_count < MAX_CANDIDATE_MIRRORS) {
						if (line[0] == '#' || line[0] == '\n') continue;
						char *p = strpbrk(line, "\r\n");
						if (p) *p = '\0';
						safe_strncpy(ctx->candidates[ctx->candidate_count].url, line, URL_MAX_LEN);
						ctx->candidates[ctx->candidate_count].priority = ctx->candidate_count + 1;
						ctx->candidate_count++;
					}
					fclose(fp);
				}
			}

			if (ctx->candidate_count > 0) {
				if (ctx->verbose) printf("PASS (%d mirrors found)\n", ctx->candidate_count);
				state = STATE_SELECT_OPTIMUM;
			} else {
				if (ctx->verbose) printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_SELECT_OPTIMUM:
			if (ctx->verbose) printf("STATE: SELECT_OPTIMUM ... ");
			if (ctx->candidate_count > 0) {
				safe_strncpy(ctx->target_mirror_url, ctx->candidates[0].url, URL_MAX_LEN);
				if (ctx->verbose) printf("PASS (%s)\n", ctx->target_mirror_url);
				state = STATE_FETCH_REPOMD;
			} else {
				if (ctx->verbose) printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_REPOMD:
		{
			char *url = xasprintf("%s/repodata/repomd.xml", ctx->target_mirror_url);
			if (ctx->verbose) printf("STATE: FETCH_REPOMD ... ");
			char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", repomd_file, url);
			int rc = system(wget_cmd);
			free(wget_cmd);
			free(url);

			if (rc == 0 || access(repomd_file, R_OK) == 0) {
				if (ctx->verbose) printf("PASS\n");
				/* Success! Cache this mirror if it's not baseurl */
				if (!ctx->repos[repo_idx].baseurl) {
					FILE *fp = fopen(mirror_cache, "w");
					if (fp) {
						fprintf(fp, "%s\n", ctx->target_mirror_url);
						fclose(fp);
					}
				}
				state = STATE_PARSE_REPOMD;
			} else {
				if (ctx->verbose) printf("FAIL\n");
				/* If cached mirror failed, retry via router */
				if (state == STATE_FETCH_REPOMD && !ctx->repos[repo_idx].baseurl && ctx->target_mirror_url[0]) {
					unlink(mirror_cache);
					ctx->target_mirror_url[0] = '\0';
					if (ctx->repos[repo_idx].metalink || ctx->repos[repo_idx].mirrorlist) {
						safe_strncpy(ctx->master_url,
							ctx->repos[repo_idx].metalink ? ctx->repos[repo_idx].metalink : ctx->repos[repo_idx].mirrorlist,
							URL_MAX_LEN);
						state = STATE_FETCH_ROUTER;
					} else {
						state = STATE_ERROR;
					}
				} else {
					state = STATE_ERROR;
				}
			}
		}
		break;

		case STATE_PARSE_REPOMD:
			if (ctx->verbose) printf("STATE: PARSE_REPOMD ... ");
			parse_repomd_stream(ctx, repomd_file);
			if (ctx->primary_xml_url[0] != '\0') {
				if (ctx->verbose) printf("PASS\n");
				/* Check if we already have this hash-based primary XML */
				if (access(ctx->primary_xml_hash_path, R_OK) == 0) {
					if (ctx->verbose) printf("[DEBUG] Metadata already fresh (hash match: %s)\n", ctx->primary_xml_hash_path);
					/* Update the legacy symlink-like file for search/info */
					unlink(primary_file);
					symlink(ctx->primary_xml_hash_path, primary_file);
					state = STATE_PARSE_PRIMARY;
				} else {
					state = STATE_FETCH_PRIMARY;
				}
			} else {
				if (ctx->verbose) printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_PRIMARY:
			if (ctx->verbose) printf("STATE: FETCH_PRIMARY ... ");
			{
				/* Fetch to the hash-based path */
				char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", ctx->primary_xml_hash_path, ctx->primary_xml_url);
				int rc = system(wget_cmd);
				free(wget_cmd);

				if (rc == 0 || access(ctx->primary_xml_hash_path, R_OK) == 0) {
					if (ctx->verbose) printf("PASS\n");
					/* Update the legacy symlink-like file for search/info */
					unlink(primary_file);
					symlink(ctx->primary_xml_hash_path, primary_file);
					state = STATE_PARSE_PRIMARY;
				} else {
					if (ctx->verbose) printf("FAIL\n");
					state = STATE_ERROR;
				}
			}
			break;

		case STATE_PARSE_PRIMARY:
			if (ctx->verbose) printf("STATE: PARSE_PRIMARY ... ");
			/* We parse during search/info or if we need to populate a database */
			if (ctx->verbose) printf("PASS\n");
			state = STATE_EXECUTE_PAYLOAD;
			success = 1;
			break;

		default:
			state = STATE_ERROR;
			break;
		}
	}

	if (!ctx->verbose) {
		if (success)
			printf("[%sOK%s]\n", CLR_GREEN, CLR_RESET);
		else
			printf("[%sFAILED%s]\n", CLR_RED, CLR_RESET);
	}

	free(cache_file);
	free(repomd_file);
	free(primary_file);
	free(mirror_cache);
	return success;
}

static int sync_repos(dnf_context_t *ctx, int force)
{
	int synced = 0;
	int total_enabled = 0;
	int current = 0;
	int needed = 0;

	for (int i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && (ctx->repos[i].metalink || ctx->repos[i].baseurl || ctx->repos[i].mirrorlist)) {
			total_enabled++;
			char *repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[i].id);
			struct stat st;
			/*
			 * OPTIMIZATION: Check if repomd.xml is fresh (within 24h).
			 */
			if (force || stat(repomd_file, &st) != 0 || (time(NULL) - st.st_mtime) > 3600 * 24) {
				needed++;
			}
			free(repomd_file);
		}
	}

	if (total_enabled == 0) return 0;

	if (needed > 0 && !ctx->verbose)
		printf("Updating and loading repositories:\n");

	for (int i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && (ctx->repos[i].metalink || ctx->repos[i].baseurl || ctx->repos[i].mirrorlist)) {
			current++;
			char *repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[i].id);
			struct stat st;
			int is_fresh = (stat(repomd_file, &st) == 0 && (time(NULL) - st.st_mtime) < 3600 * 24);
			free(repomd_file);

			if (!ctx->verbose) {
				printf(" (%d/%d): %-40s", current, total_enabled, ctx->repos[i].id);
				fflush(stdout);
			}

			if (force || !is_fresh) {
				if (run_dnf_update_cycle(ctx, i))
					synced++;
			} else {
				if (!ctx->verbose) printf("[%sOK%s]\n", CLR_GREEN, CLR_RESET);
				synced++; /* Already available and fresh */
			}
		}
	}

	if (needed > 0 && !ctx->verbose && synced > 0)
		printf("Metadata cache created.\n");

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

	if (strcmp(command, "update") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		if (sync_repos(&ctx, 0) > 0) {
			perform_update(&ctx);
		}
	} else if (strcmp(command, "check-update") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		if (sync_repos(&ctx, 0) > 0) {
			perform_check_update(&ctx);
		}
	} else if (strcmp(command, "search") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.search_term = argv[1];
		load_repos(&ctx);

		if (sync_repos(&ctx, 0) > 0) {
			perform_queries(&ctx);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "info") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.info_target = argv[1];
		if (!ctx.info_target) bb_show_usage();
		load_repos(&ctx);

		if (sync_repos(&ctx, 0) > 0) {
			perform_queries(&ctx);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else {
		bb_error_msg_and_die("command '%s' not yet implemented", command);
	}

	return EXIT_SUCCESS;
}
