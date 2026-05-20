/* vi: set sw=4 ts=4: */
/*
 * busybox_dnf - A lightweight package management frontend for BusyBox
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */

//applet:IF_DNF(APPLET(dnf, BB_DIR_USR_BIN, BB_SUID_DROP))

//usage:#define dnf_trivial_usage "[-yv] COMMAND [PACKAGE...]"
//usage:#define dnf_full_usage "\n\n"
//usage:       "High-level package management frontend\n"
//usage:     "\nOptions:"
//usage:     "\n\t-y, --assumeyes\t\tAnswer yes for all questions"
//usage:     "\n\t-v, --verbose\t\tVerbose output"
//usage:     "\n\nCommands:"
//usage:     "\n\tupdate\t\t\tUpdate list of available packages (metadata cache)"
//usage:     "\n\tinstall\t\t\tInstall new packages"
//usage:     "\n\tremove\t\t\tRemove packages"
//usage:     "\n\tupgrade\t\t\tUpgrade the system"
//usage:     "\n\treinstall\t\tReinstall packages (restores files)"
//usage:     "\n\tgroupinstall\t\tInstall all packages in a group"
//usage:     "\n\trescue-install\t\tInstall packages bypassing rpm (uses internal tar/cpio to /)"
//usage:     "\n\tverify\t\t\tVerify package sanity (check status, deps, and files)"
//usage:     "\n\tmd5check\t\tVerify package integrity (checks file hashes)"
//usage:     "\n\tcheck-update\t\tShow packages with available updates"
//usage:     "\n\tsearch\t\t\tSearch for a package"
//usage:     "\n\tinfo\t\t\tDisplay details about a package"

#include "libbb.h"
#include <sys/utsname.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* ANSI Colors */
#define CLR_GREEN  "\033[0;32m"
#define CLR_RED    "\033[0;31m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

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

typedef enum {
	MODE_INSTALL,
	MODE_UPGRADE,
	MODE_RESCUE,
	MODE_REINSTALL
} install_mode_t;

#define MAX_CANDIDATE_MIRRORS 5
#define URL_MAX_LEN 256
#define MAX_REPOS 16

typedef struct pkg_s {
	char *name;
	char *epoch;
	char *version;
	char *release;
	char *arch;
	char *summary;
	char *license;
	char *url;
	char *location;
	char *description;
	char *repo_id;
	char *depends;
	char *provides;
	int state;
	struct pkg_s *next;
} pkg_t;

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

typedef struct {
	char *releasever;
	char *basearch;
	char *search_term;
	char *info_target;
	int verbose;
	int assumeyes;
	repo_t repos[MAX_REPOS];
	int repo_count;
	int current_repo_idx;

	char master_url[URL_MAX_LEN];
	char target_mirror_url[URL_MAX_LEN];
	char primary_xml_url[URL_MAX_LEN];
	char primary_xml_hash_path[URL_MAX_LEN];
	char group_xml_url[URL_MAX_LEN];
	char group_xml_hash_path[URL_MAX_LEN];
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
				char *end;
				p += 8;
				end = strpbrk(p, " \t\n");
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
						char *end;
						v++;
						end = strchr(v, '"');
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
	const char *e1, *e2;
	long epoch1 = 0, epoch2 = 0;
	const char *u1, *u2;
	const char *r1, *r2;

	if (!v1 || !v2) return v1 ? 1 : (v2 ? -1 : 0);

	e1 = strchr(v1, ':');
	if (e1) {
		epoch1 = strtol(v1, NULL, 10);
		u1 = e1 + 1;
	} else {
		u1 = v1;
	}

	e2 = strchr(v2, ':');
	if (e2) {
		epoch2 = strtol(v2, NULL, 10);
		u2 = e2 + 1;
	} else {
		u2 = v2;
	}

	if (epoch1 != epoch2) return (epoch1 > epoch2) ? 1 : -1;

	r1 = strrchr(u1, '-');
	r2 = strrchr(u2, '-');

	if (r1 && r2) {
		char *up1 = xstrndup(u1, r1 - u1);
		char *up2 = xstrndup(u2, r2 - u2);
		int res = compare_version_part(up1, up2);
		free(up1); free(up2);
		if (res) return res;
		return compare_version_part(r1 + 1, r2 + 1);
	} else if (r1) {
		char *up1 = xstrndup(u1, r1 - u1);
		int res = compare_version_part(up1, u2);
		free(up1);
		if (res) return res;
		return compare_version_part(r1 + 1, "");
	} else if (r2) {
		char *up2 = xstrndup(u2, r2 - u2);
		int res = compare_version_part(u1, up2);
		free(up2);
		if (res) return res;
		return compare_version_part("", r2 + 1);
	}
	return compare_version_part(u1, u2);
}

static void load_repos(dnf_context_t *ctx)
{
	DIR *dir = opendir("/etc/yum.repos.d");
	struct dirent *entry;

	if (!dir) {
		printf("[WARN] Could not open /etc/yum.repos.d - directory missing?\n");
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		char *path;
		parser_t *p;
		char *tokens[2];
		repo_t *curr_repo = NULL;

		if (entry->d_name[0] == '.') continue;
		if (!strrstr(entry->d_name, ".repo")) continue;

		path = xasprintf("/etc/yum.repos.d/%s", entry->d_name);
		if (ctx->verbose) printf("[DEBUG] Parsing repo file: %s\n", path);
		p = config_open(path);
		free(path);
		if (!p) continue;

		while (config_read(p, tokens, 2, 0, "#= ", PARSE_NORMAL | PARSE_GREEDY)) {
			if (tokens[0][0] == '[') {
				char *id = xstrdup(tokens[0] + 1);
				char *end = strchr(id, ']');
				if (end) *end = '\0';

				if (ctx->repo_count < MAX_REPOS) {
					curr_repo = &ctx->repos[ctx->repo_count++];
					curr_repo->id = id;
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

	if (ctx->repo_count == 0) {
		printf("[INFO] No repository files (*.repo) found in /etc/yum.repos.d\n");
	}
}

static void parse_metalink_stream(dnf_context_t *ctx, const char *cache_path)
{
	FILE *fp = fopen_for_read(cache_path);
	char line[512];

	if (!fp) return;

	ctx->candidate_count = 0;

	while (fgets(line, sizeof(line), fp) && ctx->candidate_count < MAX_CANDIDATE_MIRRORS) {
		char *url_start = strstr(line, "<url ");
		if (url_start && strstr(line, "http")) {
			url_start = strchr(url_start, '>');
			if (url_start) {
				char *url_end;
				url_start++;
				url_end = strchr(url_start, '<');

				if (url_end && (url_end - url_start) < URL_MAX_LEN) {
					mirror_node_t *node = &ctx->candidates[ctx->candidate_count];
					int len = url_end - url_start;
					char *p;
					memcpy(node->url, url_start, len);
					node->url[len] = '\0';

					p = strstr(node->url, "/repodata/repomd.xml");
					if (p) *p = '\0';

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
	char line[1024];
	int in_primary = 0;
	int in_group = 0;

	if (!fp) return;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "<data type=\"primary\">")) {
			in_primary = 1;
			in_group = 0;
		} else if (strstr(line, "<data type=\"group\">")) {
			in_group = 1;
			in_primary = 0;
		}

		if (in_primary || in_group) {
			char *loc = strstr(line, "<location href=\"");
			if (loc) {
				char *end;
				char *target_url = in_primary ? ctx->primary_xml_url : ctx->group_xml_url;
				char *target_hash = in_primary ? ctx->primary_xml_hash_path : ctx->group_xml_hash_path;

				loc += 16;
				end = strchr(loc, '\"');
				if (end && (end - loc) < URL_MAX_LEN) {
					char *relative_url;
					char *bname;
					int tlen = strlen(ctx->target_mirror_url);
					while (tlen > 0 && ctx->target_mirror_url[tlen-1] == '/')
						tlen--;

					relative_url = xstrndup(loc, end - loc);
					while (relative_url[0] == '/')
						memmove(relative_url, relative_url + 1, strlen(relative_url));

					snprintf(target_url, URL_MAX_LEN, "%.*s/%s", tlen, ctx->target_mirror_url, relative_url);

					bname = strrchr(relative_url, '/');
					if (!bname) bname = relative_url; else bname++;
					snprintf(target_hash, URL_MAX_LEN, "/var/cache/dnf/%s_%s", ctx->repos[ctx->current_repo_idx].id, bname);

					if (ctx->verbose) {
						printf("[DEBUG] Found %s XML: %s\n", in_primary ? "Primary" : "Group", target_url);
						printf("[DEBUG] Hash-based Path: %s\n", target_hash);
					}
					free(relative_url);
				}
			}
		}
		if (strstr(line, "</data>")) {
			in_primary = 0;
			in_group = 0;
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
		int i;
		printf("  [Mirrors] Cached Candidates:\n");
		for (i = 0; i < ctx->candidate_count; i++) {
			printf("    %d. %s\n", i + 1, ctx->candidates[i].url);
		}
	}

	if (ctx->primary_xml_url[0])
		printf("  [Data]    Primary Metadata URL: %s\n", ctx->primary_xml_url);
	if (ctx->group_xml_url[0])
		printf("  [Data]    Group Metadata URL:   %s\n", ctx->group_xml_url);
	printf("----------------------------------------\n");
}

typedef void (*pkg_callback_t)(dnf_context_t *ctx, pkg_t *pkg, void *user_data);

static char *get_tag_value(const char *buf, const char *tag, char *dest, int max_len)
{
	char open_tag[64], close_tag[64];
	char *start, *end;
	int len;

	snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
	snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

	start = (char *)strstr(buf, open_tag);
	if (!start) return NULL;
	start += strlen(open_tag);

	end = strstr(start, close_tag);
	if (!end) return NULL;

	len = end - start;
	if (len >= max_len) len = max_len - 1;
	memcpy(dest, start, len);
	dest[len] = '\0';
	return dest;
}

static void parse_primary_stream(dnf_context_t *ctx, const char *cache_path, pkg_callback_t cb, void *user_data)
{
	char *cmd;
	FILE *fp;
	char *buf;
	int buf_cap = 128 * 1024;
	int buf_len = 0;

	cmd = xasprintf("unzstd -c %s 2>/dev/null || zcat %s 2>/dev/null || xzcat %s 2>/dev/null || cat %s",
					cache_path, cache_path, cache_path, cache_path);
	fp = popen(cmd, "r");
	free(cmd);

	if (!fp) return;

	buf = xmalloc(buf_cap);

	while (1) {
		int bytes_read;
		char *search_ptr;
		char *pkg_start;
		int consumed;

		bytes_read = fread(buf + buf_len, 1, buf_cap - buf_len - 1, fp);
		if (bytes_read <= 0) break;
		buf_len += bytes_read;
		buf[buf_len] = '\0';

		search_ptr = buf;

		while ((pkg_start = strstr(search_ptr, "<package type=\"rpm\">")) != NULL) {
			char *pkg_end = strstr(pkg_start, "</package>");
			int pkg_block_len;
			char *pkg_buf;
			char name[256];
			char arch[32];
			char summary[512];
			char epoch[16];
			char version[64];
			char release[64];
			char license[128];
			char url[256];
			char location[512];
			char description[2048];
			// Increased buffer size for dependencies
			char *depends = xzalloc(16384);
			char *loc_ptr;
			char *v_tag;
			char *req_start;
			pkg_t pkg;

			if (!pkg_end) break;
			pkg_end += 10;

			pkg_block_len = pkg_end - pkg_start;
			pkg_buf = xstrndup(pkg_start, pkg_block_len);

			memset(name, 0, sizeof(name));
			memset(arch, 0, sizeof(arch));
			memset(summary, 0, sizeof(summary));
			memset(epoch, 0, sizeof(epoch));
			safe_strncpy(epoch, "0", sizeof(epoch));
			memset(version, 0, sizeof(version));
			memset(release, 0, sizeof(release));
			memset(license, 0, sizeof(license));
			memset(url, 0, sizeof(url));
			memset(location, 0, sizeof(location));
			memset(description, 0, sizeof(description));

			get_tag_value(pkg_buf, "name", name, sizeof(name));
			get_tag_value(pkg_buf, "arch", arch, sizeof(arch));
			get_tag_value(pkg_buf, "summary", summary, sizeof(summary));
			get_tag_value(pkg_buf, "license", license, sizeof(license));
			get_tag_value(pkg_buf, "url", url, sizeof(url));
			get_tag_value(pkg_buf, "description", description, sizeof(description));

			loc_ptr = strstr(pkg_buf, "<location href=\"");
			if (loc_ptr) {
				char *loc_end;
				loc_ptr += 16;
				loc_end = strchr(loc_ptr, '\"');
				if (loc_end && (loc_end - loc_ptr) < (int)sizeof(location)) {
					int l_len = loc_end - loc_ptr;
					memcpy(location, loc_ptr, l_len);
					location[l_len] = '\0';
				}
			}

			v_tag = strstr(pkg_buf, "<version");
			if (v_tag) {
				char *e = strstr(v_tag, "epoch=\"");
				char *v = strstr(v_tag, "ver=\"");
				char *r = strstr(v_tag, "rel=\"");
				if (e) { sscanf(e, "epoch=\"%15[^\"]\"", epoch); }
				if (v) { sscanf(v, "ver=\"%63[^\"]\"", version); }
				if (r) { sscanf(r, "rel=\"%63[^\"]\"", release); }
			}

			req_start = strstr(pkg_buf, "<rpm:requires>");
			if (req_start) {
				char *req_end = strstr(req_start, "</rpm:requires>");
				char *entry = req_start;
				while (entry && (!req_end || entry < req_end)) {
					char dep_name[256];

					entry = strstr(entry, "<rpm:entry name=\"");
					if (!entry || (req_end && entry >= req_end)) break;
					entry += 17;

					memset(dep_name, 0, sizeof(dep_name));

					if (sscanf(entry, "%255[^\"]", dep_name) == 1) {
						int valid = 1;
						int d;
						int len = strlen(dep_name);

						if (len < 2 || len > 100) valid = 0;

						for (d = 0; dep_name[d]; d++) {
							if (dep_name[d] == ' ' || !((unsigned char)dep_name[d] >= 0x20 && (unsigned char)dep_name[d] <= 0x7E)) {
								valid = 0;
								break;
							}
						}

						// Enhanced filter: Exclude rpmlib and virtual config files
						if (valid && strncmp(dep_name, "rpmlib(", 7) != 0 && strncmp(dep_name, "config(", 7) != 0) {
							// Strip versioning and architecture from dependency name if present
							// e.g. "pkgname >= 1.2.3" -> "pkgname"
							// e.g. "pkgname(x86-64)" -> "pkgname"
							char *clean_dep = xstrdup(dep_name);
							char *p;

							// If it starts with /, it's a file dependency, keep it as is
							if (clean_dep[0] != '/') {
								p = strpbrk(clean_dep, " (><=");
								if (p) *p = '\0';
							}

							if (clean_dep[0]) {
								if (strlen(depends) + strlen(clean_dep) + 2 < 16384) {
									if (depends[0]) strcat(depends, " ");
									strcat(depends, clean_dep);
								}
							}
							free(clean_dep);
						} else if (valid && strncmp(dep_name, "config(", 7) == 0) {
							// For config(file), extract the file path
							char *clean_dep = xstrdup(dep_name + 7);
							char *p = strrchr(clean_dep, ')');
							if (p) *p = '\0';

							if (clean_dep[0]) {
								if (strlen(depends) + strlen(clean_dep) + 2 < 16384) {
									if (depends[0]) strcat(depends, " ");
									strcat(depends, clean_dep);
								}
							}
							free(clean_dep);
						}
					}
				}
			}

			char *provides = xzalloc(16384);
			char *prov_start = strstr(pkg_buf, "<rpm:provides>");
			if (prov_start) {
				char *prov_end = strstr(prov_start, "</rpm:provides>");
				char *entry = prov_start;
				while (entry && (!prov_end || entry < prov_end)) {
					char prov_name[256];

					entry = strstr(entry, "<rpm:entry name=\"");
					if (!entry || (prov_end && entry >= prov_end)) break;
					entry += 17;

					memset(prov_name, 0, sizeof(prov_name));

					if (sscanf(entry, "%255[^\"]", prov_name) == 1) {
						if (strlen(provides) + strlen(prov_name) + 2 < 16384) {
							if (provides[0]) strcat(provides, " ");
							strcat(provides, prov_name);
						}
					}
					/* Advance search pointer to avoid infinite loop on same entry */
					entry = strchr(entry, '>');
				}
			}

			/* Parse <file> tags which are also providers in RPM metadata */
			char *file_ptr = pkg_buf;
			while ((file_ptr = strstr(file_ptr, "<file")) != NULL) {
				char *file_data_start = strchr(file_ptr, '>');
				if (!file_data_start) break;
				file_data_start++;
				char *file_end = strstr(file_data_start, "</file>");
				if (file_end) {
					int f_len = file_end - file_data_start;
					if (f_len > 0 && f_len < 256) {
						char fname[256];
						memcpy(fname, file_data_start, f_len);
						fname[f_len] = '\0';
						if (strlen(provides) + f_len + 2 < 16384) {
							if (provides[0]) strcat(provides, " ");
							strcat(provides, fname);
						}
					}
					file_ptr = file_end + 7;
				} else {
					file_ptr++;
				}
			}

			memset(&pkg, 0, sizeof(pkg));
			pkg.name = name;
			pkg.epoch = epoch;
			pkg.version = version;
			pkg.release = release;
			pkg.arch = arch;
			pkg.summary = summary;
			pkg.license = license;
			pkg.url = url;
			pkg.location = location;
			pkg.description = trim(description);
			pkg.depends = depends;
			pkg.provides = provides;
			pkg.repo_id = ctx->repos[ctx->current_repo_idx].id;

			if (cb) cb(ctx, &pkg, user_data);

			free(pkg_buf);
			free(depends); // Free dynamic dependency buffer
			free(provides);
			search_ptr = pkg_end;
		}

		consumed = search_ptr - buf;
		if (consumed > 0) {
			memmove(buf, search_ptr, buf_len - consumed);
			buf_len -= consumed;
			buf[buf_len] = '\0';
		}

		if (buf_len >= buf_cap - 4096) {
			buf_cap *= 2;
			buf = xrealloc(buf, buf_cap);
		}
	}

	free(buf);
	pclose(fp);
}

static int str_contains_nocase(const char *haystack, const char *needle)
{
	char *hay_lower, *nee_lower;
	char *p;
	if (!haystack || !needle) return 0;
	
	hay_lower = xstrdup(haystack);
	nee_lower = xstrdup(needle);
	
	for (p = hay_lower; *p; p++) *p = tolower(*p);
	for (p = nee_lower; *p; p++) *p = tolower(*p);
	
	p = strstr(hay_lower, nee_lower);
	free(hay_lower); free(nee_lower);
	return (p != NULL);
}

static void query_cb(dnf_context_t *ctx, pkg_t *pkg, void *user_data)
{
	pkg_t *match = (pkg_t *)user_data;

	if (ctx->search_term) {
		if (str_contains_nocase(pkg->name, ctx->search_term) || 
			str_contains_nocase(pkg->summary, ctx->search_term)) {
			printf("%s.%s : %s\n", pkg->name, pkg->arch, pkg->summary);
		}
		return;
	}

	if (ctx->info_target && strcmp(pkg->name, ctx->info_target) == 0) {
		int replace = 0;
		if (!match->name) {
			replace = 1;
		} else {
			char *full_ver = xasprintf("%s:%s-%s", pkg->epoch, pkg->version, pkg->release);
			char *best_ver = xasprintf("%s:%s-%s", match->epoch, match->version, match->release);
			int cmp = compare_versions(full_ver, best_ver);
			if (cmp > 0) {
				replace = 1;
			} else if (cmp == 0) {
				/* If versions are equal, prefer the one matching our basearch */
				if (strcmp(pkg->arch, ctx->basearch) == 0 && strcmp(match->arch, ctx->basearch) != 0)
					replace = 1;
			}
			free(full_ver); free(best_ver);
		}

		if (replace) {
			free(match->name); match->name = xstrdup(pkg->name);
			free(match->epoch); match->epoch = xstrdup(pkg->epoch);
			free(match->version); match->version = xstrdup(pkg->version);
			free(match->release); match->release = xstrdup(pkg->release);
			free(match->arch); match->arch = xstrdup(pkg->arch);
			free(match->summary); match->summary = xstrdup(pkg->summary);
			free(match->license); match->license = xstrdup(pkg->license);
			free(match->url); match->url = xstrdup(pkg->url);
			free(match->location); match->location = xstrdup(pkg->location);
			free(match->description); match->description = xstrdup(pkg->description);
			free(match->depends); match->depends = xstrdup(pkg->depends);
			free(match->repo_id); match->repo_id = xstrdup(pkg->repo_id);
		}
	}
}

static void perform_queries(dnf_context_t *ctx)
{
	pkg_t best_match;
	int i;

	memset(&best_match, 0, sizeof(best_match));

	for (i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled) {
			char *primary_file = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[i].id);
			if (access(primary_file, R_OK) == 0) {
				ctx->current_repo_idx = i;
				parse_primary_stream(ctx, primary_file, query_cb, &best_match);
			}
			free(primary_file);
		}
	}

	if (ctx->info_target && best_match.name) {
		printf("\033[1mName          \033[0m: %s\n", best_match.name);
		printf("\033[1mVersion       \033[0m: %s\n", best_match.version);
		printf("\033[1mRelease       \033[0m: %s\n", best_match.release);
		printf("\033[1mArchitecture  \033[0m: %s\n", best_match.arch);
		printf("\033[1mLicense       \033[0m: %s\n", best_match.license);
		printf("\033[1mURL           \033[0m: %s\n", best_match.url);
		printf("\033[1mSummary       \033[0m: %s\n", best_match.summary);
		printf("\033[1mDescription   \033[0m: %s\n", best_match.description);
		if (best_match.depends && best_match.depends[0]) 
			printf("\033[1mDepends       \033[0m: %s\n", best_match.depends);
		printf("\033[1mRepository    \033[0m: %s\n", best_match.repo_id);
		printf("\n");
		
		free(best_match.name); free(best_match.epoch); free(best_match.version);
		free(best_match.release); free(best_match.arch); free(best_match.summary);
		free(best_match.license); free(best_match.url); free(best_match.location); 
		free(best_match.description); free(best_match.depends); free(best_match.repo_id);
	}
}

static pkg_t *installed_packages = NULL;
static int num_installed = 0;

static int cmp_pkg_name(const void *a, const void *b)
{
	const pkg_t *pa = (const pkg_t *)a;
	const pkg_t *pb = (const pkg_t *)b;
	return strcmp(pa->name, pb->name);
}

static void load_installed_packages(void)
{
	FILE *fp;
	char line[1024];
	int capacity = 0;

	if (installed_packages) return;

	fp = popen("rpm -qa --qf '%{NAME} %{EPOCH}:%{VERSION}-%{RELEASE} %{ARCH}\\n' 2>/dev/null", "r");
	if (!fp) return;

	while (fgets(line, sizeof(line), fp)) {
		char *saveptr;
		char *name = strtok_r(line, " \t", &saveptr);
		char *evr = strtok_r(NULL, " \t", &saveptr);
		char *arch = strtok_r(NULL, " \t\n", &saveptr);

		if (name && evr && arch) {
			pkg_t *p;
			char *colon;
			char *dash;

			if (num_installed >= capacity) {
				capacity = capacity ? capacity * 2 : 1024;
				installed_packages = xrealloc(installed_packages, capacity * sizeof(pkg_t));
			}
			
			p = &installed_packages[num_installed++];
			memset(p, 0, sizeof(pkg_t));
			p->name = xstrdup(name);
			p->arch = xstrdup(arch);

			colon = strchr(evr, ':');
			if (colon) {
				*colon = '\0';
				if (strcmp(evr, "(none)") == 0) p->epoch = xstrdup("0");
				else p->epoch = xstrdup(evr);
				evr = colon + 1;
			} else {
				p->epoch = xstrdup("0");
			}

			dash = strrchr(evr, '-');
			if (dash) {
				*dash = '\0';
				p->version = xstrdup(evr);
				p->release = xstrdup(dash + 1);
			} else {
				p->version = xstrdup(evr);
				p->release = xstrdup("");
			}
		}
	}
	pclose(fp);

	if (num_installed > 0) {
		qsort(installed_packages, num_installed, sizeof(pkg_t), cmp_pkg_name);
	}
}

static int is_package_installed(const char *name)
{
	pkg_t key;
	pkg_t *inst;
	
	if (name[0] == '/') return (access(name, F_OK) == 0);

	if (!installed_packages) load_installed_packages();
	if (num_installed == 0) return 0;
	
	key.name = (char *)name;
	inst = bsearch(&key, installed_packages, num_installed, sizeof(pkg_t), cmp_pkg_name);
	if (inst) return 1;

	/* If not found by name, it might be a capability (Provides) installed on the system */
	{
		char *cmd = xasprintf("rpm -q --whatprovides '%s' >/dev/null 2>&1", name);
		int rc = system(cmd);
		free(cmd);
		if (rc == 0) return 1;
	}

	return 0;
}

static int check_deps_sanity(dnf_context_t *ctx UNUSED_PARAM, pkg_t *p)
{
	int failed_count = 0;
	char *deps_copy, *dep_entry;

	if (!p->depends || !p->depends[0]) return 0;

	deps_copy = xstrdup(p->depends);
	dep_entry = strtok(deps_copy, " ");

	while (dep_entry) {
		if (!is_package_installed(dep_entry)) {
			if (failed_count == 0) printf("\n");
			printf("    %s[ERROR]%s Missing dependency: %s\n", CLR_RED, CLR_RESET, dep_entry);
			failed_count++;
		}
		dep_entry = strtok(NULL, " ");
	}
	free(deps_copy);
	return failed_count;
}

static int check_files_sanity(const char *pkg_name)
{
	int failed_count = 0;
	/* Sort the file list to reliably deduplicate shared files between architectures */
	char *cmd = xasprintf("rpm -ql %s | sort", pkg_name);
	FILE *f = popen(cmd, "r");
	char *line;
	char *last_line = NULL;

	if (!f) {
		free(cmd);
		return 1;
	}

	while ((line = xmalloc_fgetline(f)) != NULL) {
		struct stat st;

		if (last_line && strcmp(line, last_line) == 0) {
			free(line);
			continue;
		}
		free(last_line);
		last_line = xstrdup(line);

		if (lstat(line, &st) != 0) {
			if (failed_count == 0) printf("\n");
			printf("    %s[MISSING]%s %s\n", CLR_RED, CLR_RESET, line);
			failed_count++;
		} else if (S_ISLNK(st.st_mode)) {
			struct stat st2;
			if (stat(line, &st2) != 0) {
				if (failed_count == 0) printf("\n");
				printf("    %s[BROKEN LINK]%s %s\n", CLR_RED, CLR_RESET, line);
				failed_count++;
			}
		}
		free(line);
	}
	free(last_line);
	pclose(f);
	free(cmd);
	return failed_count;
}

typedef struct {
	pkg_t *installed;
	pkg_t repo_pkg;
} update_candidate_t;

static update_candidate_t *update_candidates = NULL;
static int num_updates = 0;

static void check_update_cb(dnf_context_t *ctx UNUSED_PARAM, pkg_t *pkg, void *user_data UNUSED_PARAM)
{
	pkg_t key;
	pkg_t *inst;

	key.name = pkg->name;
	inst = bsearch(&key, installed_packages, num_installed, sizeof(pkg_t), cmp_pkg_name);
	if (inst) {
		/* bsearch might return any arch of a multilib package.
		 * We must find the specific arch match or a compatible one. */
		while (inst > installed_packages && strcmp((inst-1)->name, pkg->name) == 0)
			inst--;

		while (inst < installed_packages + num_installed && strcmp(inst->name, pkg->name) == 0) {
			char *inst_ver;
			char *repo_ver;

			if (strcmp(inst->arch, pkg->arch) != 0 && strcmp(pkg->arch, "noarch") != 0 && strcmp(inst->arch, "noarch") != 0) {
				inst++;
				continue;
			}

			inst_ver = xasprintf("%s:%s-%s", inst->epoch, inst->version, inst->release);
			repo_ver = xasprintf("%s:%s-%s", pkg->epoch, pkg->version, pkg->release);

			/* Skip if exact same version string */
			if (strcmp(inst_ver, repo_ver) == 0) {
				free(inst_ver); free(repo_ver);
				inst++;
				continue;
			}

			if (compare_versions(repo_ver, inst_ver) > 0) {
				int i;
				update_candidate_t *cand = NULL;
				for (i = 0; i < num_updates; i++) {
					if (strcmp(update_candidates[i].installed->name, inst->name) == 0 &&
					    strcmp(update_candidates[i].repo_pkg.arch, pkg->arch) == 0) {
						cand = &update_candidates[i];
						break;
					}
				}

				if (cand) {
					char *cand_ver = xasprintf("%s:%s-%s", cand->repo_pkg.epoch, cand->repo_pkg.version, cand->repo_pkg.release);
					if (compare_versions(repo_ver, cand_ver) > 0) {
						free(cand->repo_pkg.epoch); cand->repo_pkg.epoch = xstrdup(pkg->epoch);
						free(cand->repo_pkg.version); cand->repo_pkg.version = xstrdup(pkg->version);
						free(cand->repo_pkg.release); cand->repo_pkg.release = xstrdup(pkg->release);
						free(cand->repo_pkg.repo_id); cand->repo_pkg.repo_id = xstrdup(pkg->repo_id);
					}
					free(cand_ver);
				} else {
					update_candidates = xrealloc(update_candidates, (num_updates + 1) * sizeof(update_candidate_t));
					cand = &update_candidates[num_updates++];
					cand->installed = inst;
					memset(&cand->repo_pkg, 0, sizeof(pkg_t));
					cand->repo_pkg.name = xstrdup(pkg->name);
					cand->repo_pkg.epoch = xstrdup(pkg->epoch);
					cand->repo_pkg.version = xstrdup(pkg->version);
					cand->repo_pkg.release = xstrdup(pkg->release);
					cand->repo_pkg.arch = xstrdup(pkg->arch);
					cand->repo_pkg.repo_id = xstrdup(pkg->repo_id);
				}
			}
			free(inst_ver);
			free(repo_ver);
			inst++;
		}
	}
}

static int cmp_update_candidate(const void *a, const void *b)
{
	const update_candidate_t *ca = (const update_candidate_t *)a;
	const update_candidate_t *cb = (const update_candidate_t *)b;
	return strcmp(ca->repo_pkg.name, cb->repo_pkg.name);
}

static void perform_check_update(dnf_context_t *ctx)
{
	int i;
	
	num_updates = 0;
	free(update_candidates);
	update_candidates = NULL;

	load_installed_packages();
	
	for (i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled) {
			char *primary_file = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[i].id);
			if (access(primary_file, R_OK) == 0) {
				ctx->current_repo_idx = i;
				parse_primary_stream(ctx, primary_file, check_update_cb, NULL);
			}
			free(primary_file);
		}
	}

	printf("Repositories loaded.\n");
	if (num_updates > 0) {
		qsort(update_candidates, num_updates, sizeof(update_candidate_t), cmp_update_candidate);
		printf("Upgrades (available for reinstall, available for upgrade)\n");
		for (i = 0; i < num_updates; i++) {
			update_candidate_t *c = &update_candidates[i];
			char evr[128];
			char name_arch[160];

			if (strcmp(c->repo_pkg.epoch, "0") == 0) {
				snprintf(evr, sizeof(evr), "%s-%s", c->repo_pkg.version, c->repo_pkg.release);
			} else {
				snprintf(evr, sizeof(evr), "%s:%s-%s", c->repo_pkg.epoch, c->repo_pkg.version, c->repo_pkg.release);
			}
			
			snprintf(name_arch, sizeof(name_arch), "%s.%s", c->repo_pkg.name, c->repo_pkg.arch);
			printf("%-34s %-25s %s\n", name_arch, evr, c->repo_pkg.repo_id);
		}
	}
}

static void perform_rescue_install(dnf_context_t *ctx, char **packages, int num_packages, install_mode_t mode);

static void perform_update(dnf_context_t *ctx)
{
	perform_check_update(ctx);

	if (num_updates > 0) {
		int i;
		char **pkgs = xmalloc(num_updates * sizeof(char *));
		
		for (i = 0; i < num_updates; i++) {
			pkgs[i] = xstrdup(update_candidates[i].repo_pkg.name);
		}
		
		printf("Upgrading %d packages...\n", num_updates);
		printf("Resolving dependencies...\n");
		perform_rescue_install(ctx, pkgs, num_updates, MODE_UPGRADE);
		
		for (i = 0; i < num_updates; i++) {
			free(pkgs[i]);
		}
		free(pkgs);
	} else {
		printf("Nothing to do.\n");
	}
	printf("Complete!\n");
}

static void sync_cache_file(const char *src_path, const char *dst_path)
{
	int src_fd = open(src_path, O_RDONLY);
	int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (src_fd >= 0 && dst_fd >= 0) {
		bb_copyfd_eof(src_fd, dst_fd);
	}
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) close(dst_fd);
}

static int run_dnf_update_cycle(dnf_context_t *ctx, int repo_idx)
{
	dnf_state_t state = STATE_INIT;
	char *cache_file = xasprintf("/var/cache/dnf/%s_metalink.xml", ctx->repos[repo_idx].id);
	char *repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[repo_idx].id);
	char *primary_link = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[repo_idx].id);
	char *group_link = xasprintf("/var/cache/dnf/%s_group.raw", ctx->repos[repo_idx].id);
	char *mirror_cache = xasprintf("/var/cache/dnf/%s_mirror.txt", ctx->repos[repo_idx].id);
	int success = 0;

	ctx->current_repo_idx = repo_idx;
	ctx->primary_xml_url[0] = '\0';
	ctx->group_xml_url[0] = '\0';
	ctx->target_mirror_url[0] = '\0';

	while (state != STATE_EXECUTE_PAYLOAD && state != STATE_ERROR) {
		dnf_debug_context(ctx, state);
		switch (state) {
		case STATE_INIT:
			if (ctx->repos[repo_idx].baseurl) {
				safe_strncpy(ctx->target_mirror_url, ctx->repos[repo_idx].baseurl, URL_MAX_LEN);
				state = STATE_FETCH_REPOMD;
			} else if (ctx->repos[repo_idx].metalink || ctx->repos[repo_idx].mirrorlist) {
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
				char *wget_cmd;
				int rc;
				mkdir("/var/cache/dnf", 0755);
				wget_cmd = xasprintf("wget -q -O %s \"%s\"", cache_file, ctx->master_url);
				rc = system(wget_cmd);
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
				FILE *fp = fopen_for_read(cache_file);
				if (fp) {
					char line[URL_MAX_LEN];
					ctx->candidate_count = 0;
					while (fgets(line, sizeof(line), fp) && ctx->candidate_count < MAX_CANDIDATE_MIRRORS) {
						char *p;
						if (line[0] == '#' || line[0] == '\n') continue;
						p = strpbrk(line, "\r\n");
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
			char *wget_cmd;
			int rc;

			if (ctx->verbose) printf("STATE: FETCH_REPOMD ... ");
			wget_cmd = xasprintf("wget -q -O %s \"%s\"", repomd_file, url);
			rc = system(wget_cmd);
			free(wget_cmd);
			free(url);

			if (rc == 0 || access(repomd_file, R_OK) == 0) {
				if (ctx->verbose) printf("PASS\n");
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
				if (access(ctx->primary_xml_hash_path, R_OK) == 0) {
					if (ctx->verbose) printf("[DEBUG] Primary metadata already fresh\n");
					unlink(primary_link);
					sync_cache_file(ctx->primary_xml_hash_path, primary_link);
				}
				if (ctx->group_xml_url[0] != '\0' && access(ctx->group_xml_hash_path, R_OK) == 0) {
					if (ctx->verbose) printf("[DEBUG] Group metadata already fresh\n");
					unlink(group_link);
					sync_cache_file(ctx->group_xml_hash_path, group_link);
				}
				state = STATE_FETCH_PRIMARY;
			} else {
				if (ctx->verbose) printf("FAIL\n");
				state = STATE_ERROR;
			}
			break;

		case STATE_FETCH_PRIMARY:
			if (ctx->verbose) printf("STATE: FETCH_METADATA ... ");
			{
				int p_ok = 1;
				if (access(primary_link, R_OK) != 0) {
					char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", ctx->primary_xml_hash_path, ctx->primary_xml_url);
					if (system(wget_cmd) == 0 || access(ctx->primary_xml_hash_path, R_OK) == 0) {
						unlink(primary_link);
						sync_cache_file(ctx->primary_xml_hash_path, primary_link);
					} else p_ok = 0;
					free(wget_cmd);
				}
				if (ctx->group_xml_url[0] != '\0' && access(group_link, R_OK) != 0) {
					char *wget_cmd = xasprintf("wget -q -O %s \"%s\"", ctx->group_xml_hash_path, ctx->group_xml_url);
					if (system(wget_cmd) == 0 || access(ctx->group_xml_hash_path, R_OK) == 0) {
						unlink(group_link);
						sync_cache_file(ctx->group_xml_hash_path, group_link);
					}
					free(wget_cmd);
				}

				if (p_ok) {
					if (ctx->verbose) printf("PASS\n");
					state = STATE_PARSE_PRIMARY;
				} else {
					if (ctx->verbose) printf("FAIL\n");
					state = STATE_ERROR;
				}
			}
			break;

		case STATE_PARSE_PRIMARY:
			if (ctx->verbose) printf("STATE: PARSE_PRIMARY ... ");
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
	free(primary_link);
	free(group_link);
	free(mirror_cache);
	return success;
}

static int sync_repos(dnf_context_t *ctx, int force)
{
	int synced = 0;
	int total_enabled = 0;
	int current = 0;
	int needed = 0;
	int i;

	for (i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && (ctx->repos[i].metalink || ctx->repos[i].baseurl || ctx->repos[i].mirrorlist)) {
			char *repomd_file;
			struct stat st;

			total_enabled++;
			repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[i].id);
			if (force || stat(repomd_file, &st) != 0 || (time(NULL) - st.st_mtime) > 3600 * 6) {
				needed++;
			}
			free(repomd_file);
		}
	}

	if (total_enabled == 0) return 0;

	if (needed > 0 && !ctx->verbose)
		printf("Updating and loading repositories:\n");

	for (i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled && (ctx->repos[i].metalink || ctx->repos[i].baseurl || ctx->repos[i].mirrorlist)) {
			char *repomd_file;
			struct stat st;
			int is_fresh;

			current++;
			repomd_file = xasprintf("/var/cache/dnf/%s_repomd.xml", ctx->repos[i].id);
			is_fresh = (stat(repomd_file, &st) == 0 && (time(NULL) - st.st_mtime) < 3600 * 6);

			if (!ctx->verbose) {
				printf(" (%d/%d): %-40s", current, total_enabled, ctx->repos[i].id);
				fflush(stdout);
			}

			if (force || !is_fresh) {
				free(repomd_file);
				if (run_dnf_update_cycle(ctx, i))
					synced++;
			} else {
				char *primary_link = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[i].id);
				char *group_link = xasprintf("/var/cache/dnf/%s_group.raw", ctx->repos[i].id);
				if (access(primary_link, R_OK) != 0 || access(group_link, R_OK) != 0) {
					char *mirror_cache;
					FILE *m_fp;

					ctx->current_repo_idx = i;
					ctx->target_mirror_url[0] = '\0';
					ctx->group_xml_hash_path[0] = '\0';

					mirror_cache = xasprintf("/var/cache/dnf/%s_mirror.txt", ctx->repos[i].id);
					m_fp = fopen(mirror_cache, "r");
					if (m_fp) {
						char line[URL_MAX_LEN];
						if (fgets(line, sizeof(line), m_fp)) {
							char *p = strpbrk(line, "\r\n");
							if (p) *p = '\0';
							safe_strncpy(ctx->target_mirror_url, line, URL_MAX_LEN);
						}
						fclose(m_fp);
					}
					free(mirror_cache);

					if (!ctx->target_mirror_url[0] && ctx->repos[i].baseurl) {
						safe_strncpy(ctx->target_mirror_url, ctx->repos[i].baseurl, URL_MAX_LEN);
					}
					
					parse_repomd_stream(ctx, repomd_file);
					if (ctx->primary_xml_hash_path[0] && access(ctx->primary_xml_hash_path, R_OK) == 0) {
						sync_cache_file(ctx->primary_xml_hash_path, primary_link);
					}
					if (ctx->group_xml_hash_path[0] && access(ctx->group_xml_hash_path, R_OK) == 0) {
						sync_cache_file(ctx->group_xml_hash_path, group_link);
					}
				}
				free(primary_link);
				free(group_link);
				free(repomd_file);

				if (!ctx->verbose) printf("[%sOK%s]\n", CLR_GREEN, CLR_RESET);
				synced++;
			}
		}
	}

	if (needed > 0 && !ctx->verbose && synced > 0)
		printf("Metadata cache created.\n");

	return synced;
}

typedef struct {
	pkg_t *items;
	int count;
} dep_list_t;

static void free_pkg_contents(pkg_t *pkg)
{
	free(pkg->name); free(pkg->epoch); free(pkg->version);
	free(pkg->release); free(pkg->arch); free(pkg->summary);
	free(pkg->license); free(pkg->url); free(pkg->location);
	free(pkg->description); free(pkg->depends); free(pkg->repo_id);
	memset(pkg, 0, sizeof(*pkg));
}

static void add_to_dep_list(dep_list_t *list, const char *name)
{
	int i;
	for (i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].name, name) == 0) return;
	}
	list->items = xrealloc(list->items, (list->count + 1) * sizeof(pkg_t));
	memset(&list->items[list->count], 0, sizeof(pkg_t));
	list->items[list->count].name = xstrdup(name);
	list->count++;
}

static void resolve_cb(dnf_context_t *ctx, pkg_t *pkg, void *user_data)
{
	dep_list_t *list = (dep_list_t *)user_data;
	int i;

	for (i = 0; i < list->count; i++) {
		int match_found = 0;
		if (strcmp(list->items[i].name, pkg->name) == 0) {
			match_found = 1;
		} else if (pkg->provides) {
			char *prov_copy = xstrdup(pkg->provides);
			char *saveptr_prov;
			char *p = strtok_r(prov_copy, " ", &saveptr_prov);
			while (p) {
				// Match base name or exact name
				// Many capabilities look like "name(arch)" or "name = version"
				if (strcmp(list->items[i].name, p) == 0) {
					match_found = 1;
					break;
				}
				// Check for config(path) or config(name)
				if (strncmp(p, "config(", 7) == 0) {
					char *clean_p = xstrdup(p + 7);
					char *end = strrchr(clean_p, ')');
					if (end) *end = '\0';
					if (strcmp(list->items[i].name, clean_p) == 0) {
						match_found = 1;
						free(clean_p);
						break;
					}
					free(clean_p);
				}
				// Also try matching without architecture suffix for cases like NetworkManager-l2tp(x86-64)
				char *paren = strchr(p, '(');
				if (paren) {
					*paren = '\0';
					if (strcmp(list->items[i].name, p) == 0) {
						match_found = 1;
						break;
					}
					*paren = '('; // restore
				}
				// Handle file provides like /usr/bin/sh
				if (p[0] == '/' && strcmp(list->items[i].name, p) == 0) {
					match_found = 1;
					break;
				}
				// Handle capabilities like desktop-notification-daemon
				if (strcmp(list->items[i].name, p) == 0) {
					match_found = 1;
					break;
				}
				p = strtok_r(NULL, " ", &saveptr_prov);
			}
			free(prov_copy);
		}

		if (match_found) {
			pkg_t *match = &list->items[i];
			int replace = 0;

			if (!match->location) {
				replace = 1;
			} else {
				char *full_ver = xasprintf("%s:%s-%s", pkg->epoch, pkg->version, pkg->release);
				char *best_ver = xasprintf("%s:%s-%s", match->epoch, match->version, match->release);
				int cmp = compare_versions(full_ver, best_ver);
				if (cmp > 0) replace = 1;
				else if (cmp == 0 && strcmp(pkg->arch, ctx->basearch) == 0 && strcmp(match->arch, ctx->basearch) != 0)
					replace = 1;
				free(full_ver); free(best_ver);
			}

			if (replace) {
				char *saved_name = match->name;
				// If the match was by capability/file, but we found a package,
				// update the name to the actual package name for clearer logging
				if (saved_name[0] == '/' || strstr(saved_name, ".so")) {
					free(saved_name);
					match->name = xstrdup(pkg->name);
				} else {
					match->name = saved_name;
				}

				free(match->epoch); match->epoch = xstrdup(pkg->epoch);
				free(match->version); match->version = xstrdup(pkg->version);
				free(match->release); match->release = xstrdup(pkg->release);
				free(match->arch); match->arch = xstrdup(pkg->arch);
				free(match->summary); match->summary = xstrdup(pkg->summary);
				free(match->license); match->license = xstrdup(pkg->license);
				free(match->url); match->url = xstrdup(pkg->url);
				free(match->location); match->location = xstrdup(pkg->location);
				free(match->description); match->description = xstrdup(pkg->description);
				free(match->depends); match->depends = xstrdup(pkg->depends);
				free(match->repo_id); match->repo_id = xstrdup(pkg->repo_id);

				if (match->depends) {
					char *deps = xstrdup(match->depends);
					char *saveptr_dep;
					char *p_dep = strtok_r(deps, " ", &saveptr_dep);
					while (p_dep) {
						if (!is_package_installed(p_dep)) {
							int old_count = list->count;
							add_to_dep_list(list, p_dep);
							if (list->count > old_count) {
								printf("%s ", p_dep);
								/* Print a newline every few packages to keep output manageable for copy-paste */
								if (list->count % 5 == 0) printf("\n");
								fflush(stdout);
							}
						}
						p_dep = strtok_r(NULL, " ", &saveptr_dep);
					}
					free(deps);
				}
			}
			/* Continue scanning, might find other packages in list */
		}
	}
}

static void resolve_all_dependencies(dnf_context_t *ctx, dep_list_t *list)
{
	int i, last_count = 0;
	int all_resolved = 0;

	while (!all_resolved) {
		all_resolved = 1;
		for (i = 0; i < list->count; i++) {
			if (!list->items[i].location) {
				all_resolved = 0;
				break;
			}
		}
		if (all_resolved) break;

		if (list->count == last_count) {
			/* No new names added, and some are still unresolved.
			 * We've tried all repos. Give up on those names. */
			break;
		}
		last_count = list->count;

		for (i = 0; i < ctx->repo_count; i++) {
			if (ctx->repos[i].enabled) {
				char *primary_file = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[i].id);
				if (access(primary_file, R_OK) == 0) {
					ctx->current_repo_idx = i;
					parse_primary_stream(ctx, primary_file, resolve_cb, list);
				}
				free(primary_file);
			}
		}
	}
}

static void get_package_info(dnf_context_t *ctx, const char *name, pkg_t *best_match)
{
	int i;
	memset(best_match, 0, sizeof(*best_match));
	ctx->info_target = (char *)name;
	ctx->search_term = NULL;

	for (i = 0; i < ctx->repo_count; i++) {
		if (ctx->repos[i].enabled) {
			char *primary_file = xasprintf("/var/cache/dnf/%s_primary.raw", ctx->repos[i].id);
			if (access(primary_file, R_OK) == 0) {
				ctx->current_repo_idx = i;
				parse_primary_stream(ctx, primary_file, query_cb, best_match);
			}
			free(primary_file);
		}
	}
	ctx->info_target = NULL;
}

static void perform_rescue_install(dnf_context_t *ctx, char **packages, int num_packages, install_mode_t mode)
{
	dep_list_t list = { NULL, 0 };
	int i;
	int needed = 0;
	char *batch_rpms = xstrdup("");

	for (i = 0; i < num_packages; i++) {
		if (mode == MODE_INSTALL && is_package_installed(packages[i])) {
			// Don't print "already installed" here if it's part of a large batch
			if (num_packages < 5) printf("Package %s is already installed.\n", packages[i]);
			continue;
		}
		needed++;
		// Add without printing immediately to avoid duplicates from group expansion
		{
			int found = 0;
			int j;
			for (j = 0; j < list.count; j++) {
				if (strcmp(list.items[j].name, packages[i]) == 0) { found = 1; break; }
			}
			if (!found) {
				list.items = xrealloc(list.items, (list.count + 1) * sizeof(pkg_t));
				memset(&list.items[list.count], 0, sizeof(pkg_t));
				list.items[list.count].name = xstrdup(packages[i]);
				list.count++;
			}
		}
	}

	if (needed > 0) {
		resolve_all_dependencies(ctx, &list);
		printf("\n");
	}

	if (list.count == 0) {
		if (needed > 0) printf("Nothing to do.\n");
		free(batch_rpms);
		return;
	}

	for (i = 0; i < list.count; i++) {
		if (!list.items[i].location) {
			printf("[ERROR] Package not found: %s\n", list.items[i].name);
			goto cleanup;
		}
	}
	
	printf("Dependencies resolved. Packages to install:\n\n");
	for (i = 0; i < list.count; i++) {
		printf("%s%s", i == 0 ? "" : " ", list.items[i].name);
	}
	printf("\n\n");

	for (i = 0; i < list.count; i++) {
		pkg_t *best_match = &list.items[i];
		if (best_match->name && best_match->location) {
			char *mirror = NULL;
			int j;
			for (j = 0; j < ctx->repo_count; j++) {
				if (strcmp(ctx->repos[j].id, best_match->repo_id) == 0) {
					char *mirror_cache = xasprintf("/var/cache/dnf/%s_mirror.txt", ctx->repos[j].id);
					FILE *fp = fopen(mirror_cache, "r");
					if (fp) {
						char line[URL_MAX_LEN];
						if (fgets(line, sizeof(line), fp)) {
							char *p = strpbrk(line, "\r\n");
							if (p) *p = '\0';
							mirror = xstrdup(line);
						}
						fclose(fp);
					}
					free(mirror_cache);
					if (!mirror && ctx->repos[j].baseurl) {
						mirror = xstrdup(ctx->repos[j].baseurl);
					}
					break;
				}
			}
			
			if (mirror) {
				char *rpm_url = xasprintf("%s/%s", mirror, best_match->location);
				char *bname = strrchr(best_match->location, '/');
				if (!bname) bname = best_match->location; else bname++;
				char *rpm_file = xasprintf("/var/cache/dnf/%s", bname);
				char *cmd;
				
				printf("Get:%d %s [%s]\n", i + 1, rpm_url, best_match->repo_id);
				cmd = xasprintf("wget -q -O %s \"%s\"", rpm_file, rpm_url);
				system(cmd);
				free(cmd);
				
				if (mode == MODE_RESCUE) {
					printf("Extracting %s (rescue install)...\n", best_match->name);
					cmd = xasprintf("rpm2cpio %s | cpio -idmuv --quiet", rpm_file);
					system(cmd);
					free(cmd);
				} else {
					// Deduplicate RPMs in the batch transaction
					if (!strstr(batch_rpms, rpm_file)) {
						char *tmp = xasprintf("%s %s", batch_rpms, rpm_file);
						free(batch_rpms);
						batch_rpms = tmp;
					}
				}
				
				free(rpm_url);
				free(rpm_file);
				free(mirror);
			}
		}
	}

	if (mode != MODE_RESCUE && batch_rpms[0] != '\0') {
		char *cmd;
		printf("Installing packages in a single transaction...\n");
		if (mode == MODE_REINSTALL) {
			cmd = xasprintf("rpm -Uvh --replacepkgs --nodeps --force %s", batch_rpms);
		} else {
			cmd = xasprintf("rpm -Uvh --nodeps --force %s", batch_rpms);
		}
		system(cmd);
		free(cmd);
	}

	/* Cleanup downloaded RPMs */
	for (i = 0; i < list.count; i++) {
		pkg_t *best_match = &list.items[i];
		if (best_match->location) {
			char *bname = strrchr(best_match->location, '/');
			if (!bname) bname = best_match->location; else bname++;
			char *rpm_file = xasprintf("/var/cache/dnf/%s", bname);
			unlink(rpm_file);
			free(rpm_file);
		}
	}
	
cleanup:
	free(batch_rpms);
	for (i = 0; i < list.count; i++) free_pkg_contents(&list.items[i]);
	free(list.items);
}

static void format_size(long long bytes, char *buf, int buf_size) {
	if (bytes < 1024) snprintf(buf, buf_size, "%lld B", bytes);
	else if (bytes < 1024 * 1024) snprintf(buf, buf_size, "%.1f KiB", bytes / 1024.0);
	else if (bytes < 1024 * 1024 * 1024) snprintf(buf, buf_size, "%.1f MiB", bytes / (1024.0 * 1024.0));
	else snprintf(buf, buf_size, "%.1f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
}

static void perform_remove(dnf_context_t *ctx, char **packages, int num_packages) {
	char *batch = xstrdup("");
	long long total_size = 0;
	int i, count = 0;
	char size_buf[32];
	char total_buf[32];

	printf("Package                                 Arch       Version                                Repository        Size\n");
	printf("Removing:\n");

	for (i = 0; i < num_packages; i++) {
		char cmd[512];
		FILE *fp;
		char line[1024];

		snprintf(cmd, sizeof(cmd), "rpm -q --qf '%%{NAME}|%%{ARCH}|%%{EPOCH}:%%{VERSION}-%%{RELEASE}|%%{SIZE}\\n' %s 2>/dev/null", packages[i]);
		fp = popen(cmd, "r");
		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				char *saveptr;
				char *name = strtok_r(line, "|", &saveptr);
				char *arch = strtok_r(NULL, "|", &saveptr);
				char *evr = strtok_r(NULL, "|", &saveptr);
				char *size_str = strtok_r(NULL, "\n", &saveptr);

				if (name && arch && evr && size_str) {
					char *tmp;
					long long size = atoll(size_str);
					total_size += size;
					format_size(size, size_buf, sizeof(size_buf));
					
					if (strncmp(evr, "(none):", 7) == 0) evr += 7;
					else if (strncmp(evr, "0:", 2) == 0) evr += 2;

					printf(" %-38s %-10s %-38s %-17s %s\n", name, arch, evr, "<unknown>", size_buf);

					tmp = xasprintf("%s %s", batch, name);
					free(batch);
					batch = tmp;
					count++;
				}
			}
			pclose(fp);
		}
	}

	if (count == 0) {
		printf("No match for argument: %s\n", packages[0]);
		printf("Nothing to do.\n");
		free(batch);
		return;
	}

	printf("\nTransaction Summary:\n");
	printf(" Removing:           %d package%s\n\n", count, count > 1 ? "s" : "");

	format_size(total_size, total_buf, sizeof(total_buf));
	printf("After this operation, %s will be freed (install 0 B, remove %s).\n", total_buf, total_buf);

	if (!ctx->assumeyes) {
		char answer[16];
		printf("Is this ok [y/N]: ");
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
			printf("Operation aborted.\n");
			free(batch);
			return;
		}
	}

	printf("Running transaction\n");
	printf("[1/2] Prepare transaction                                                       100%% \n");
	
	{
		char *exec_cmd = xasprintf("rpm -e --nodeps %s", batch);
		system(exec_cmd);
		free(exec_cmd);
	}

	printf("[2/2] Removing packages...                                                      100%% \n");
	printf("Complete!\n");

	free(batch);
}

static void parse_comps_stream(const char *cache_path, const char *group_target, dep_list_t *list)
{
	char *cmd;
	FILE *fp;
	char line[1024];
	int in_group = 0;
	int target_found = 0;
	int in_packagelist = 0;

	cmd = xasprintf("unzstd -c %s 2>/dev/null || zcat %s 2>/dev/null || xzcat %s 2>/dev/null || cat %s",
					cache_path, cache_path, cache_path, cache_path);
	fp = popen(cmd, "r");
	free(cmd);

	if (!fp) return;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "<group>")) {
			in_group = 1;
			target_found = 0;
		}
		if (in_group) {
			char val[256];
			if (get_tag_value(line, "id", val, sizeof(val)) || get_tag_value(line, "name", val, sizeof(val))) {
				if (strcmp(val, group_target) == 0) {
					target_found = 1;
				}
			}
			if (target_found && strstr(line, "<packagelist>")) {
				in_packagelist = 1;
			}
			if (in_packagelist) {
				if (strstr(line, "<packagereq")) {
					char *type = strstr(line, "type=\"");
					if (type) {
						type += 6;
						if (strncmp(type, "mandatory", 9) == 0 || strncmp(type, "default", 7) == 0) {
							char *p = strchr(line, '>');
							if (p) {
								char *end;
								p++;
								end = strchr(p, '<');
								if (end) {
									char *pkg_name = xstrndup(p, end - p);
									int old_count = list->count;
									add_to_dep_list(list, pkg_name);
									if (list->count > old_count) {
										printf("%s ", pkg_name);
										fflush(stdout);
									}
									free(pkg_name);
								}
							}
						}
					}
				}
				if (strstr(line, "</packagelist>")) {
					in_packagelist = 0;
					in_group = 0;
				}
			}
		}
		if (strstr(line, "</group>")) {
			in_group = 0;
			in_packagelist = 0;
		}
	}
	pclose(fp);
}

static void perform_group_install(dnf_context_t *ctx, char **groups, int num_groups, install_mode_t mode)
{
	dep_list_t pkgs = { NULL, 0 };
	int i, j;
	char **names;

	printf("Resolving dependencies...\n");
	for (i = 0; i < num_groups; i++) {
		for (j = 0; j < ctx->repo_count; j++) {
			if (ctx->repos[j].enabled) {
				char *group_file = xasprintf("/var/cache/dnf/%s_group.raw", ctx->repos[j].id);
				if (access(group_file, R_OK) == 0) {
					parse_comps_stream(group_file, groups[i], &pkgs);
				}
				free(group_file);
			}
		}
	}

	if (pkgs.count > 0) {
		names = xmalloc(pkgs.count * sizeof(char *));
		for (i = 0; i < pkgs.count; i++) names[i] = pkgs.items[i].name;
		perform_rescue_install(ctx, names, pkgs.count, mode);
		free(names);
		for (i = 0; i < pkgs.count; i++) free_pkg_contents(&pkgs.items[i]);
		free(pkgs.items);
	} else {
		printf("Warning: Group %s does not exist.\n", groups[0]);
	}
}

int dnf_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int dnf_main(int argc UNUSED_PARAM, char **argv)
{
	dnf_context_t ctx;
	unsigned opts;
	const char *command;

	memset(&ctx, 0, sizeof(ctx));

	opts = getopt32(argv, "yv");
	if (opts & 1) ctx.assumeyes = 1;
	if (opts & 2) ctx.verbose = 1;
	argv += optind;

	if (!argv[0])
		bb_show_usage();

	command = argv[0];

	if (strcmp(command, "update") == 0) {
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		if (sync_repos(&ctx, 1) > 0) {
			printf("Repository lists successfully updated.\n");
		} else {
			printf("No repositories updated.\n");
		}
	} else if (strcmp(command, "upgrade") == 0) {
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
		if (!argv[1]) bb_show_usage();
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.search_term = argv[1];
		ctx.info_target = NULL;
		load_repos(&ctx);

		if (sync_repos(&ctx, 0) > 0) {
			perform_queries(&ctx);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "info") == 0) {
		if (!argv[1]) bb_show_usage();
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		ctx.info_target = argv[1];
		ctx.search_term = NULL;
		load_repos(&ctx);

		if (sync_repos(&ctx, 0) > 0) {
			perform_queries(&ctx);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "install") == 0 || strcmp(command, "rescue-install") == 0 || strcmp(command, "reinstall") == 0) {
		int num_pkgs = 0;
		install_mode_t mode = MODE_INSTALL;

		if (!argv[1]) bb_show_usage();
		
		if (strcmp(command, "rescue-install") == 0) mode = MODE_RESCUE;
		else if (strcmp(command, "reinstall") == 0) mode = MODE_REINSTALL;

		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		if (sync_repos(&ctx, 0) > 0) {
			while (argv[num_pkgs + 1]) num_pkgs++;
			printf("Resolving dependencies...\n");
			perform_rescue_install(&ctx, argv + 1, num_pkgs, mode);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "groupinstall") == 0) {
		int num_groups = 0;
		if (!argv[1]) bb_show_usage();
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		if (sync_repos(&ctx, 0) > 0) {
			while (argv[num_groups + 1]) num_groups++;
			perform_group_install(&ctx, argv + 1, num_groups, MODE_INSTALL);
		} else {
			bb_error_msg_and_die("failed to initialize repository context");
		}
	} else if (strcmp(command, "remove") == 0) {
		int num_pkgs = 0;
		if (!argv[1]) bb_show_usage();
		while (argv[num_pkgs + 1]) num_pkgs++;
		
		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		perform_remove(&ctx, argv + 1, num_pkgs);
	} else if (strcmp(command, "verify") == 0) {
		int i;
		if (!argv[1]) bb_show_usage();

		ctx.releasever = get_releasever();
		ctx.basearch = get_basearch();
		load_repos(&ctx);
		sync_repos(&ctx, 0);
		load_installed_packages();

		for (i = 1; argv[i]; i++) {
			pkg_t best_match;
			int installed = is_package_installed(argv[i]);
			int total_failed = 0;

			printf("%sVerifying package:%s %s\n", CLR_BOLD, CLR_RESET, argv[i]);
			printf("  [INFO] Status:  %s%s%s\n",
				installed ? CLR_GREEN : CLR_RED,
				installed ? "Installed" : "NOT INSTALLED / BROKEN",
				CLR_RESET);

			get_package_info(&ctx, argv[i], &best_match);
			if (best_match.name) {
				printf("  [INFO] Version: %s-%s\n", best_match.version, best_match.release);
				if (installed) {
					int d_failed, f_failed;
					printf("  [CHECK] Dependencies... ");
					fflush(stdout);
					d_failed = check_deps_sanity(&ctx, &best_match);
					if (d_failed == 0) printf("%sOK%s\n", CLR_GREEN, CLR_RESET);
					else {
						printf("%sFAILED (%d missing)%s\n", CLR_RED, d_failed, CLR_RESET);
					}

					printf("  [CHECK] Filesystem... ");
					fflush(stdout);
					f_failed = check_files_sanity(argv[i]);
					if (f_failed == 0) printf("%sOK%s\n", CLR_GREEN, CLR_RESET);
					else {
						printf("%sFAILED (%d issues)%s\n", CLR_RED, f_failed, CLR_RESET);
					}

					total_failed = d_failed + f_failed;
				} else {
					total_failed = 1;
				}
				free(best_match.name); free(best_match.epoch); free(best_match.version);
				free(best_match.release); free(best_match.arch); free(best_match.summary);
				free(best_match.license); free(best_match.url); free(best_match.location);
				free(best_match.description); free(best_match.depends); free(best_match.repo_id);
			} else {
				printf("  [WARN] Package metadata not found in cache.\n");
				total_failed = 1;
			}

			printf("  [RESULT] Sanity check: %s%s%s\n",
				total_failed == 0 ? CLR_GREEN : CLR_RED,
				total_failed == 0 ? "PASSED" : "FAILED",
				CLR_RESET);
			printf("\n");
		}
	} else if (strcmp(command, "md5check") == 0) {
		int i;
		if (!argv[1]) bb_show_usage();

		for (i = 1; argv[i]; i++) {
			/* Use FILEDIGESTS (modern) and pipe delimiter for robust parsing */
			char *cmd = xasprintf("rpm -q --qf '[%%{FILENAMES}|%%{FILEDIGESTS}\\n]' %s 2>/dev/null", argv[i]);
			FILE *f = popen(cmd, "r");
			char *line;
			int checked = 0, failed = 0;

			if (!f) {
				bb_error_msg("no integrity data for %s", argv[i]);
				free(cmd);
				continue;
			}

			printf("Verifying %s...\n", argv[i]);

			while ((line = xmalloc_fgetline(f)) != NULL) {
				char *fname = strtok(line, "|");
				char *hash = strtok(NULL, "|");

				if (fname && hash && hash[0] != '(' && hash[0] != '\0') {
					const char *tool = "md5sum";
					int hlen = strlen(hash);
					char *cmd_check;

					/* Detect algorithm by hash length */
					if (hlen == 64) tool = "sha256sum";
					else if (hlen == 40) tool = "sha1sum";

					cmd_check = xasprintf("echo \"%s  %s\" | %s -c --status 2>/dev/null", hash, fname, tool);
					if (system(cmd_check) != 0) {
						printf("%s%s: FAILED%s\n", CLR_RED, fname, CLR_RESET);
						failed++;
					}
					checked++;
					free(cmd_check);
				}
				free(line);
			}
			pclose(f);
			free(cmd);

			if (checked == 0) {
				bb_error_msg("no files found for %s or package not installed", argv[i]);
			} else {
				if (failed == 0)
					printf("%sVerification successful:%s all %d files match hashes.\n", CLR_GREEN, CLR_RESET, checked);
				else
					printf("%sVerification failed:%s %d of %d files are corrupted.\n", CLR_RED, CLR_RESET, failed, checked);
			}
		}
	} else {
		bb_error_msg_and_die("command '%s' not yet implemented", command);
	}

	return EXIT_SUCCESS;
}
