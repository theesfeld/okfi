/* okfi — a terminal browser for Open Knowledge Format (OKF) bundles.
 *
 * Reads a bundle directory of markdown concept files, parses the flat
 * key:value frontmatter subset OKF uses, and presents a two-pane viewer.
 * See okf/okf-format.md for the parser contract and okf/tui-viewer.md for keys.
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- model ------------------------------------------------------------- */

typedef struct {
	char *key;
	char *val;
} KV;

typedef struct {
	char *relpath;  /* bundle-root relative, for display */
	int   reserved; /* index.md / log.md — not a concept, no type required */
	KV   *fm;       /* frontmatter, in source order, unknown keys preserved */
	int   nfm;
	char *type;     /* borrowed pointer into fm, or NULL */
	char *title;    /* borrowed pointer into fm, or NULL */
	char *body;     /* text after the frontmatter (or whole file) */
} Concept;

static Concept *concepts;
static int nconcepts, capconcepts;

/* ---- small string helpers --------------------------------------------- */

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	return s;
}

/* Display width approximated as UTF-8 codepoint count over a byte range
 * (ignores double-width).
 * ponytail: char-count not wcwidth; swap in wcwidth if CJK alignment matters. */
static int u8len_n(const char *s, int bytes) {
	int n = 0;
	for (int i = 0; i < bytes; i++)
		if (((unsigned char)s[i] & 0xC0) != 0x80)
			n++;
	return n;
}

static const char *base_name(const char *p) {
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

/* ---- frontmatter parsing ---------------------------------------------- */

static void add_kv(Concept *c, const char *k, const char *v) {
	c->fm = realloc(c->fm, sizeof(KV) * (c->nfm + 1));
	c->fm[c->nfm].key = strdup(k);
	c->fm[c->nfm].val = strdup(v);
	c->nfm++;
}

static int is_fence(const char *l) { return strcmp(l, "---") == 0; }

/* Parse one frontmatter value: strip wrapping quotes, and for an inline list
 * [a, b, c] strip the surrounding brackets so the comma list shows plainly. */
static char *normalize_scalar(char *v) {
	size_t n = strlen(v);
	if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
		v[n - 1] = '\0';
		v++;
		n -= 2;
	}
	if (n >= 2 && v[0] == '[' && v[n - 1] == ']') {
		v[n - 1] = '\0';
		v = trim(v + 1);
	}
	return v;
}

/* Split buf in place on '\n' (stripping a trailing '\r') into a malloc'd array
 * of line pointers into buf. Caller frees the array; the strings live in buf. */
static char **split_lines(char *buf, int *nlines) {
	char **lines = NULL;
	int n = 0;
	for (char *p = buf; p;) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		size_t ll = strlen(p);
		if (ll && p[ll - 1] == '\r')
			p[ll - 1] = '\0';
		lines = realloc(lines, sizeof(char *) * (n + 1));
		lines[n++] = p;
		p = nl ? nl + 1 : NULL;
	}
	*nlines = n;
	return lines;
}

/* parse_buffer takes a copy of `content`, so callers may pass literals. */
static void parse_buffer(Concept *c, const char *relpath, const char *content) {
	memset(c, 0, sizeof *c);
	c->relpath = strdup(relpath);
	c->reserved = strcmp(base_name(relpath), "index.md") == 0 ||
	              strcmp(base_name(relpath), "log.md") == 0;

	char *buf = strdup(content);
	int nlines = 0;
	char **lines = split_lines(buf, &nlines);

	int body_start = 0;
	if (nlines > 0 && is_fence(lines[0])) {
		int close = -1;
		for (int i = 1; i < nlines; i++)
			if (is_fence(lines[i])) {
				close = i;
				break;
			}
		if (close >= 0) {
			for (int i = 1; i < close; i++) {
				char *t = trim(lines[i]);
				if (*t == '\0' || *t == '#')
					continue;
				if (t[0] == '-' && (t[1] == ' ' || t[1] == '\0'))
					continue; /* block-list item, consumed by its key */
				char *colon = strchr(t, ':');
				if (!colon)
					continue;
				*colon = '\0';
				char *key = trim(t);
				char *val = trim(colon + 1);
				if (*val == '\0') {
					/* gather an indented block list into a comma string */
					char joined[1024];
					joined[0] = '\0';
					for (int j = i + 1; j < close; j++) {
						char *u = trim(lines[j]);
						if (u[0] != '-')
							break;
						char *item = trim(u + 1);
						if (joined[0])
							strncat(joined, ", ",
							        sizeof joined - strlen(joined) - 1);
						strncat(joined, item,
						        sizeof joined - strlen(joined) - 1);
					}
					add_kv(c, key, joined);
				} else {
					add_kv(c, key, normalize_scalar(val));
				}
			}
			body_start = close + 1;
		}
		/* unclosed fence: tolerate — treat the whole file as body */
	}

	for (int i = 0; i < c->nfm; i++) {
		if (!c->type && strcmp(c->fm[i].key, "type") == 0)
			c->type = c->fm[i].val;
		if (!c->title && strcmp(c->fm[i].key, "title") == 0)
			c->title = c->fm[i].val;
	}

	/* reassemble body from body_start with single '\n' separators */
	size_t blen = 1;
	for (int i = body_start; i < nlines; i++)
		blen += strlen(lines[i]) + 1;
	c->body = malloc(blen);
	c->body[0] = '\0';
	for (int i = body_start; i < nlines; i++) {
		strcat(c->body, lines[i]);
		if (i + 1 < nlines)
			strcat(c->body, "\n");
	}

	free(lines);
	free(buf);
}

/* ---- bundle collection ------------------------------------------------- */

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc(n + 1);
	size_t got = fread(buf, 1, n, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

static int ends_with_md(const char *s) {
	size_t n = strlen(s);
	return n > 3 && strcmp(s + n - 3, ".md") == 0;
}

static void collect(const char *dir, size_t root_len, int depth) {
	if (depth > 24) /* guard against symlink loops / pathological nesting */
		return;
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[4096];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		struct stat st;
		if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode))
			continue; /* skip symlinks outright — no loops, no escapes */
		if (S_ISDIR(st.st_mode)) {
			collect(path, root_len, depth + 1);
		} else if (S_ISREG(st.st_mode) && ends_with_md(e->d_name)) {
			char *content = read_file(path);
			if (!content)
				continue;
			const char *rel = path + root_len;
			while (*rel == '/')
				rel++;
			if (nconcepts == capconcepts) {
				capconcepts = capconcepts ? capconcepts * 2 : 16;
				concepts = realloc(concepts, sizeof(Concept) * capconcepts);
			}
			parse_buffer(&concepts[nconcepts++], rel, content);
			free(content);
		}
	}
	closedir(d);
}

static int cmp_concept(const void *a, const void *b) {
	return strcmp(((const Concept *)a)->relpath, ((const Concept *)b)->relpath);
}

static void free_concepts(void) {
	for (int i = 0; i < nconcepts; i++) {
		free(concepts[i].relpath);
		for (int k = 0; k < concepts[i].nfm; k++) {
			free(concepts[i].fm[k].key);
			free(concepts[i].fm[k].val);
		}
		free(concepts[i].fm);
		free(concepts[i].body);
	}
	free(concepts);
	concepts = NULL;
	nconcepts = capconcepts = 0;
}

static char bundle_root[1024]; /* path of the open bundle */

/* ---- tree view: concepts grouped by type for the left pane ------------- */

typedef struct {
	int header;      /* 1 = a group header row, 0 = a concept row */
	int cidx;        /* concept index for a concept row */
	int count;       /* concepts in the group (header rows) */
	int collapsed;   /* group is collapsed (header rows) */
	char label[160]; /* type name (header) or concept title (row) */
} VRow;
static VRow *vrows;
static int nvrows, capvrows;

/* Group fold state, persisted in the config and keyed by bundle path so a
 * collapsed group stays folded across runs (the single source of truth). */
typedef struct {
	char bundle[1024];
	char group[160];
} Fold;
static Fold folds[256];
static int nfolds;

/* group ordering: "type" (alpha), "count" (desc), or "priority" (cfg list) */
static char cfg_group_order[16] = "type";
static char cfg_priority[32][64];
static int n_priority;

static int has_fold(const char *bundle, const char *g) {
	for (int i = 0; i < nfolds; i++)
		if (strcmp(folds[i].bundle, bundle) == 0 && strcmp(folds[i].group, g) == 0)
			return i;
	return -1;
}
static void add_fold(const char *bundle, const char *g) {
	if (nfolds < 256 && has_fold(bundle, g) < 0) {
		snprintf(folds[nfolds].bundle, sizeof folds[0].bundle, "%s", bundle);
		snprintf(folds[nfolds].group, sizeof folds[0].group, "%s", g);
		nfolds++;
	}
}
static void remove_fold(const char *bundle, const char *g) {
	int i = has_fold(bundle, g);
	if (i >= 0) {
		memmove(&folds[i], &folds[i + 1], sizeof folds[0] * (nfolds - i - 1));
		nfolds--;
	}
}
static int is_collapsed(const char *g) { return has_fold(bundle_root, g) >= 0; }

static const char *concept_group(int i) {
	return concepts[i].reserved ? "reserved"
	       : (concepts[i].type && concepts[i].type[0]) ? concepts[i].type
	                                                    : "(untyped)";
}
static const char *concept_label(int i) {
	return concepts[i].title ? concepts[i].title : base_name(concepts[i].relpath);
}

static int priority_rank(const char *g) {
	for (int i = 0; i < n_priority; i++)
		if (strcmp(cfg_priority[i], g) == 0)
			return i;
	return n_priority + 1000; /* unlisted groups sort after listed ones */
}

/* should group (ga,count ca) sort before (gb,cb)? reserved always last */
static int group_less(const char *ga, int ca, const char *gb, int cb) {
	int ra = strcmp(ga, "reserved") == 0, rb = strcmp(gb, "reserved") == 0;
	if (ra != rb)
		return rb; /* the reserved one goes after */
	if (strcmp(cfg_group_order, "count") == 0) {
		if (ca != cb)
			return ca > cb; /* larger groups first */
		return strcmp(ga, gb) < 0;
	}
	if (strcmp(cfg_group_order, "priority") == 0) {
		int pa = priority_rank(ga), pb = priority_rank(gb);
		if (pa != pb)
			return pa < pb;
		return strcmp(ga, gb) < 0;
	}
	return strcmp(ga, gb) < 0; /* "type": alphabetical */
}

static int title_cmp(const void *pa, const void *pb) {
	return strcmp(concept_label(*(const int *)pa), concept_label(*(const int *)pb));
}

static VRow *push_vrow(int header, int cidx, const char *label) {
	if (nvrows == capvrows) {
		capvrows = capvrows ? capvrows * 2 : 32;
		vrows = realloc(vrows, sizeof(VRow) * capvrows);
	}
	VRow *v = &vrows[nvrows++];
	v->header = header;
	v->cidx = cidx;
	v->count = 0;
	v->collapsed = 0;
	snprintf(v->label, sizeof v->label, "%s", label);
	return v;
}

static void build_tree_view(void) {
	nvrows = 0;
	if (nconcepts == 0)
		return;
	/* distinct groups + counts — sized to nconcepts (its true upper bound), so
	 * no fixed cap can silently drop a concept whose type is the Nth distinct. */
	char(*groups)[160] = malloc(sizeof groups[0] * nconcepts);
	int *gcount = malloc(sizeof(int) * nconcepts);
	int *order = malloc(sizeof(int) * nconcepts);
	int ng = 0;
	for (int i = 0; i < nconcepts; i++) {
		const char *g = concept_group(i);
		int f = -1;
		for (int k = 0; k < ng; k++)
			if (strcmp(groups[k], g) == 0) {
				f = k;
				break;
			}
		if (f < 0) {
			snprintf(groups[ng], sizeof groups[0], "%s", g);
			gcount[ng] = 0;
			f = ng++;
		}
		gcount[f]++;
	}
	/* order the groups (selection sort; ng is tiny) */
	for (int k = 0; k < ng; k++)
		order[k] = k;
	for (int a = 0; a < ng; a++)
		for (int b = a + 1; b < ng; b++)
			if (group_less(groups[order[b]], gcount[order[b]], groups[order[a]],
			               gcount[order[a]])) {
				int t = order[a];
				order[a] = order[b];
				order[b] = t;
			}
	/* emit header + (when expanded) title-sorted concepts per group */
	for (int oi = 0; oi < ng; oi++) {
		const char *g = groups[order[oi]];
		int col = is_collapsed(g);
		VRow *h = push_vrow(1, -1, g);
		h->count = gcount[order[oi]];
		h->collapsed = col;
		if (col)
			continue;
		int *ids = malloc(sizeof(int) * gcount[order[oi]]);
		int n = 0;
		for (int i = 0; i < nconcepts; i++)
			if (strcmp(concept_group(i), g) == 0)
				ids[n++] = i;
		qsort(ids, n, sizeof(int), title_cmp);
		for (int m = 0; m < n; m++)
			push_vrow(0, ids[m], concept_label(ids[m]));
		free(ids);
	}
	free(groups);
	free(gcount);
	free(order);
}

static int vrow_of_header(const char *g) {
	for (int i = 0; i < nvrows; i++)
		if (vrows[i].header && strcmp(vrows[i].label, g) == 0)
			return i;
	return 0;
}

static int first_concept_row(void) {
	for (int i = 0; i < nvrows; i++)
		if (!vrows[i].header)
			return i;
	return 0;
}
static int vrow_of_concept(int cidx) {
	for (int i = 0; i < nvrows; i++)
		if (!vrows[i].header && vrows[i].cidx == cidx)
			return i;
	return first_concept_row();
}

static int load_bundle(const char *dir) {
	free_concepts();
	if (dir != bundle_root) /* guard the overlapping self-copy on reload */
		snprintf(bundle_root, sizeof bundle_root, "%s", dir);
	collect(bundle_root, strlen(bundle_root), 0);
	qsort(concepts, nconcepts, sizeof(Concept), cmp_concept);
	build_tree_view();
	return nconcepts;
}

/* ---- config + bundle discovery ----------------------------------------- */

enum { MAX_ROOTS = 32 };
static char cfg_roots[MAX_ROOTS][1024];
static int cfg_nroots;
static char cfg_theme[32] = "default";
static char cfg_editor[32] = "internal"; /* "internal" (in-TUI) or "system" ($EDITOR) */
static char cfg_extra[8192]; /* unknown config lines, preserved verbatim on save */
static char cfg_path[1024];

/* Per-role color overrides — every style is configurable. Order matches the
 * P_* pairs in init_styles (C_x maps to color pair C_x+1). fg/bg are 0-255
 * terminal color indices, or -1 for the terminal default; set==0 means "use the
 * theme's value". */
enum { C_HEAD, C_BOLD, C_ITAL, C_CODE, C_LINK, C_CODEBLK, C_KEY, C_BAR, C_TAG, C_NROLES };
static const char *color_role_name[C_NROLES] = {
    "head", "bold", "ital", "code", "link", "codeblk", "key", "bar", "tag"};
static struct {
	int fg, bg, set;
} cfg_color[C_NROLES];

/* parse "fg" or "fg,bg" (integers, or -1 / "default") into a role override */
static void cfg_set_color(int role, const char *val) {
	int fg = -1, bg = -1;
	char tmp[64];
	snprintf(tmp, sizeof tmp, "%s", val);
	char *comma = strchr(tmp, ',');
	if (comma)
		*comma = '\0';
	char *fs = trim(tmp);
	fg = (strcmp(fs, "default") == 0) ? -1 : atoi(fs);
	if (comma) {
		char *bs = trim(comma + 1);
		bg = (strcmp(bs, "default") == 0) ? -1 : atoi(bs);
	}
	cfg_color[role].fg = fg;
	cfg_color[role].bg = bg;
	cfg_color[role].set = 1;
}

typedef struct {
	char path[1024];
	char name[128];
} Bundle;
static Bundle *bundles;
static int nbundles, capbundles;

static void mkdir_p(const char *path) {
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static void cfg_add_root(const char *r) {
	if (cfg_nroots >= MAX_ROOTS || !*r)
		return;
	for (int i = 0; i < cfg_nroots; i++)
		if (strcmp(cfg_roots[i], r) == 0)
			return;
	snprintf(cfg_roots[cfg_nroots++], sizeof cfg_roots[0], "%s", r);
}

static void cfg_remove_root(int idx) {
	if (idx < 0 || idx >= cfg_nroots)
		return;
	for (int i = idx; i + 1 < cfg_nroots; i++)
		memcpy(cfg_roots[i], cfg_roots[i + 1], sizeof cfg_roots[0]);
	cfg_nroots--;
}

static void load_config(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(cfg_path, sizeof cfg_path, "%s/okfi/config", xdg);
	else
		snprintf(cfg_path, sizeof cfg_path, "%s/.config/okfi/config",
		         home ? home : ".");
	char *buf = read_file(cfg_path);
	if (!buf)
		return;
	cfg_extra[0] = '\0';
	for (char *p = buf; p && *p;) {
		char *nl = strchr(p, '\n');
		char line[1024];
		int ll = nl ? (int)(nl - p) : (int)strlen(p);
		if (ll >= (int)sizeof line)
			ll = sizeof line - 1;
		memcpy(line, p, ll);
		line[ll] = '\0';
		p = nl ? nl + 1 : NULL;
		char *t = line;
		while (*t == ' ' || *t == '\t')
			t++;
		if (*t == '\0' || *t == '#')
			continue;
		char *eq = strchr(t, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = trim(t), *val = trim(eq + 1);
		if (strcmp(key, "root") == 0) {
			cfg_add_root(val);
		} else if (strcmp(key, "theme") == 0) {
			snprintf(cfg_theme, sizeof cfg_theme, "%s", val);
		} else if (strcmp(key, "editor") == 0) {
			snprintf(cfg_editor, sizeof cfg_editor, "%s", val);
		} else if (strcmp(key, "group_order") == 0) {
			snprintf(cfg_group_order, sizeof cfg_group_order, "%s", val);
		} else if (strcmp(key, "group_priority") == 0) {
			n_priority = 0;
			char tmp[1024];
			snprintf(tmp, sizeof tmp, "%s", val);
			for (char *tok = strtok(tmp, ","); tok && n_priority < 32;
			     tok = strtok(NULL, ","))
				snprintf(cfg_priority[n_priority++], sizeof cfg_priority[0], "%s",
				         trim(tok));
		} else if (strcmp(key, "fold") == 0) {
			char *tab = strchr(val, '\t');
			if (tab) {
				*tab = '\0';
				add_fold(trim(val), trim(tab + 1));
			}
		} else if (strncmp(key, "color.", 6) == 0) {
			int role = -1;
			for (int r = 0; r < C_NROLES; r++)
				if (strcmp(key + 6, color_role_name[r]) == 0)
					role = r;
			if (role >= 0)
				cfg_set_color(role, val);
			else { /* unknown color role: preserve */
				size_t n = strlen(cfg_extra);
				snprintf(cfg_extra + n, sizeof cfg_extra - n, "%s = %s\n", key, val);
			}
		} else { /* preserve unknown keys (liberal consumer, like OKF frontmatter) */
			size_t n = strlen(cfg_extra);
			snprintf(cfg_extra + n, sizeof cfg_extra - n, "%s = %s\n", key, val);
		}
	}
	free(buf);
}

static void save_config(void) {
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", cfg_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	FILE *f = fopen(cfg_path, "w");
	if (!f)
		return;
	for (int i = 0; i < cfg_nroots; i++)
		fprintf(f, "root = %s\n", cfg_roots[i]);
	fprintf(f, "theme = %s\n", cfg_theme);
	fprintf(f, "editor = %s\n", cfg_editor);
	fprintf(f, "group_order = %s\n", cfg_group_order);
	if (n_priority) {
		fprintf(f, "group_priority = ");
		for (int i = 0; i < n_priority; i++)
			fprintf(f, "%s%s", i ? "," : "", cfg_priority[i]);
		fputc('\n', f);
	}
	for (int r = 0; r < C_NROLES; r++)
		if (cfg_color[r].set)
			fprintf(f, "color.%s = %d,%d\n", color_role_name[r], cfg_color[r].fg,
			        cfg_color[r].bg);
	for (int i = 0; i < nfolds; i++)
		fprintf(f, "fold = %s\t%s\n", folds[i].bundle, folds[i].group);
	if (cfg_extra[0])
		fputs(cfg_extra, f);
	fclose(f);
}

/* fold operations that persist immediately (defined after save_config) */
static void toggle_collapsed(const char *g) {
	if (has_fold(bundle_root, g) >= 0)
		remove_fold(bundle_root, g);
	else
		add_fold(bundle_root, g);
	save_config();
}
static void collapse_all(void) {
	for (int i = 0; i < nvrows; i++)
		if (vrows[i].header)
			add_fold(bundle_root, vrows[i].label);
	save_config();
}
static void expand_all(void) {
	for (int i = nfolds - 1; i >= 0; i--)
		if (strcmp(folds[i].bundle, bundle_root) == 0)
			remove_fold(bundle_root, folds[i].group);
	save_config();
}

static void seed_default_roots(void) {
	char cwd[1024];
	if (getcwd(cwd, sizeof cwd)) {
		char *s = strrchr(cwd, '/');
		if (s && s != cwd)
			*s = '\0';
		cfg_add_root(cwd);
	}
	const char *home = getenv("HOME");
	if (home) {
		char p[1024];
		snprintf(p, sizeof p, "%s/Projects", home);
		struct stat st;
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
			cfg_add_root(p);
	}
}

static int dir_has_md(const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.' && ends_with_md(e->d_name)) {
			found = 1;
			break;
		}
	closedir(d);
	return found;
}

/* A bundle root: an index.md carrying okf_version (the spec's canonical marker,
 * only the root index has it), or an `okf`-named dir holding concept .md files. */
static int is_bundle_root(const char *dir) {
	char idx[1024];
	snprintf(idx, sizeof idx, "%s/index.md", dir);
	FILE *f = fopen(idx, "rb"); /* frontmatter is at the top — read only a prefix */
	if (f) {
		char head[1024];
		size_t n = fread(head, 1, sizeof head - 1, f);
		head[n] = '\0';
		fclose(f);
		if (strstr(head, "okf_version"))
			return 1;
	}
	return strcmp(base_name(dir), "okf") == 0 && dir_has_md(dir);
}

static void add_bundle(const char *path) {
	if (nbundles == capbundles) {
		capbundles = capbundles ? capbundles * 2 : 16;
		bundles = realloc(bundles, sizeof(Bundle) * capbundles);
	}
	snprintf(bundles[nbundles].path, sizeof bundles[0].path, "%s", path);
	const char *b = base_name(path);
	char parent[1024];
	if (strcmp(b, "okf") == 0) { /* name the bundle after its project dir */
		snprintf(parent, sizeof parent, "%s", path);
		char *s = strrchr(parent, '/');
		if (s) {
			*s = '\0';
			b = base_name(parent);
		}
	}
	snprintf(bundles[nbundles].name, sizeof bundles[0].name, "%.*s",
	         (int)sizeof(bundles[0].name) - 1, b);
	nbundles++;
}

static int skip_scan_dir(const char *name) {
	return name[0] == '.' || strcmp(name, "node_modules") == 0 ||
	       strcmp(name, "target") == 0 || strcmp(name, "build") == 0 ||
	       strcmp(name, "dist") == 0;
}

static void discover(const char *dir, int depth) {
	if (depth > 5 || nbundles >= 4096)
		return;
	if (is_bundle_root(dir)) {
		add_bundle(dir); /* don't descend into a bundle's own subdirs */
		return;
	}
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (skip_scan_dir(e->d_name))
			continue;
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		discover(path, depth + 1);
	}
	closedir(d);
}

static int cmp_bundle(const void *a, const void *b) {
	return strcmp(((const Bundle *)a)->name, ((const Bundle *)b)->name);
}

static void discover_all(void) {
	nbundles = 0;
	for (int i = 0; i < cfg_nroots; i++)
		discover(cfg_roots[i], 0);
	qsort(bundles, nbundles, sizeof(Bundle), cmp_bundle);
}

/* cheap signature of the search-root set, to skip rescans when only theme/colors
 * changed in settings */
static unsigned roots_sig(void) {
	unsigned h = 5381;
	for (int i = 0; i < cfg_nroots; i++)
		for (const char *p = cfg_roots[i]; *p; p++)
			h = h * 33u + (unsigned char)*p;
	return h ^ (unsigned)cfg_nroots;
}

/* ---- styled wrapping --------------------------------------------------- */

/* A rendered line carries one attribute per byte (multibyte chars share the
 * attr of their lead byte), so markdown emphasis survives word-wrapping. */
typedef struct {
	char   *t;
	attr_t *a;
	int     len;
} WLine;

static WLine *wl;
static int nwl, capwl;

static void push_wl(const char *t, const attr_t *a, int len) {
	if (nwl == capwl) {
		capwl = capwl ? capwl * 2 : 64;
		wl = realloc(wl, sizeof(WLine) * capwl);
	}
	wl[nwl].t = malloc(len + 1);
	memcpy(wl[nwl].t, t, len);
	wl[nwl].t[len] = '\0';
	wl[nwl].a = malloc(sizeof(attr_t) * (len > 0 ? len : 1));
	if (a)
		memcpy(wl[nwl].a, a, sizeof(attr_t) * len);
	else
		for (int i = 0; i < len; i++)
			wl[nwl].a[i] = A_NORMAL;
	wl[nwl].len = len;
	nwl++;
}

static void free_wl(void) {
	for (int i = 0; i < nwl; i++) {
		free(wl[i].t);
		free(wl[i].a);
	}
	nwl = 0;
}

/* byte offset reached after advancing `cols` UTF-8 codepoints (NUL-bounded) */
static int u8_advance(const char *s, int cols) {
	int c = 0;
	const char *p = s;
	while (*p && c < cols) {
		p++;
		while (((unsigned char)*p & 0xC0) == 0x80)
			p++;
		c++;
	}
	return (int)(p - s);
}

/* word-wrap text[0..len) carrying its parallel attr array into WLines */
static void wrap_rich(const char *t, const attr_t *a, int len, int width) {
	if (width < 1)
		width = 1;
	if (len == 0) {
		push_wl("", NULL, 0);
		return;
	}
	char cur[8192];
	attr_t cura[8192];
	int cl = 0, ccols = 0, i = 0;
	while (i < len) {
		while (i < len && t[i] == ' ')
			i++;
		int ws = i;
		while (i < len && t[i] != ' ')
			i++;
		int wb = i - ws;
		if (wb == 0)
			break;
		int wcols = u8len_n(t + ws, wb);
		if (wcols > width) { /* hard-split an over-long word */
			if (cl) {
				push_wl(cur, cura, cl);
				cl = ccols = 0;
			}
			int o = ws;
			while (o < i) {
				int take = u8_advance(t + o, width);
				if (o + take > i)
					take = i - o;
				push_wl(t + o, a + o, take);
				o += take;
			}
			continue;
		}
		if (ccols && ccols + 1 + wcols > width) {
			push_wl(cur, cura, cl);
			cl = ccols = 0;
		}
		if (ccols) {
			cur[cl] = ' ';
			cura[cl] = A_NORMAL;
			cl++;
			ccols++;
		}
		for (int k = 0; k < wb && cl < (int)sizeof cur - 1; k++) {
			cur[cl] = t[ws + k];
			cura[cl] = a[ws + k];
			cl++;
		}
		ccols += wcols;
	}
	if (cl)
		push_wl(cur, cura, cl);
}

/* ---- markdown styling -------------------------------------------------- */

/* append up to `cap` bytes total into tb/ab — the cap is the caller's buffer size.
 * ab may be NULL when the caller only needs the text (e.g. measuring a width). */
static void put(char *tb, attr_t *ab, int *n, const char *s, int len, attr_t a,
                int cap) {
	for (int i = 0; i < len && *n < cap - 1; i++) {
		tb[*n] = s[i];
		if (ab)
			ab[*n] = a;
		(*n)++;
	}
}

/* append a pre-styled run (text + its own per-byte attrs) into tb/ab */
static void put_run(char *tb, attr_t *ab, int *n, const char *s, const attr_t *a,
                    int len, int cap) {
	for (int i = 0; i < len && *n < cap - 1; i++) {
		tb[*n] = s[i];
		ab[*n] = a[i];
		(*n)++;
	}
}

/* Semantic styles, resolved once from terminal capability + theme. */
static attr_t sty_head, sty_bold, sty_ital, sty_code, sty_link;
static attr_t sty_codeblk, sty_key, sty_bar, sty_tag;
static attr_t sty_logo[6];          /* cyan→magenta gradient rows */
static attr_t sty_frame, sty_framef; /* unfocused / focused pane border */

enum {
	P_HEAD = 1, P_BOLD, P_ITAL, P_CODE, P_LINK, P_CODEBLK, P_KEY, P_BAR, P_TAG,
	P_LOGO0, P_LOGO1, P_LOGO2, P_LOGO3, P_LOGO4, P_LOGO5, /* 10..15 */
	P_FRAME, P_FRAMEF                                     /* 16, 17 */
};

/* call after start_color()+use_default_colors(); color=0 (or theme "mono") = mono.
 * Themes: dark (default), light, bbs, mono. Per-role config overrides win. */
static void init_styles(int color) {
	if (!color || strcmp(cfg_theme, "mono") == 0) {
		sty_head = sty_bold = sty_key = A_BOLD;
		sty_ital = A_ITALIC;
		sty_code = A_REVERSE;
		sty_link = A_UNDERLINE;
		sty_codeblk = sty_tag = A_DIM;
		sty_bar = A_REVERSE;
		sty_frame = A_DIM;
		sty_framef = A_BOLD;
		for (int i = 0; i < 6; i++)
			sty_logo[i] = A_BOLD;
		return;
	}
	int fg[C_NROLES], bg[C_NROLES];
	int logo[6], framef, framefoc, hi256 = COLORS >= 256;
	int light = strcmp(cfg_theme, "light") == 0;
	int bbs = strcmp(cfg_theme, "bbs") == 0;

	if (hi256 && light) { /* dark inks for a light background */
		int f[] = {23, 130, 28, 124, 26, 240, 23, 231, 58};
		int b[] = {-1, -1, -1, -1, -1, -1, -1, 24, -1};
		int lg[] = {30, 31, 61, 97, 133, 162};
		memcpy(fg, f, sizeof fg); memcpy(bg, b, sizeof bg); memcpy(logo, lg, sizeof logo);
		framef = 244; framefoc = 23;
	} else if (hi256 && bbs) { /* vivid BBS, dark bg */
		int f[] = {51, 214, 213, 46, 45, 245, 51, 231, 121};
		int b[] = {-1, -1, -1, -1, -1, -1, -1, 21, -1};
		int lg[] = {51, 45, 69, 99, 171, 201};
		memcpy(fg, f, sizeof fg); memcpy(bg, b, sizeof bg); memcpy(logo, lg, sizeof logo);
		framef = 240; framefoc = 51;
	} else if (hi256) { /* dark theme (default) */
		int f[] = {81, 215, 114, 210, 75, 245, 81, 231, 108};
		int b[] = {-1, -1, -1, -1, -1, -1, -1, 24, -1};
		int lg[] = {51, 45, 69, 99, 171, 201};
		memcpy(fg, f, sizeof fg); memcpy(bg, b, sizeof bg); memcpy(logo, lg, sizeof logo);
		framef = 240; framefoc = 51;
	} else { /* 8/16-color fallback */
		int f[] = {COLOR_CYAN,  COLOR_YELLOW, COLOR_MAGENTA, COLOR_RED,  COLOR_BLUE,
		           COLOR_BLUE,  COLOR_CYAN,   COLOR_WHITE,   COLOR_GREEN};
		int b[] = {-1, -1, -1, -1, -1, -1, -1, COLOR_BLUE, -1};
		int lg[] = {COLOR_CYAN, COLOR_CYAN, COLOR_BLUE,
		            COLOR_MAGENTA, COLOR_MAGENTA, COLOR_MAGENTA};
		memcpy(fg, f, sizeof fg); memcpy(bg, b, sizeof bg); memcpy(logo, lg, sizeof logo);
		framef = COLOR_BLUE; framefoc = COLOR_CYAN;
	}
	for (int r = 0; r < C_NROLES; r++)
		if (cfg_color[r].set) { /* user override beats the theme */
			fg[r] = cfg_color[r].fg;
			bg[r] = cfg_color[r].bg;
		}
	for (int r = 0; r < C_NROLES; r++)
		init_pair(r + 1, fg[r], bg[r]);
	for (int i = 0; i < 6; i++)
		init_pair(P_LOGO0 + i, logo[i], -1);
	init_pair(P_FRAME, framef, -1);
	init_pair(P_FRAMEF, framefoc, -1);

	sty_head = COLOR_PAIR(P_HEAD) | A_BOLD;
	sty_bold = COLOR_PAIR(P_BOLD) | A_BOLD;
	sty_ital = COLOR_PAIR(P_ITAL) | A_ITALIC;
	sty_code = COLOR_PAIR(P_CODE);
	sty_link = COLOR_PAIR(P_LINK) | A_UNDERLINE;
	sty_codeblk = COLOR_PAIR(P_CODEBLK);
	sty_key = COLOR_PAIR(P_KEY) | A_BOLD;
	sty_bar = COLOR_PAIR(P_BAR) | A_BOLD;
	sty_tag = COLOR_PAIR(P_TAG);
	for (int i = 0; i < 6; i++)
		sty_logo[i] = COLOR_PAIR(P_LOGO0 + i) | A_BOLD;
	sty_frame = COLOR_PAIR(P_FRAME);
	sty_framef = COLOR_PAIR(P_FRAMEF) | A_BOLD;
}

/* CommonMark code span at s[i] (a backtick): an opening run of N backticks
 * closes on the next run of exactly N, so `` `x` `` embeds single backticks.
 * On success sets the content range [*cs, *cs+*cl) (one surrounding space
 * stripped per CommonMark when the content isn't all spaces) and *ni (index
 * past the closing run), and returns 1; returns 0 if no closing run exists. */
static int code_span(const char *s, int i, int L, int *cs, int *cl, int *ni) {
	int n = 0;
	while (i + n < L && s[i + n] == '`')
		n++;
	for (int j = i + n; j < L;) {
		if (s[j] != '`') {
			j++;
			continue;
		}
		int m = 0;
		while (j + m < L && s[j + m] == '`')
			m++;
		if (m != n) {
			j += m; /* a run of the wrong length can't close the span */
			continue;
		}
		int start = i + n, end = j, len = end - start;
		if (len >= 2 && s[start] == ' ' && s[end - 1] == ' ') {
			int allspace = 1;
			for (int k = start; k < end; k++)
				if (s[k] != ' ') {
					allspace = 0;
					break;
				}
			if (!allspace) {
				start++;
				end--;
			}
		}
		*cs = start;
		*cl = end - start;
		*ni = j + m;
		return 1;
	}
	return 0;
}

/* Recursion cap for nested inline spans — far beyond real markdown, a guard so a
 * pathologically nested line can't blow the stack. */
enum { INLINE_MAX_DEPTH = 16 };

/* Style s[0..L) onto tb/ab over `base`, recursing into bold/italic/link spans so
 * nested emphasis (e.g. **bold `code`**) layers attributes. Code spans are
 * literal (no recursion); unclosed delimiters emit literally. */
static void style_run(const char *s, int L, attr_t base, char *tb, attr_t *ab,
                      int *np, int cap, int depth) {
	for (int i = 0; i < L;) {
		char c = s[i];
		if (c == '\\' && i + 1 < L) {
			put(tb, ab, np, s + i + 1, 1, base, cap);
			i += 2;
			continue;
		}
		if (c == '`') {
			int cs, cl, ni;
			if (code_span(s, i, L, &cs, &cl, &ni)) {
				put(tb, ab, np, s + cs, cl, base | sty_code, cap);
				i = ni;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && c == '*' && i + 1 < L && s[i + 1] == '*') {
			int j = i + 2;
			while (j + 1 < L && !(s[j] == '*' && s[j + 1] == '*'))
				j++;
			if (j + 1 < L && s[j] == '*' && s[j + 1] == '*') {
				style_run(s + i + 2, j - (i + 2), base | sty_bold, tb, ab, np, cap,
				          depth + 1);
				i = j + 2;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && (c == '*' || c == '_')) {
			int j = i + 1;
			while (j < L && s[j] != c)
				j++;
			if (j < L && j > i + 1) {
				style_run(s + i + 1, j - (i + 1), base | sty_ital, tb, ab, np, cap,
				          depth + 1);
				i = j + 1;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && c == '[') {
			int rb = i + 1;
			while (rb < L && s[rb] != ']')
				rb++;
			if (rb + 1 < L && s[rb + 1] == '(') {
				int rp = rb + 2;
				while (rp < L && s[rp] != ')')
					rp++;
				if (rp < L) {
					style_run(s + i + 1, rb - (i + 1), base | sty_link, tb, ab, np,
					          cap, depth + 1);
					i = rp + 1;
					continue;
				}
			}
		}
		put(tb, ab, np, s + i, 1, base, cap);
		i++;
	}
}

static void style_inline(const char *s, attr_t base, char *tb, attr_t *ab,
                         int *np, int cap) {
	style_run(s, (int)strlen(s), base, tb, ab, np, cap, 0);
}

enum { RBUF = 8192 };

enum { AL_LEFT, AL_RIGHT, AL_CENTER };

/* alignment of a separator cell: :--: center, --: right, else left */
static int col_align(const char *cell) {
	int L = (int)strlen(cell);
	if (L == 0)
		return AL_LEFT;
	int l = cell[0] == ':', r = cell[L - 1] == ':';
	return (l && r) ? AL_CENTER : r ? AL_RIGHT : AL_LEFT;
}

/* A separator row like |---|:--:| : only -, :, |, space, with at least one of each */
static int is_table_sep(const char *s) {
	while (*s == ' ')
		s++;
	int dash = 0, bar = 0;
	for (const char *p = s; *p; p++) {
		if (*p == '|')
			bar = 1;
		else if (*p == '-')
			dash = 1;
		else if (*p != ':' && *p != ' ')
			return 0;
	}
	return dash && bar;
}

/* split a table row into trimmed cells (mutates row); drops the empty cell a
 * trailing pipe produces. Returns the cell count. */
static int split_cells(char *row, char **out, int maxc) {
	while (*row == ' ')
		row++;
	if (*row == '|')
		row++;
	int n = 0;
	char *start = row;
	for (char *p = row;; p++) {
		if (*p == '|' || *p == '\0') {
			int last = (*p == '\0');
			*p = '\0';
			if (n < maxc)
				out[n++] = trim(start);
			if (last)
				break;
			start = p + 1;
		}
	}
	if (n > 0 && out[n - 1][0] == '\0')
		n--;
	return n;
}

enum { LK_HEADING, LK_QUOTE, LK_BULLET, LK_PARA };

/* Classify a non-fence, non-table body line. Sets *level (heading depth, or a
 * bullet's indent) and *content (the line's text past any marker). Shared by
 * the TUI renderer and the PDF exporter so markdown handling never diverges. */
static int classify_line(const char *line, int *level, const char **content) {
	*level = 0; /* only HEADING/BULLET set a real depth; keep PARA/QUOTE defined */
	const char *h = line;
	while (*h == ' ')
		h++;
	if (*h == '#') {
		int k = 0;
		while (h[k] == '#')
			k++;
		if (h[k] == ' ' || h[k] == '\0') {
			*level = k;
			const char *b = h + k;
			while (*b == ' ')
				b++;
			*content = b;
			return LK_HEADING;
		}
	}
	if (h[0] == '>' && (h[1] == ' ' || h[1] == '\0')) {
		const char *b = h + 1;
		while (*b == ' ')
			b++;
		*content = b;
		return LK_QUOTE;
	}
	if ((h[0] == '-' || h[0] == '*' || h[0] == '+') && h[1] == ' ') {
		*level = (int)(h - line);
		*content = h + 2;
		return LK_BULLET;
	}
	*content = line;
	return LK_PARA;
}

/* If lines[i] begins a pipe table (a row followed by a separator), set *end to
 * the line past the block and return 1; else return 0. */
static int table_block(char **lines, int i, int nlines, int *end) {
	if (!(strchr(lines[i], '|') && i + 1 < nlines && is_table_sep(lines[i + 1])))
		return 0;
	int j = i + 2;
	while (j < nlines) {
		char *t = lines[j];
		while (*t == ' ')
			t++;
		if (*t == '\0' || !strchr(t, '|'))
			break;
		j++;
	}
	*end = j;
	return 1;
}

/* Does lines[j] begin a new block, so it cannot continue a paragraph?
 * Blank, fence, table, heading, bullet, and blockquote all start fresh. */
static int starts_block(char **lines, int j, int nlines) {
	char *t = lines[j];
	while (*t == ' ')
		t++;
	if (*t == '\0' || strncmp(t, "```", 3) == 0)
		return 1;
	int end;
	if (table_block(lines, j, nlines, &end))
		return 1;
	int level;
	const char *content;
	return classify_line(lines[j], &level, &content) != LK_PARA;
}

static void sb_add(char *out, size_t *n, size_t cap, const char *s) {
	while (*s && *n + 1 < cap)
		out[(*n)++] = *s++;
	out[*n] = '\0';
}

/* Read one logical block starting at lines[i] (caller has already handled
 * fences and tables). Joins lazy-continuation lines so inline markup that spans
 * a soft line break renders: a paragraph or bullet absorbs following plain
 * lines, a blockquote absorbs following `>` lines, a heading is one line. Sets
 * the kind and level, writes the joined content to out, returns the next index. */
static int read_block(char **lines, int i, int nlines, int *kind, int *level,
                      char *out, size_t cap) {
	const char *content;
	*kind = classify_line(lines[i], level, &content);
	size_t n = 0;
	out[0] = '\0';
	while (*content == ' ')
		content++;
	sb_add(out, &n, cap, content);

	int j = i + 1;
	if (*kind == LK_HEADING)
		return j;

	if (*kind == LK_QUOTE) {
		while (j < nlines) {
			int lv;
			const char *c2;
			if (classify_line(lines[j], &lv, &c2) != LK_QUOTE)
				break;
			if (n + 1 < cap)
				out[n++] = ' ';
			sb_add(out, &n, cap, c2);
			j++;
		}
		return j;
	}

	while (j < nlines && !starts_block(lines, j, nlines)) {
		const char *cc = lines[j];
		while (*cc == ' ')
			cc++;
		if (n + 1 < cap)
			out[n++] = ' ';
		sb_add(out, &n, cap, cc);
		j++;
	}
	return j;
}

/* Render a markdown pipe table (lines[start]=header, start+1=separator) as
 * aligned columns. Cells are inline-styled; header bold; column rule in box
 * drawing; column alignment honors the separator's :--, --:, :--: colons.
 * ponytail: rows wider than the pane are clipped by draw_wline — no h-scroll. */
static void render_table(char **lines, int start, int end) {
	char tb[RBUF], tmp[RBUF], sep[RBUF], ctb[2048];
	attr_t ab[RBUF], cab[2048];
	char *cells[64];
	int colw[64] = {0}, align[64] = {0}, ncols = 0;

	/* column alignment from the separator row */
	strncpy(sep, lines[start + 1], sizeof sep - 1);
	sep[sizeof sep - 1] = '\0';
	int snc = split_cells(sep, cells, 64);
	for (int cc = 0; cc < snc; cc++)
		align[cc] = col_align(cells[cc]);

	/* column widths from styled cell text (markup stripped) */
	for (int r = start; r < end; r++) {
		if (r == start + 1)
			continue;
		strncpy(tmp, lines[r], sizeof tmp - 1);
		tmp[sizeof tmp - 1] = '\0';
		int nc = split_cells(tmp, cells, 64);
		if (nc > ncols)
			ncols = nc;
		for (int cc = 0; cc < nc; cc++) {
			int cn = 0; /* measure only — NULL attr array */
			style_inline(cells[cc], A_NORMAL, ctb, NULL, &cn, sizeof ctb);
			int w = u8len_n(ctb, cn);
			if (w > colw[cc])
				colw[cc] = w;
		}
	}

	for (int r = start; r < end; r++) {
		int n = 0;
		if (r == start + 1) { /* "─┼─" rule under the header */
			for (int cc = 0; cc < ncols; cc++) {
				if (cc)
					put(tb, ab, &n, "\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80", 9,
					    sty_codeblk, RBUF);
				for (int k = 0; k < colw[cc]; k++)
					put(tb, ab, &n, "\xe2\x94\x80", 3, sty_codeblk, RBUF);
			}
			push_wl(tb, ab, n);
			continue;
		}
		strncpy(tmp, lines[r], sizeof tmp - 1);
		tmp[sizeof tmp - 1] = '\0';
		int nc = split_cells(tmp, cells, 64);
		attr_t cellbase = (r == start) ? sty_bold : A_NORMAL;
		for (int cc = 0; cc < ncols; cc++) {
			if (cc)
				put(tb, ab, &n, " \xe2\x94\x82 ", 5, A_NORMAL, RBUF); /* " │ " */
			int cn = 0;
			style_inline(cc < nc ? cells[cc] : "", cellbase, ctb, cab, &cn,
			             sizeof ctb);
			int w = u8len_n(ctb, cn), pad = colw[cc] - w;
			if (pad < 0)
				pad = 0;
			int lead = align[cc] == AL_RIGHT ? pad
			           : align[cc] == AL_CENTER ? pad / 2
			                                    : 0;
			for (int k = 0; k < lead && n < RBUF - 1; k++)
				put(tb, ab, &n, " ", 1, A_NORMAL, RBUF);
			put_run(tb, ab, &n, ctb, cab, cn, RBUF);
			for (int k = 0; k < pad - lead && n < RBUF - 1; k++)
				put(tb, ab, &n, " ", 1, A_NORMAL, RBUF);
		}
		push_wl(tb, ab, n);
	}
}

/* ---- cross-links ------------------------------------------------------- */

enum { MAX_LINKS = 9 }; /* one per digit key 1-9; raise with the key handler */

static struct {
	char text[80];
	int target;
} links[MAX_LINKS];
static int nlinks;

static void dirname_of(const char *path, char *buf, size_t cap) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		buf[0] = '\0';
		return;
	}
	size_t k = (size_t)(slash - path);
	if (k >= cap)
		k = cap - 1;
	memcpy(buf, path, k);
	buf[k] = '\0';
}

/* Resolve a markdown link URL to a concept index, or -1 if it names no concept.
 * Handles bundle-absolute (/x.md) and current-dir-relative (./x.md, x.md).
 * ponytail: '..' segments are not resolved — such a link stays unfollowable. */
static int resolve_link(const char *url, const char *from_rel) {
	char u[1024];
	strncpy(u, url, sizeof u - 1);
	u[sizeof u - 1] = '\0';
	char *hash = strchr(u, '#');
	if (hash)
		*hash = '\0';
	if (u[0] == '\0')
		return -1;

	char target[2048];
	if (u[0] == '/') {
		snprintf(target, sizeof target, "%s", u + 1);
	} else {
		const char *r = u;
		if (strncmp(r, "./", 2) == 0)
			r += 2;
		char dir[1024];
		dirname_of(from_rel, dir, sizeof dir);
		if (dir[0])
			snprintf(target, sizeof target, "%s/%s", dir, r);
		else
			snprintf(target, sizeof target, "%s", r);
	}
	for (int i = 0; i < nconcepts; i++)
		if (strcmp(concepts[i].relpath, target) == 0)
			return i;
	return -1;
}

/* Collect up to 9 followable links (resolving to a concept) from the body. */
static void collect_links(const Concept *c) {
	nlinks = 0;
	if (!c->body)
		return;
	for (const char *p = c->body; *p && nlinks < MAX_LINKS;) {
		if (*p == '[') {
			const char *rb = strchr(p, ']');
			if (rb && rb[1] == '(') {
				const char *rp = strchr(rb + 2, ')');
				if (rp) {
					char url[1024];
					int ul = (int)(rp - (rb + 2));
					if (ul > (int)sizeof url - 1)
						ul = sizeof url - 1;
					memcpy(url, rb + 2, ul);
					url[ul] = '\0';
					int idx = resolve_link(url, c->relpath);
					if (idx >= 0) {
						int tl = (int)(rb - (p + 1));
						if (tl > 79)
							tl = 79;
						memcpy(links[nlinks].text, p + 1, tl);
						links[nlinks].text[tl] = '\0';
						links[nlinks].target = idx;
						nlinks++;
					}
					p = rp + 1;
					continue;
				}
			}
		}
		p++;
	}
}

static void build_right(const Concept *c, int width) {
	free_wl();
	collect_links(c);
	char tb[RBUF], work[RBUF];
	attr_t ab[RBUF];

	for (int i = 0; i < c->nfm; i++) {
		int n = 0;
		put(tb, ab, &n, c->fm[i].key, (int)strlen(c->fm[i].key), sty_key, RBUF);
		put(tb, ab, &n, ": ", 2, A_NORMAL, RBUF);
		put(tb, ab, &n, c->fm[i].val, (int)strlen(c->fm[i].val), A_NORMAL, RBUF);
		wrap_rich(tb, ab, n, width);
	}
	if (c->nfm)
		push_wl("", NULL, 0);

	/* split body into a mutable line array so tables can look ahead */
	char *bodybuf = strdup(c->body ? c->body : "");
	int nlines = 0;
	char **lines = split_lines(bodybuf, &nlines);

	int in_code = 0;
	for (int i = 0; i < nlines;) {
		char *src = lines[i];
		char *tr = src;
		while (*tr == ' ')
			tr++;

		if (strncmp(tr, "```", 3) == 0) {
			in_code = !in_code; /* fence line itself is not shown */
			i++;
			continue;
		}
		if (in_code) {
			int n = 0;
			put(tb, ab, &n, src, (int)strlen(src), sty_codeblk, RBUF);
			wrap_rich(tb, ab, n, width);
			i++;
			continue;
		}
		if (*tr == '\0') { /* blank line → a spacer row */
			push_wl("", NULL, 0);
			i++;
			continue;
		}
		int end;
		if (table_block(lines, i, nlines, &end)) {
			render_table(lines, i, end);
			i = end;
			continue;
		}

		int level, kind;
		char block[RBUF];
		i = read_block(lines, i, nlines, &kind, &level, block, sizeof block);
		attr_t base = A_NORMAL;
		const char *body = block;
		if (kind == LK_HEADING) {
			base = sty_head;
		} else if (kind == LK_QUOTE) {
			base = sty_ital;
		} else if (kind == LK_BULLET) {
			int wn = 0; /* "<indent>• <content>" */
			for (int s = 0; s < level && wn < (int)sizeof work - 8; s++)
				work[wn++] = ' ';
			memcpy(work + wn, "\xe2\x80\xa2 ", 4); /* "• " */
			wn += 4;
			int rl = (int)strlen(block);
			if (rl > (int)sizeof work - wn - 1)
				rl = sizeof work - wn - 1;
			memcpy(work + wn, block, rl);
			work[wn + rl] = '\0';
			body = work;
		}
		int n = 0;
		style_inline(body, base, tb, ab, &n, RBUF);
		wrap_rich(tb, ab, n, width);
	}

	free(lines);
	free(bodybuf);
}

/* draw one styled line, switching ncurses attributes per byte run */
static void draw_wline(int y, int x, const WLine *w, int maxcols) {
	move(y, x);
	int col = 0, i = 0;
	while (i < w->len && col < maxcols) {
		int j = i + 1;
		while (j < w->len && ((unsigned char)w->t[j] & 0xC0) == 0x80)
			j++;
		attrset(w->a[i]);
		addnstr(w->t + i, j - i);
		col++;
		i = j;
	}
	attrset(A_NORMAL);
}

/* ---- ui ---------------------------------------------------------------- */

/* ---- shared ui chrome -------------------------------------------------- */

#define BX_TL "\xe2\x95\x94" /* ╔ */
#define BX_TR "\xe2\x95\x97" /* ╗ */
#define BX_BL "\xe2\x95\x9a" /* ╚ */
#define BX_BR "\xe2\x95\x9d" /* ╝ */
#define BX_H "\xe2\x95\x90"  /* ═ */
#define BX_V "\xe2\x95\x91"  /* ║ */

#define SL_TL "\xe2\x94\x8c" /* ┌ */
#define SL_TR "\xe2\x94\x90" /* ┐ */
#define SL_BL "\xe2\x94\x94" /* └ */
#define SL_BR "\xe2\x94\x98" /* ┘ */
#define SL_H "\xe2\x94\x80"  /* ─ */
#define SL_V "\xe2\x94\x82"  /* │ */
#define SL_TD "\xe2\x94\xac" /* ┬ */
#define SL_TU "\xe2\x94\xb4" /* ┴ */
#define SL_VR "\xe2\x94\x9c" /* ├ */
#define SL_VL "\xe2\x94\xa4" /* ┤ */
#define SL_X "\xe2\x94\xbc"  /* ┼ */

static int g_color; /* so settings can re-init styles live */

/* OKFI in FIGlet "ANSI Shadow" — 28 display columns wide, 6 rows. */
static const char *const OKFI_LOGO[6] = {
    " ██████╗ ██╗  ██╗███████╗██╗",
    "██╔═══██╗██║ ██╔╝██╔════╝██║",
    "██║   ██║█████╔╝ █████╗  ██║",
    "██║   ██║██╔═██╗ ██╔══╝  ██║",
    "╚██████╔╝██║  ██╗██║     ██║",
    " ╚═════╝ ╚═╝  ╚═╝╚═╝     ╚═╝",
};
#define LOGO_W 28
#define LOGO_H 6

/* draw the gradient logo centered, top row at y; returns rows consumed */
static int draw_logo(int y, int cols) {
	int x = (cols - LOGO_W) / 2;
	if (x < 0)
		x = 0;
	for (int i = 0; i < LOGO_H; i++) {
		attron(sty_logo[i]);
		mvaddstr(y + i, x, OKFI_LOGO[i]);
		attroff(sty_logo[i]);
	}
	const char *tag = "Open Knowledge Format Interface";
	int tx = (cols - (int)strlen(tag)) / 2;
	if (tx < 0)
		tx = 0;
	attron(sty_tag);
	mvprintw(y + LOGO_H, tx, "%s", tag);
	attroff(sty_tag);
	return LOGO_H + 1;
}

static void draw_box(int y, int x, int h, int w) {
	if (h < 2 || w < 2)
		return;
	mvaddstr(y, x, BX_TL);
	mvaddstr(y, x + w - 1, BX_TR);
	mvaddstr(y + h - 1, x, BX_BL);
	mvaddstr(y + h - 1, x + w - 1, BX_BR);
	for (int i = 1; i < w - 1; i++) {
		mvaddstr(y, x + i, BX_H);
		mvaddstr(y + h - 1, x + i, BX_H);
	}
	for (int r = 1; r < h - 1; r++) {
		mvaddstr(y + r, x, BX_V);
		mvaddstr(y + r, x + w - 1, BX_V);
	}
}

/* single-line box (matches the browser frame) with an optional title */
static void draw_sbox(int y, int x, int h, int w, const char *title) {
	if (h < 2 || w < 2)
		return;
	mvaddstr(y, x, SL_TL);
	mvaddstr(y, x + w - 1, SL_TR);
	mvaddstr(y + h - 1, x, SL_BL);
	mvaddstr(y + h - 1, x + w - 1, SL_BR);
	for (int i = 1; i < w - 1; i++) {
		mvaddstr(y, x + i, SL_H);
		mvaddstr(y + h - 1, x + i, SL_H);
	}
	for (int r = 1; r < h - 1; r++) {
		mvaddstr(y + r, x, SL_V);
		mvaddstr(y + r, x + w - 1, SL_V);
	}
	if (title)
		mvprintw(y, x + 2, " %s ", title);
}

/* full-width status/menu bar at row y. On the very last row we stop one column
 * short: writing the bottom-right cell scrolls the whole screen (eating the top
 * bar) on terminals where that triggers an auto-advance. */
static void bar_at(int y, const char *text) {
	char b[1024];
	snprintf(b, sizeof b, " %s", text);
	int w = COLS - 1; /* never touch the last column (auto-margin safe) */
	if (w < 0)
		w = 0;
	attron(sty_bar);
	mvprintw(y, 0, "%-*.*s", w, w, b);
	attroff(sty_bar);
}

/* a centered modal help/info box; any key dismisses */
static void run_help(const char *title, const char *const *body, int n) {
	int w = 56, h = n + 4;
	for (int i = 0; i < n; i++) {
		int l = (int)strlen(body[i]) + 4;
		if (l > w)
			w = l;
	}
	if (w > COLS - 2)
		w = COLS - 2;
	if (h > LINES - 2)
		h = LINES - 2;
	int y = (LINES - h) / 2, x = (COLS - w) / 2;
	if (y < 0)
		y = 0;
	if (x < 0)
		x = 0;
	attron(sty_bar);
	draw_box(y, x, h, w);
	mvprintw(y, x + 2, " %s ", title);
	attroff(sty_bar);
	for (int i = 0; i < n && i < h - 4; i++)
		mvprintw(y + 2 + i, x + 2, "%-*.*s", w - 4, w - 4, body[i]);
	refresh();
	getch();
}

/* ---- minibuffer + atomic writes ---------------------------------------- */

/* ---- codepoint helpers (shared by the prompt and the editor) ----------- */

static int u8_prev(const char *s, int col) {
	col--;
	while (col > 0 && ((unsigned char)s[col] & 0xC0) == 0x80)
		col--;
	return col;
}
static int u8_next(const char *s, int col, int len) {
	col++;
	while (col < len && ((unsigned char)s[col] & 0xC0) == 0x80)
		col++;
	return col;
}
static int u8_snap(const char *s, int col) {
	while (col > 0 && ((unsigned char)s[col] & 0xC0) == 0x80)
		col--;
	return col;
}

/* read a line at the bottom bar; returns 1 on Enter, 0 on Esc. UTF-8 aware:
 * typed multibyte input is assembled, backspace removes a whole codepoint. */
static int prompt_line(const char *label, char *out, size_t cap) {
	size_t len = strlen(out);
	curs_set(1);
	for (;;) {
		attron(sty_bar);
		mvprintw(LINES - 1, 0, "%-*.*s", COLS, COLS, "");
		mvprintw(LINES - 1, 0, " %s%s", label, out);
		attroff(sty_bar);
		int cx = 1 + (int)strlen(label) + u8len_n(out, (int)len);
		move(LINES - 1, cx < COLS ? cx : COLS - 1);
		int c = getch();
		if (c == '\n' || c == '\r') {
			curs_set(0);
			return 1;
		}
		if (c == 27) {
			curs_set(0);
			out[0] = '\0';
			return 0;
		}
		if (c == KEY_BACKSPACE || c == 127 || c == 8) {
			if (len > 0) {
				int pc = u8_prev(out, (int)len);
				out[pc] = '\0';
				len = pc;
			}
		} else if (c >= 32 && c < 256) { /* assemble one (maybe multibyte) char */
			unsigned char b = (unsigned char)c;
			char ch[8];
			int cn = 0;
			ch[cn++] = (char)b;
			int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
			for (int k = 0; k < more; k++) {
				int cc = getch();
				if (cc < 0)
					break;
				ch[cn++] = (char)cc;
			}
			if (len + cn + 1 < cap) {
				memcpy(out + len, ch, cn);
				len += cn;
				out[len] = '\0';
			}
		}
	}
}

/* one-keypress confirm shown on the bottom bar; true on y/Y */
static int confirm(const char *label) {
	attron(sty_bar);
	mvprintw(LINES - 1, 0, "%-*.*s", COLS, COLS, "");
	mvprintw(LINES - 1, 0, " %s", label);
	attroff(sty_bar);
	int c = getch();
	return c == 'y' || c == 'Y';
}

/* write via temp+rename so a crash/ENOSPC never truncates the real file */
static int write_atomic(const char *path, const char *data, size_t len) {
	char tmp[1200];
	snprintf(tmp, sizeof tmp, "%s.okfitmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	int err = (fwrite(data, 1, len, f) != len);
	if (fclose(f) != 0)
		err = 1;
	if (err || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void today_date(char *out, size_t cap) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, cap, "%Y-%m-%d", &tm);
}

/* prepend a dated, newest-first entry to a bundle's log.md (§7 structure) */
static void log_prepend(const char *bundle, const char *action, const char *desc) {
	char path[1024];
	snprintf(path, sizeof path, "%s/log.md", bundle);
	char date[16];
	today_date(date, sizeof date);
	char *old = read_file(path);
	const char *src = old ? old : "";
	char header[256] = "# Update Log";
	const char *rest = src;
	if (src[0] == '#' && src[1] == ' ') {
		const char *nl = strchr(src, '\n');
		int hl = nl ? (int)(nl - src) : (int)strlen(src);
		if (hl >= (int)sizeof header)
			hl = sizeof header - 1;
		memcpy(header, src, hl);
		header[hl] = '\0';
		rest = nl ? nl + 1 : "";
	}
	while (*rest == '\n')
		rest++;
	char todayhdr[32];
	snprintf(todayhdr, sizeof todayhdr, "## %s", date);
	char bullet[1024];
	snprintf(bullet, sizeof bullet, "* **%s**: %s\n", action, desc);
	char *buf = malloc(strlen(src) + strlen(bullet) + 320);
	size_t n = sprintf(buf, "%s\n\n", header);
	if (strncmp(rest, todayhdr, strlen(todayhdr)) == 0) {
		const char *nl = strchr(rest, '\n');
		int hl = nl ? (int)(nl - rest) + 1 : (int)strlen(rest);
		memcpy(buf + n, rest, hl);
		n += hl;
		memcpy(buf + n, bullet, strlen(bullet));
		n += strlen(bullet);
		strcpy(buf + n, nl ? nl + 1 : "");
		n += strlen(buf + n);
	} else {
		n += sprintf(buf + n, "%s\n%s\n", todayhdr, bullet);
		strcpy(buf + n, rest);
		n += strlen(rest);
	}
	write_atomic(path, buf, n);
	free(buf);
	free(old);
}

/* ---- create (scaffold) ------------------------------------------------- */

static int create_concept_file(const char *bundle, const char *name,
                               const char *type) {
	char rel[512];
	snprintf(rel, sizeof rel, "%s", name);
	if (!ends_with_md(rel))
		strncat(rel, ".md", sizeof rel - strlen(rel) - 1);
	const char *bn = base_name(rel);
	if (strcmp(bn, "index.md") == 0 || strcmp(bn, "log.md") == 0)
		return -1; /* reserved names are not concepts */
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", bundle, rel);
	struct stat st;
	if (stat(path, &st) == 0)
		return -1; /* never clobber */
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", path);
	char *s = strrchr(dir, '/');
	if (s) {
		*s = '\0';
		mkdir_p(dir);
	}
	char title[256];
	snprintf(title, sizeof title, "%s", bn);
	char *d = strrchr(title, '.');
	if (d && strcmp(d, ".md") == 0)
		*d = '\0';
	char date[16];
	today_date(date, sizeof date);
	char body[2048];
	int n = snprintf(body, sizeof body,
	                 "---\ntype: %s\ntitle: %s\ndescription: \ntags: []\n"
	                 "timestamp: %sT00:00:00Z\n---\n# %s\n\n",
	                 (type && *type) ? type : "Concept", title, date, title);
	return write_atomic(path, body, n);
}

static void new_bundle_at(const char *dir) {
	mkdir_p(dir);
	char date[16];
	today_date(date, sizeof date);
	struct stat st;
	char p[1024];
	snprintf(p, sizeof p, "%s/index.md", dir);
	if (stat(p, &st) != 0) {
		char b[1024];
		int n = snprintf(b, sizeof b,
		                 "---\nokf_version: \"0.1\"\n---\n# %s knowledge catalog\n\n",
		                 base_name(dir));
		write_atomic(p, b, n);
	}
	snprintf(p, sizeof p, "%s/log.md", dir);
	if (stat(p, &st) != 0) {
		char b[1024];
		int n = snprintf(b, sizeof b,
		                 "# Update Log\n\n## %s\n* **Creation**: Seeded bundle.\n",
		                 date);
		write_atomic(p, b, n);
	}
}

/* TUI: prompt for a new concept in the open bundle, then reload */
static void new_concept_ui(void) {
	char name[256] = "";
	if (!prompt_line("New concept path (e.g. tables/orders.md): ", name, sizeof name) ||
	    !*name)
		return;
	char type[128] = "";
	prompt_line("Type (e.g. Reference): ", type, sizeof type);
	if (create_concept_file(bundle_root, name, type) == 0) {
		char desc[300];
		snprintf(desc, sizeof desc, "Added `%s`.", name);
		log_prepend(bundle_root, "Creation", desc);
	}
	load_bundle(bundle_root); /* in-bundle reload — keep folds */
}

/* ---- in-TUI editor (codepoint-aware, atomic save) ---------------------- */

typedef struct {
	char *s;
	int len, cap;
} Eline;

static void eline_ensure(Eline *e, int need) {
	if (e->cap < need) {
		e->cap = need * 2 + 16;
		e->s = realloc(e->s, e->cap);
	}
}

static int run_editor(const char *path) {
	char *content = read_file(path);
	Eline *L = NULL;
	int nL = 0, capL = 0;
	for (const char *p = content ? content : "";;) {
		const char *nl = strchr(p, '\n');
		int ll = nl ? (int)(nl - p) : (int)strlen(p);
		if (nL == capL) {
			capL = capL ? capL * 2 : 32;
			L = realloc(L, sizeof(Eline) * capL);
		}
		L[nL].s = NULL;
		L[nL].len = ll;
		L[nL].cap = 0;
		eline_ensure(&L[nL], ll + 1);
		memcpy(L[nL].s, p, ll);
		L[nL].s[ll] = '\0';
		nL++;
		if (!nl)
			break;
		p = nl + 1;
	}
	/* a file ending in '\n' splits to a trailing empty segment; drop that
	 * artifact so an open→save round trip doesn't grow a blank line each time */
	if (nL > 1 && L[nL - 1].len == 0) {
		free(L[nL - 1].s);
		nL--;
	}
	free(content);

	int row = 0, col = 0, top = 0, modified = 0, saved = 0, running = 1;
	const char *errmsg = NULL;
	while (running) {
		curs_set(1);
		int rows = LINES, cols = COLS, vh = rows - 2;
		if (row < top)
			top = row;
		if (row >= top + vh)
			top = row - vh + 1;
		erase();
		char hd[1200];
		snprintf(hd, sizeof hd, "EDIT  %s", path);
		bar_at(0, hd);
		for (int i = 0; i < vh && top + i < nL; i++)
			mvaddnstr(1 + i, 0, L[top + i].s,
			          L[top + i].len > cols ? cols : L[top + i].len);
		char ft[256];
		if (errmsg)
			snprintf(ft, sizeof ft, "%s — buffer kept; retry ^O or ^X to abandon",
			         errmsg);
		else
			snprintf(ft, sizeof ft, "^O save  ^X/ESC cancel %s  ln %d/%d col %d",
			         modified ? "[modified]" : "", row + 1, nL, col + 1);
		bar_at(rows - 1, ft);
		int scol = u8len_n(L[row].s, col);
		move(1 + row - top, scol < cols ? scol : cols - 1);
		refresh();

		int c = getch();
		Eline *cur = &L[row];
		if (c == 15 || c == 19) { /* ^O / ^S: serialize and atomically save */
			size_t total = 1;
			for (int i = 0; i < nL; i++)
				total += L[i].len + 1;
			char *out = malloc(total);
			size_t n = 0;
			for (int i = 0; i < nL; i++) {
				memcpy(out + n, L[i].s, L[i].len);
				n += L[i].len;
				out[n++] = '\n';
			}
			saved = (write_atomic(path, out, n) == 0);
			free(out);
			if (saved)
				running = 0; /* only leave once the bytes are safely on disk */
			else
				errmsg = "SAVE FAILED (disk full / permissions?)";
		} else if (c == 27 || c == 24) { /* ESC / ^X: cancel */
			if (!modified || confirm("Discard changes? (y/N)"))
				running = 0;
		} else if (c == KEY_LEFT) {
			if (col > 0)
				col = u8_prev(cur->s, col);
			else if (row > 0)
				col = L[--row].len;
		} else if (c == KEY_RIGHT) {
			if (col < cur->len)
				col = u8_next(cur->s, col, cur->len);
			else if (row < nL - 1)
				row++, col = 0;
		} else if (c == KEY_UP) {
			if (row > 0) {
				row--;
				if (col > L[row].len)
					col = L[row].len;
				col = u8_snap(L[row].s, col);
			}
		} else if (c == KEY_DOWN) {
			if (row < nL - 1) {
				row++;
				if (col > L[row].len)
					col = L[row].len;
				col = u8_snap(L[row].s, col);
			}
		} else if (c == KEY_HOME) {
			col = 0;
		} else if (c == KEY_END) {
			col = cur->len;
		} else if (c == KEY_BACKSPACE || c == 127 || c == 8) {
			if (col > 0) {
				int pc = u8_prev(cur->s, col), w = col - pc;
				memmove(cur->s + pc, cur->s + col, cur->len - col + 1);
				cur->len -= w;
				col = pc;
				modified = 1;
			} else if (row > 0) { /* join with previous line */
				Eline *pl = &L[row - 1];
				int at = pl->len;
				eline_ensure(pl, pl->len + cur->len + 1);
				memcpy(pl->s + pl->len, cur->s, cur->len + 1);
				pl->len += cur->len;
				free(cur->s);
				memmove(&L[row], &L[row + 1], sizeof(Eline) * (nL - row - 1));
				nL--;
				row--;
				col = at;
				modified = 1;
			}
		} else if (c == KEY_DC) {
			if (col < cur->len) {
				int nc = u8_next(cur->s, col, cur->len), w = nc - col;
				memmove(cur->s + col, cur->s + nc, cur->len - nc + 1);
				cur->len -= w;
				modified = 1;
			} else if (row < nL - 1) {
				Eline *nx = &L[row + 1];
				eline_ensure(cur, cur->len + nx->len + 1);
				memcpy(cur->s + cur->len, nx->s, nx->len + 1);
				cur->len += nx->len;
				free(nx->s);
				memmove(&L[row + 1], &L[row + 2], sizeof(Eline) * (nL - row - 2));
				nL--;
				modified = 1;
			}
		} else if (c == '\n' || c == '\r') {
			if (nL == capL) {
				capL *= 2;
				L = realloc(L, sizeof(Eline) * capL);
				cur = &L[row];
			}
			memmove(&L[row + 2], &L[row + 1], sizeof(Eline) * (nL - row - 1));
			nL++;
			Eline *nw = &L[row + 1];
			nw->s = NULL;
			nw->cap = 0;
			int tail = cur->len - col;
			nw->len = tail;
			eline_ensure(nw, tail + 1);
			memcpy(nw->s, cur->s + col, tail);
			nw->s[tail] = '\0';
			cur->len = col;
			cur->s[col] = '\0';
			row++;
			col = 0;
			modified = 1;
		} else if (c >= 32 && c < 256) { /* insert one (maybe multibyte) codepoint */
			unsigned char b = (unsigned char)c;
			char ch[8];
			int cn = 0;
			ch[cn++] = (char)b;
			int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
			for (int k = 0; k < more; k++) {
				int cc = getch();
				if (cc < 0)
					break;
				ch[cn++] = (char)cc;
			}
			eline_ensure(cur, cur->len + cn + 1);
			memmove(cur->s + col + cn, cur->s + col, cur->len - col + 1);
			memcpy(cur->s + col, ch, cn);
			cur->len += cn;
			col += cn;
			modified = 1;
		}
	}
	curs_set(0);
	for (int i = 0; i < nL; i++)
		free(L[i].s);
	free(L);
	return saved;
}

/* Suspend curses and open the file in $VISUAL/$EDITOR (default vi). The editor
 * command is split on spaces (so "emacs -nw" works); the path is a separate argv
 * element, so no shell and no quoting/injection. */
static void launch_external_editor(const char *path) {
	const char *ed = getenv("VISUAL");
	if (!ed || !*ed)
		ed = getenv("EDITOR");
	if (!ed || !*ed)
		ed = "vi";
	char cmd[256];
	snprintf(cmd, sizeof cmd, "%s", ed);
	char *argv[16];
	int ac = 0;
	for (char *t = strtok(cmd, " "); t && ac < 14; t = strtok(NULL, " "))
		argv[ac++] = t;
	if (ac == 0)
		return;
	argv[ac++] = (char *)path;
	argv[ac] = NULL;

	endwin(); /* hand the terminal to the editor */
	pid_t pid = fork();
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	} else if (pid > 0) {
		int st;
		waitpid(pid, &st, 0);
	}
	/* resume curses; the editor may have re-enabled flow control */
	reset_prog_mode();
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) == 0) {
		t.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	clearok(stdscr, TRUE);
	refresh();
}

/* ---- settings ---------------------------------------------------------- */

static void run_settings(void) {
	int sel = 0;
	for (;;) {
		int nroots = cfg_nroots;
		int ROOT0 = 3;              /* first root index (after theme/editor/order) */
		int ADD = ROOT0 + nroots;   /* "[ add search root ]" index */
		int COL0 = ADD + 1;         /* first color role index */
		int n_items = COL0 + C_NROLES;
		if (sel >= n_items)
			sel = n_items - 1;
		if (sel < 0)
			sel = 0;
		erase();
		bar_at(0, "okfi  SETTINGS   Enter:change  d:delete root  Esc:save & back");
		int y = 2;
		for (int it = 0; it < n_items; it++) {
			char line[1024];
			if (it == 0)
				snprintf(line, sizeof line, "theme            : %s", cfg_theme);
			else if (it == 1)
				snprintf(line, sizeof line, "editor           : %s", cfg_editor);
			else if (it == 2)
				snprintf(line, sizeof line, "group order      : %s", cfg_group_order);
			else if (it < ADD)
				snprintf(line, sizeof line, "root             : %s",
				         cfg_roots[it - ROOT0]);
			else if (it == ADD)
				snprintf(line, sizeof line, "[ add search root ]");
			else {
				int r = it - COL0;
				if (cfg_color[r].set)
					snprintf(line, sizeof line, "color.%-10s: %d,%d",
					         color_role_name[r], cfg_color[r].fg, cfg_color[r].bg);
				else
					snprintf(line, sizeof line, "color.%-10s: (theme)",
					         color_role_name[r]);
			}
			attrset(it == sel ? A_REVERSE : A_NORMAL);
			mvprintw(y + it, 2, "%-*.*s", COLS - 4, COLS - 4, line);
			attrset(A_NORMAL);
		}
		bar_at(LINES - 1, "config: written to your XDG config dir on every change");
		refresh();

		int c = getch();
		if (c == 27 || c == 'q') {
			save_config();
			return;
		}
		if (c == 'j' || c == KEY_DOWN)
			sel = (sel + 1) % n_items;
		else if (c == 'k' || c == KEY_UP)
			sel = (sel - 1 + n_items) % n_items;
		else if (c == 'd' && sel >= ROOT0 && sel < ADD) {
			cfg_remove_root(sel - ROOT0);
			save_config();
		} else if (c == '\n' || c == '\r') {
			if (sel == 0) { /* cycle theme: dark → light → bbs → mono */
				const char *next =
				    strcmp(cfg_theme, "dark") == 0 || strcmp(cfg_theme, "default") == 0 ? "light"
				    : strcmp(cfg_theme, "light") == 0 ? "bbs"
				    : strcmp(cfg_theme, "bbs") == 0   ? "mono"
				                                      : "dark";
				snprintf(cfg_theme, sizeof cfg_theme, "%s", next);
				init_styles(g_color);
				save_config();
			} else if (sel == 1) { /* toggle editor */
				snprintf(cfg_editor, sizeof cfg_editor, "%s",
				         strcmp(cfg_editor, "internal") == 0 ? "system" : "internal");
				save_config();
			} else if (sel == 2) { /* cycle group order */
				const char *next = strcmp(cfg_group_order, "type") == 0    ? "count"
				                   : strcmp(cfg_group_order, "count") == 0 ? "priority"
				                                                           : "type";
				snprintf(cfg_group_order, sizeof cfg_group_order, "%s", next);
				if (strcmp(cfg_group_order, "priority") == 0) {
					/* let the user set the list here so the mode actually does something */
					char pl[1024] = "";
					for (int i = 0; i < n_priority; i++) {
						strncat(pl, i ? "," : "", sizeof pl - strlen(pl) - 1);
						strncat(pl, cfg_priority[i], sizeof pl - strlen(pl) - 1);
					}
					if (prompt_line("Priority types (comma-separated): ", pl,
					                sizeof pl)) {
						n_priority = 0;
						for (char *tok = strtok(pl, ","); tok && n_priority < 32;
						     tok = strtok(NULL, ","))
							snprintf(cfg_priority[n_priority++], sizeof cfg_priority[0],
							         "%s", trim(tok));
					}
				}
				save_config();
			} else if (sel == ADD) { /* add root */
				char r[1024] = "";
				if (prompt_line("Add search root (path): ", r, sizeof r) && *r) {
					cfg_add_root(r);
					save_config();
				}
			} else if (sel > ADD) { /* set a color */
				int role = sel - COL0;
				char v[64] = "";
				char lbl[64];
				snprintf(lbl, sizeof lbl, "color.%s = (fg or fg,bg): ",
				         color_role_name[role]);
				if (prompt_line(lbl, v, sizeof v) && *v) {
					cfg_set_color(role, v);
					init_styles(g_color);
					save_config();
				}
			}
		}
	}
}

/* ---- browser (one open bundle) ----------------------------------------- */

static const char *const BROWSE_HELP[] = {
    "F9 / \\              open the menu bar",
    "j / k  or  arrows   move within the focused pane",
    "l / h  or  arrows   focus content / tree",
    "Tab                 collapse/expand the current group",
    "Shift+Tab  or  *    collapse all / expand all",
    "Space / Enter       fold a group, or open a concept",
    "g / G , J / K       first/last · scroll content",
    "1-9                 follow a numbered body link",
    "e edit · n new · E export PDF · , settings",
    "Esc  back to bundles    ?  help    q  quit",
};

/* draw the two-pane frame; the focused pane's border is brightened */
/* one connected single-line frame split into two panes; the focused pane's
 * border is redrawn in the bright frame colour so it reads as active. */
static void draw_frame(int rows, int cols, int split, int focus) {
	int b = 1, bb = rows - 2; /* top / bottom border rows */
	attrset(sty_frame);
	mvaddstr(b, 0, SL_TL);
	mvaddstr(b, cols - 1, SL_TR);
	mvaddstr(bb, 0, SL_BL);
	mvaddstr(bb, cols - 1, SL_BR);
	for (int c = 1; c < cols - 1; c++) {
		mvaddstr(b, c, SL_H);
		mvaddstr(bb, c, SL_H);
	}
	mvaddstr(b, split, SL_TD);  /* ┬ joins the divider to the top edge */
	mvaddstr(bb, split, SL_TU); /* ┴ joins it to the bottom edge */
	for (int y = b + 1; y < bb; y++) {
		mvaddstr(y, 0, SL_V);
		mvaddstr(y, split, SL_V);
		mvaddstr(y, cols - 1, SL_V);
	}
	attrset(A_NORMAL);

	/* redraw the focused pane's three edges (outer verticals + top/bottom run +
	 * the shared divider) in the bright frame colour */
	attrset(sty_framef);
	int x0 = focus ? split : 0, x1 = focus ? cols - 1 : split;
	for (int c = x0; c <= x1; c++) {
		mvaddstr(b, c, c == x0 ? (focus ? SL_TD : SL_TL) : c == x1 ? (focus ? SL_TR : SL_TD) : SL_H);
		mvaddstr(bb, c, c == x0 ? (focus ? SL_TU : SL_BL) : c == x1 ? (focus ? SL_BR : SL_TU) : SL_H);
	}
	for (int y = b + 1; y < bb; y++) {
		mvaddstr(y, x0, SL_V);
		mvaddstr(y, x1, SL_V);
	}
	attrset(A_NORMAL);

	/* pane titles on the top border; the focused one highlighted */
	attrset(focus == 0 ? sty_framef | A_REVERSE : sty_frame);
	mvprintw(b, 2, " TREE ");
	attrset(focus == 1 ? sty_framef | A_REVERSE : sty_frame);
	mvprintw(b, split + 2, " CONTENT ");
	attrset(A_NORMAL);
}

/* ---- dropdown menus (File / Edit / View / Settings / Help) -------------- */

/* menu-only synthetic keys (returned by menu_run, handled in the loops) */
enum {
	MK_BASE = 0x1000,
	MK_FOLDALL, MK_UNFOLDALL, MK_FOCTREE, MK_FOCCONTENT, MK_THEME, MK_ABOUT
};
enum { MENU_PICKER, MENU_BROWSER };

typedef struct {
	const char *label;
	int key; /* the key the loop already handles; 0 = separator */
} MItem;
typedef struct {
	const char *title;
	const MItem *items;
	int n;
} MMenu;

static const MItem PK_FILE[] = {
    {"Open bundle", '\n'}, {"New bundle", 'N'}, {"", 0}, {"Quit", 'q'}};
static const MItem PK_SET[] = {{"Preferences...", ','}, {"Cycle theme", MK_THEME}};
static const MItem PK_HELP[] = {{"Key reference", '?'}, {"About OKFI", MK_ABOUT}};
static const MMenu PK_MENUS[] = {
    {"File", PK_FILE, 4}, {"Settings", PK_SET, 2}, {"Help", PK_HELP, 2}};

static const MItem BR_FILE[] = {{"New concept", 'n'},  {"Export to PDF", 'E'},
                                {"", 0},               {"Close bundle", 27},
                                {"", 0},               {"Quit", 'q'}};
static const MItem BR_EDIT[] = {{"Edit concept", 'e'}};
static const MItem BR_VIEW[] = {{"Collapse all", MK_FOLDALL},
                                {"Expand all", MK_UNFOLDALL},
                                {"", 0},
                                {"Focus tree", MK_FOCTREE},
                                {"Focus content", MK_FOCCONTENT}};
static const MItem BR_SET[] = {{"Preferences...", ','}, {"Cycle theme", MK_THEME}};
static const MItem BR_HELP[] = {{"Key reference", '?'}, {"About OKFI", MK_ABOUT}};
static const MMenu BR_MENUS[] = {{"File", BR_FILE, 6}, {"Edit", BR_EDIT, 1},
                                 {"View", BR_VIEW, 5}, {"Settings", BR_SET, 2},
                                 {"Help", BR_HELP, 2}};

static void menus_for(int ctx, const MMenu **m, int *n) {
	if (ctx == MENU_BROWSER) {
		*m = BR_MENUS;
		*n = (int)(sizeof BR_MENUS / sizeof BR_MENUS[0]);
	} else {
		*m = PK_MENUS;
		*n = (int)(sizeof PK_MENUS / sizeof PK_MENUS[0]);
	}
}

static void menu_layout(int ctx, int *xs) {
	const MMenu *m;
	int n;
	menus_for(ctx, &m, &n);
	int x = 1 + 4 + 3; /* " OKFI   " */
	for (int i = 0; i < n; i++) {
		xs[i] = x;
		x += (int)strlen(m[i].title) + 2;
	}
}

/* draw the row-0 menu bar; active >= 0 highlights that title; right is an
 * optional right-aligned label (e.g. the open bundle name) */
static void menu_bar(int ctx, int active, const char *right) {
	const MMenu *m;
	int n, xs[8];
	menus_for(ctx, &m, &n);
	menu_layout(ctx, xs);
	attron(sty_bar);
	mvprintw(0, 0, "%-*.*s", COLS - 1, COLS - 1, "");
	mvprintw(0, 1, "OKFI");
	attroff(sty_bar);
	for (int i = 0; i < n; i++) {
		attron(i == active ? (sty_bar | A_REVERSE) : sty_bar);
		mvprintw(0, xs[i], " %s ", m[i].title);
		attroff(i == active ? (sty_bar | A_REVERSE) : sty_bar);
	}
	if (right) {
		int rx = COLS - 2 - (int)strlen(right);
		if (rx > xs[n - 1] + 8) {
			attron(sty_bar);
			mvprintw(0, rx, "%s", right);
			attroff(sty_bar);
		}
	}
}

/* run the menu bar modally; returns the chosen item's key, or 0 if cancelled */
static int menu_run(int ctx) {
	const MMenu *m;
	int n, xs[8];
	menus_for(ctx, &m, &n);
	int mi = 0, ii = 0;
	while (ii < m[mi].n && m[mi].items[ii].key == 0)
		ii++;
	for (;;) {
		erase();
		menu_layout(ctx, xs);
		menu_bar(ctx, mi, NULL);
		int items = m[mi].n, w = 0;
		for (int k = 0; k < items; k++) {
			int l = (int)strlen(m[mi].items[k].label);
			if (l > w)
				w = l;
		}
		w += 4;
		int dx = xs[mi];
		if (dx + w > COLS)
			dx = COLS - w;
		if (dx < 0)
			dx = 0;
		attrset(sty_framef);
		draw_sbox(1, dx, items + 2, w, NULL);
		attrset(A_NORMAL);
		for (int k = 0; k < items; k++) {
			int yy = 2 + k;
			if (m[mi].items[k].key == 0) { /* separator joined to the box sides */
				attrset(sty_framef);
				mvaddstr(yy, dx, SL_VR);
				for (int c2 = 1; c2 < w - 1; c2++)
					mvaddstr(yy, dx + c2, SL_H);
				mvaddstr(yy, dx + w - 1, SL_VL);
				attrset(A_NORMAL);
				continue;
			}
			attrset(k == ii ? A_REVERSE : A_NORMAL);
			mvprintw(yy, dx + 1, " %-*.*s", w - 3, w - 3, m[mi].items[k].label);
			attrset(A_NORMAL);
		}
		refresh();
		int c = getch();
		if (c == 27 || c == KEY_F(9) || c == 'q')
			return 0;
		else if (c == KEY_LEFT || c == 'h') {
			mi = (mi - 1 + n) % n;
			ii = 0;
			while (ii < m[mi].n && m[mi].items[ii].key == 0)
				ii++;
		} else if (c == KEY_RIGHT || c == 'l') {
			mi = (mi + 1) % n;
			ii = 0;
			while (ii < m[mi].n && m[mi].items[ii].key == 0)
				ii++;
		} else if (c == KEY_UP || c == 'k') {
			do
				ii = (ii - 1 + m[mi].n) % m[mi].n;
			while (m[mi].items[ii].key == 0);
		} else if (c == KEY_DOWN || c == 'j') {
			do
				ii = (ii + 1) % m[mi].n;
			while (m[mi].items[ii].key == 0);
		} else if (c == '\n' || c == '\r' || c == KEY_ENTER) {
			return m[mi].items[ii].key;
		}
	}
}

static const char *const ABOUT_TEXT[] = {
    "OKFI — Open Knowledge Format Interface",
    "",
    "A terminal browser, editor, and PDF exporter",
    "for OKF knowledge-catalog bundles.",
    "",
    "F9 / \\  open menus    ?  key reference",
};
static void about_dialog(void) {
	run_help("About OKFI", ABOUT_TEXT,
	         (int)(sizeof ABOUT_TEXT / sizeof ABOUT_TEXT[0]));
}

static int run_export(const char *path); /* defined in the PDF-export section */

/* returns 0 to quit the app, 1 to go back to the bundle picker */
static int run_browser(int picker_active) {
	int sel = first_concept_row(), list_top = 0, body_top = 0;
	int built_ci = -1, built_w = -1, focus = 0; /* focus: 0=tree, 1=content */
	for (;;) {
		if (nvrows == 0)
			sel = 0;
		else if (sel >= nvrows)
			sel = nvrows - 1;
		else if (sel < 0)
			sel = 0;
		int on_header = nvrows && vrows[sel].header;
		int ci = (nvrows && !on_header) ? vrows[sel].cidx : -1;

		erase();
		int rows = LINES, cols = COLS - 1; /* reserve last column */
		int split = cols / 3;
		if (split > 44)
			split = 44;
		if (split < 20)
			split = 20;
		int top = 2, bottom = rows - 2, visible = bottom - top;
		int listx = 1, listw = split - 1;
		int bodyx = split + 1, bodyw = cols - split - 2;
		if (bodyw < 1)
			bodyw = 1;

		menu_bar(MENU_BROWSER, -1, base_name(bundle_root));
		draw_frame(rows, cols, split, focus);

		/* left pane: type-grouped, collapsible tree */
		if (sel < list_top)
			list_top = sel;
		if (sel >= list_top + visible)
			list_top = sel - visible + 1;
		for (int i = 0; i < visible && list_top + i < nvrows; i++) {
			VRow *v = &vrows[list_top + i];
			int yy = top + i, on = (list_top + i == sel);
			char line[256];
			if (v->header)
				snprintf(line, sizeof line, "%s %s (%d)",
				         v->collapsed ? "\xe2\x96\xb8" : "\xe2\x96\xbe", /* ▸ / ▾ */
				         v->label, v->count);
			else
				snprintf(line, sizeof line, "  %s", v->label);
			attrset(on ? A_REVERSE : (v->header ? sty_head : A_NORMAL));
			mvprintw(yy, listx, "%-*.*s", listw, listw, line);
			attrset(A_NORMAL);
		}

		/* right pane: a concept body, or a folder summary for a group header */
		if (ci >= 0) {
			if (ci != built_ci || bodyw != built_w) {
				build_right(&concepts[ci], bodyw);
				built_ci = ci;
				built_w = bodyw;
				body_top = 0;
			}
			if (body_top > nwl - 1)
				body_top = nwl > 0 ? nwl - 1 : 0;
			for (int i = 0; i < visible && body_top + i < nwl; i++)
				draw_wline(top + i, bodyx, &wl[body_top + i], bodyw);
		} else if (on_header) {
			char s[256];
			snprintf(s, sizeof s, "%s  (%d concept%s)", vrows[sel].label,
			         vrows[sel].count, vrows[sel].count == 1 ? "" : "s");
			attrset(sty_head);
			mvprintw(top, bodyx, "%-*.*s", bodyw, bodyw, s);
			attrset(A_NORMAL);
			int yy = top + 2;
			for (int k = 0; k < nconcepts && yy < bottom; k++)
				if (strcmp(concept_group(k), vrows[sel].label) == 0) {
					char ln[256];
					snprintf(ln, sizeof ln, "  \xe2\x80\xa2 %s", concept_label(k));
					mvprintw(yy++, bodyx, "%-*.*s", bodyw, bodyw, ln);
				}
		}

		char bar[2048];
		int off = snprintf(bar, sizeof bar, "%s  ·  %s  %d/%d", bundle_root,
		                   focus ? "content" : "tree",
		                   ci >= 0 ? (nwl ? body_top + 1 : 0) : 0, ci >= 0 ? nwl : 0);
		for (int i = 0; i < nlinks && off < (int)sizeof bar - 1; i++)
			off += snprintf(bar + off, sizeof bar - off, "  [%d]%s", i + 1,
			                links[i].text);
		bar_at(rows - 1, bar);
		refresh();

		int c = getch();
		if (c == KEY_RESIZE) {
			clear();
			continue;
		}
		if (c == KEY_F(9) || c == '\\') { /* menu → synthetic key */
			c = menu_run(MENU_BROWSER);
			if (!c)
				continue;
		}
		if (c == 'q')
			return 0;
		if (c == 27) /* Esc: back to the bundle list */
			return picker_active ? 1 : 0;
		if (c == '\t') { /* Tab: fold/unfold the current group */
			const char *g = on_header ? vrows[sel].label : ci >= 0 ? concept_group(ci) : NULL;
			if (g) {
				char gg[160];
				snprintf(gg, sizeof gg, "%s", g);
				toggle_collapsed(gg);
				build_tree_view();
				sel = vrow_of_header(gg);
			}
		} else if (c == KEY_BTAB) { /* Shift+Tab: fold/unfold all */
			int allc = 1;
			for (int i = 0; i < nvrows; i++)
				if (vrows[i].header && !vrows[i].collapsed)
					allc = 0;
			if (allc)
				expand_all();
			else
				collapse_all();
			build_tree_view();
			if (sel >= nvrows)
				sel = nvrows - 1;
		} else if (c == MK_FOLDALL) {
			collapse_all();
			build_tree_view();
			if (sel >= nvrows)
				sel = nvrows - 1;
		} else if (c == MK_UNFOLDALL) {
			expand_all();
			build_tree_view();
		} else if (c == MK_FOCTREE) {
			focus = 0;
		} else if (c == MK_FOCCONTENT) {
			if (ci >= 0)
				focus = 1;
		} else if (c == MK_THEME) {
			const char *nx = strcmp(cfg_theme, "dark") == 0 || strcmp(cfg_theme, "default") == 0 ? "light"
			                 : strcmp(cfg_theme, "light") == 0 ? "bbs"
			                 : strcmp(cfg_theme, "bbs") == 0   ? "mono"
			                                                   : "dark";
			snprintf(cfg_theme, sizeof cfg_theme, "%s", nx);
			init_styles(g_color);
			save_config();
			built_ci = -1;
		} else if (c == MK_ABOUT) {
			about_dialog();
		} else if (c == 'E' && ci >= 0) { /* export this concept to PDF */
			char p[1100];
			snprintf(p, sizeof p, "%s/%s", bundle_root, concepts[ci].relpath);
			endwin();
			run_export(p);
			reset_prog_mode();
			clear();
		} else if (c == 'l' || c == KEY_RIGHT) {
			if (ci >= 0)
				focus = 1;
		} else if (c == 'h' || c == KEY_LEFT) {
			focus = 0;
		} else if (c == ' ' || c == '\n' || c == '\r' || c == KEY_ENTER) {
			if (!focus && on_header) { /* collapse/expand this group */
				char g[160];
				snprintf(g, sizeof g, "%s", vrows[sel].label);
				toggle_collapsed(g);
				build_tree_view();
				sel = vrow_of_header(g);
			} else if (ci >= 0) {
				focus = 1;
			}
		} else if (c == 'j' || c == KEY_DOWN) {
			if (focus)
				body_top++;
			else if (sel < nvrows - 1)
				sel++;
		} else if (c == 'k' || c == KEY_UP) {
			if (focus) {
				if (body_top > 0)
					body_top--;
			} else if (sel > 0)
				sel--;
		} else if (c == 'g') {
			if (focus)
				body_top = 0;
			else
				sel = 0;
		} else if (c == 'G') {
			if (focus)
				body_top = nwl > 0 ? nwl - 1 : 0;
			else
				sel = nvrows ? nvrows - 1 : 0;
		} else if (c == '*') { /* collapse all, or expand all if already folded */
			int anyg = 0, allc = 1;
			for (int i = 0; i < nvrows; i++)
				if (vrows[i].header) {
					anyg = 1;
					if (!vrows[i].collapsed)
						allc = 0;
				}
			if (anyg) {
				if (allc)
					expand_all();
				else
					collapse_all();
				build_tree_view();
				if (sel >= nvrows)
					sel = nvrows - 1;
			}
		} else if (c == 'J' || c == KEY_NPAGE) {
			body_top++;
		} else if (c == 'K' || c == KEY_PPAGE) {
			if (body_top > 0)
				body_top--;
		} else if (c >= '1' && c <= '9') {
			int li = c - '1';
			if (li < nlinks) {
				int tc = links[li].target;
				const char *g = concept_group(tc);
				if (is_collapsed(g)) { /* expand so the row exists */
					toggle_collapsed(g);
					build_tree_view();
				}
				sel = vrow_of_concept(tc);
				focus = 0;
			}
		} else if (c == 'e' && ci >= 0) {
			char p[1100];
			snprintf(p, sizeof p, "%s/%s", bundle_root, concepts[ci].relpath);
			if (strcmp(cfg_editor, "system") == 0)
				launch_external_editor(p);
			else
				run_editor(p);
			load_bundle(bundle_root); /* in-bundle reload — keep folds */
			built_ci = -1;
		} else if (c == 'n') {
			new_concept_ui();
			built_ci = -1;
		} else if (c == ',') {
			run_settings();
			init_styles(g_color);
			build_tree_view(); /* group order may have changed */
			if (sel >= nvrows)
				sel = nvrows ? nvrows - 1 : 0;
			built_ci = -1;
		} else if (c == '?') {
			run_help("Browser keys", BROWSE_HELP,
			         (int)(sizeof BROWSE_HELP / sizeof BROWSE_HELP[0]));
		}
	}
}

/* ---- bundle picker (project → project) --------------------------------- */

static const char *const PICKER_HELP[] = {
    "F9 / \\     open the menu bar",
    "j / k      previous / next bundle",
    "Enter      open the selected bundle",
    "N          create a new bundle",
    ",          settings (search roots, theme, colors)",
    "?          this help          q  quit",
};

static void run_picker(void) {
	int sel = 0, top = 0;
	for (;;) {
		erase();
		int rows = LINES, cols = COLS - 1; /* reserve last column */
		char binfo[48];
		snprintf(binfo, sizeof binfo, "%d bundle%s", nbundles, nbundles == 1 ? "" : "s");
		menu_bar(MENU_PICKER, -1, binfo);

		int logoy = rows > 18 ? 2 : 1; /* draw the gradient logo near the top */
		int after = logoy + draw_logo(logoy, cols);

		int boxtop = after + 1, boxbot = rows - 1; /* list box up to the status row */
		int boxh = boxbot - boxtop;
		if (boxh < 3) {
			boxtop = 1;
			boxh = boxbot - boxtop;
		}
		char btitle[64];
		snprintf(btitle, sizeof btitle, "BUNDLES (%d)", nbundles);
		attrset(sty_framef);
		draw_sbox(boxtop, 0, boxh, cols, btitle);
		attrset(A_NORMAL);

		int ly = boxtop + 1, lh = boxh - 2, innerw = cols - 4;
		if (nbundles == 0) {
			mvprintw(ly + 1, 2, "%.*s", innerw,
			         "No OKF bundles found under your search roots.");
			mvprintw(ly + 2, 2, "%.*s", innerw,
			         "Press N to create one, or open Settings to add a search root.");
		}
		if (sel < top)
			top = sel;
		if (sel >= top + lh)
			top = sel - lh + 1;
		for (int i = 0; i < lh && top + i < nbundles; i++) {
			Bundle *b = &bundles[top + i];
			char line[1200];
			snprintf(line, sizeof line, "%-22s  %s", b->name, b->path);
			int on = (top + i == sel);
			attrset(on ? A_REVERSE : A_NORMAL);
			mvprintw(ly + i, 2, "%-*.*s", innerw, innerw, line);
			if (!on) { /* tint the path dim */
				int pp = 24;
				if (pp < innerw) {
					attrset(sty_tag);
					mvprintw(ly + i, 2 + pp, "%.*s", innerw - pp, line + pp);
				}
			}
			attrset(A_NORMAL);
		}
		bar_at(rows - 1, "Enter open · N new · F9 menu · , settings · ? help · q quit");
		refresh();

		int c = getch();
		if (c == KEY_RESIZE) {
			clear();
			continue;
		}
		if (c == KEY_F(9) || c == '\\') { /* open the menu; result is a synthetic key */
			c = menu_run(MENU_PICKER);
			if (!c)
				continue;
		}
		if (c == 'q')
			return;
		if (c == 'j' || c == KEY_DOWN) {
			if (sel < nbundles - 1)
				sel++;
		} else if (c == 'k' || c == KEY_UP) {
			if (sel > 0)
				sel--;
		} else if (c == MK_THEME) {
			const char *nx = strcmp(cfg_theme, "dark") == 0 || strcmp(cfg_theme, "default") == 0 ? "light"
			                 : strcmp(cfg_theme, "light") == 0 ? "bbs"
			                 : strcmp(cfg_theme, "bbs") == 0   ? "mono"
			                                                   : "dark";
			snprintf(cfg_theme, sizeof cfg_theme, "%s", nx);
			init_styles(g_color);
			save_config();
		} else if (c == MK_ABOUT) {
			about_dialog();
		} else if ((c == '\n' || c == '\r') && nbundles > 0) {
			load_bundle(bundles[sel].path); /* switching bundles */
			if (run_browser(1) == 0)
				return;
		} else if (c == 'N') {
			char dir[1024] = "";
			if (prompt_line("New bundle directory (e.g. ~/Projects/foo/okf): ", dir,
			                sizeof dir) &&
			    *dir) {
				char ex[1100]; /* expand a leading ~ */
				const char *home = getenv("HOME");
				if (dir[0] == '~' && dir[1] == '/' && home)
					snprintf(ex, sizeof ex, "%s%s", home, dir + 1);
				else
					snprintf(ex, sizeof ex, "%s", dir);
				new_bundle_at(ex);
				discover_all();
			}
		} else if (c == ',') {
			unsigned before = roots_sig();
			run_settings();
			init_styles(g_color);
			if (roots_sig() != before) /* only rescan when the roots changed */
				discover_all();
		} else if (c == '?') {
			run_help("Bundle picker keys", PICKER_HELP,
			         (int)(sizeof PICKER_HELP / sizeof PICKER_HELP[0]));
		}
	}
}

static void run_ui(const char *direct_dir, int force_mono) {
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	scrollok(stdscr, FALSE); /* never scroll the screen on a corner write */
	idlok(stdscr, FALSE);
	set_escdelay(25); /* make ESC-to-cancel feel instant */
	{ /* disable XON/XOFF so the editor's ^S reaches us instead of pausing the tty */
		struct termios t;
		if (tcgetattr(STDIN_FILENO, &t) == 0) {
			t.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
			tcsetattr(STDIN_FILENO, TCSANOW, &t);
		}
	}

	g_color = 0;
	if (!force_mono && !getenv("NO_COLOR") && has_colors()) {
		start_color();
		use_default_colors();
		g_color = 1;
	}
	init_styles(g_color);

	if (direct_dir) {
		load_bundle(direct_dir);
		run_browser(0);
	} else {
		discover_all();
		run_picker();
	}
	endwin();
}

/* ---- pdf export (milstd milmanual, per the user's §8 PDF directive) ---- */

/* Emit text with LaTeX specials escaped. In tt mode, break each hyphen with
 * {} so \texttt{--mono} keeps two hyphens rather than forming an en-dash. */
static void lx(FILE *o, const char *s, int len, int tt) {
	for (int i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)s[i];
		switch (ch) {
		case '&': fputs("\\&", o); break;
		case '%': fputs("\\%", o); break;
		case '$': fputs("\\$", o); break;
		case '#': fputs("\\#", o); break;
		case '_': fputs("\\_", o); break;
		case '{': fputs("\\{", o); break;
		case '}': fputs("\\}", o); break;
		case '\\': fputs("\\textbackslash{}", o); break;
		case '^': fputs("\\textasciicircum{}", o); break;
		case '~': fputs("\\textasciitilde{}", o); break;
		case '`': fputs("\\textasciigrave{}", o); break; /* else a quote ligature */
		case '-': fputs(tt ? "-{}" : "-", o); break;
		default: fputc(ch, o);
		}
	}
}

static void lx_str(FILE *o, const char *s, int tt) { lx(o, s, (int)strlen(s), tt); }

/* Inline markdown → LaTeX: same grammar (and nesting) as the screen styler,
 * different output. Code spans stay literal; other spans recurse. */
static void latex_run(FILE *o, const char *s, int L, int depth) {
	for (int i = 0; i < L;) {
		char c = s[i];
		if (c == '\\' && i + 1 < L) {
			lx(o, s + i + 1, 1, 0);
			i += 2;
			continue;
		}
		if (c == '`') {
			int cs, cl, ni;
			if (code_span(s, i, L, &cs, &cl, &ni)) {
				fputs("\\texttt{", o);
				lx(o, s + cs, cl, 1);
				fputc('}', o);
				i = ni;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && c == '*' && i + 1 < L && s[i + 1] == '*') {
			int j = i + 2;
			while (j + 1 < L && !(s[j] == '*' && s[j + 1] == '*'))
				j++;
			if (j + 1 < L && s[j] == '*' && s[j + 1] == '*') {
				fputs("\\textbf{", o);
				latex_run(o, s + i + 2, j - (i + 2), depth + 1);
				fputc('}', o);
				i = j + 2;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && (c == '*' || c == '_')) {
			int j = i + 1;
			while (j < L && s[j] != c)
				j++;
			if (j < L && j > i + 1) {
				fputs("\\emph{", o);
				latex_run(o, s + i + 1, j - (i + 1), depth + 1);
				fputc('}', o);
				i = j + 1;
				continue;
			}
		}
		if (depth < INLINE_MAX_DEPTH && c == '[') {
			int rb = i + 1;
			while (rb < L && s[rb] != ']')
				rb++;
			if (rb + 1 < L && s[rb + 1] == '(') {
				int rp = rb + 2;
				while (rp < L && s[rp] != ')')
					rp++;
				if (rp < L) {
					fputs("\\href{", o);
					lx(o, s + rb + 2, rp - (rb + 2), 0);
					fputs("}{", o);
					latex_run(o, s + i + 1, rb - (i + 1), depth + 1);
					fputc('}', o);
					i = rp + 1;
					continue;
				}
			}
		}
		lx(o, s + i, 1, 0);
		i++;
	}
}

static void latex_inline(FILE *o, const char *s) {
	latex_run(o, s, (int)strlen(s), 0);
}

/* A pipe table → a booktabs tabular (no vertical rules), honoring alignment. */
static void emit_table_latex(FILE *o, char **lines, int start, int end) {
	char tmp[RBUF];
	char *cells[64];
	int align[64] = {0}, ncols = 0;

	strncpy(tmp, lines[start + 1], sizeof tmp - 1);
	tmp[sizeof tmp - 1] = '\0';
	int snc = split_cells(tmp, cells, 64);
	for (int c = 0; c < snc; c++)
		align[c] = col_align(cells[c]);
	for (int r = start; r < end; r++) {
		if (r == start + 1)
			continue;
		strncpy(tmp, lines[r], sizeof tmp - 1);
		tmp[sizeof tmp - 1] = '\0';
		int nc = split_cells(tmp, cells, 64);
		if (nc > ncols)
			ncols = nc;
	}
	if (ncols < 1)
		return;

	fputs("\\begin{center}\n\\begin{tabular}{", o);
	for (int c = 0; c < ncols; c++)
		fputc(align[c] == AL_RIGHT ? 'r' : align[c] == AL_CENTER ? 'c' : 'l', o);
	fputs("}\n\\toprule\n", o);
	for (int r = start; r < end; r++) {
		if (r == start + 1) {
			fputs("\\midrule\n", o);
			continue;
		}
		strncpy(tmp, lines[r], sizeof tmp - 1);
		tmp[sizeof tmp - 1] = '\0';
		int nc = split_cells(tmp, cells, 64);
		for (int c = 0; c < ncols; c++) {
			if (c)
				fputs(" & ", o);
			if (r == start)
				fputs("\\textbf{", o);
			latex_inline(o, c < nc ? cells[c] : "");
			if (r == start)
				fputc('}', o);
		}
		fputs(" \\\\\n", o);
	}
	fputs("\\bottomrule\n\\end{tabular}\n\\end{center}\n", o);
}

/* Body markdown → LaTeX, reusing the shared block classifiers. */
static void emit_body(FILE *o, const char *body) {
	char *buf = strdup(body ? body : "");
	int nlines = 0;
	char **lines = split_lines(buf, &nlines);
	int in_code = 0, in_list = 0;

	for (int i = 0; i < nlines;) {
		char *src = lines[i];
		char *tr = src;
		while (*tr == ' ')
			tr++;
		if (strncmp(tr, "```", 3) == 0) {
			if (!in_code) {
				if (in_list) {
					fputs("\\end{itemize}\n", o);
					in_list = 0;
				}
				fputs("\\begin{lstlisting}\n", o);
				in_code = 1;
			} else {
				fputs("\\end{lstlisting}\n", o);
				in_code = 0;
			}
			i++;
			continue;
		}
		if (in_code) { /* raw — lstlisting takes content verbatim */
			fputs(src, o);
			fputc('\n', o);
			i++;
			continue;
		}
		if (*tr == '\0') { /* blank line → paragraph break */
			fputc('\n', o);
			i++;
			continue;
		}
		int end;
		if (table_block(lines, i, nlines, &end)) {
			if (in_list) {
				fputs("\\end{itemize}\n", o);
				in_list = 0;
			}
			emit_table_latex(o, lines, i, end);
			i = end;
			continue;
		}
		int level, kind;
		char block[RBUF];
		i = read_block(lines, i, nlines, &kind, &level, block, sizeof block);
		if (kind == LK_BULLET) {
			if (!in_list) {
				fputs("\\begin{itemize}\n", o);
				in_list = 1;
			}
			fputs("\\item ", o);
			latex_inline(o, block);
			fputc('\n', o);
			continue;
		}
		if (in_list) {
			fputs("\\end{itemize}\n", o);
			in_list = 0;
		}
		if (kind == LK_HEADING) {
			fputs(level <= 1 ? "\\section{" : "\\milpar{", o);
			latex_inline(o, block);
			fputs("}\n", o);
		} else if (kind == LK_QUOTE) {
			fputs("\\begin{quote}\n", o);
			latex_inline(o, block);
			fputs("\n\\end{quote}\n", o);
		} else {
			latex_inline(o, block);
			fputc('\n', o);
		}
	}
	if (in_list)
		fputs("\\end{itemize}\n", o);
	if (in_code)
		fputs("\\end{lstlisting}\n", o);
	free(lines);
	free(buf);
}

static const char *fm_get(const Concept *c, const char *key) {
	for (int i = 0; i < c->nfm; i++)
		if (strcmp(c->fm[i].key, key) == 0)
			return c->fm[i].val;
	return NULL;
}

/* A frontmatter value: \url it (so a long URL line-breaks) only when it has no
 * char that could break out of \url{}; otherwise escape it like any text. The
 * escaping floor holds for every value either way. */
static void emit_fm_value(FILE *o, const char *v) {
	if (strstr(v, "://") && !strpbrk(v, "{}\\%#^~")) {
		fputs("\\url{", o);
		fputs(v, o);
		fputc('}', o);
	} else {
		lx(o, v, (int)strlen(v), 0);
	}
}

/* Emit the full milmanual document for one concept. */
static void emit_doc(FILE *o, const Concept *c) {
	const char *title = c->title ? c->title : base_name(c->relpath);
	const char *type = c->type ? c->type : "OKF CONCEPT";
	const char *ts = fm_get(c, "timestamp");

	fputs("\\documentclass{milmanual}\n", o);
	fputs("\\usepackage{booktabs}\n", o); /* milmanual provides hyperref itself */
	fputs("\\usepackage{listings}\n", o); /* code blocks: tolerates any content */
	fputs("\\lstset{basicstyle=\\ttfamily\\small,breaklines=true,"
	      "columns=fullflexible,keepspaces=true}\n",
	      o);
	fputs("\\hypersetup{hidelinks}\n", o);
	fputs("\\title{", o);
	lx_str(o, title, 0);
	fputs("}\n\\documentnumber{", o);
	lx_str(o, type, 0);
	fputs("}\n\\organization{OPEN KNOWLEDGE FORMAT}\n", o);
	if (ts && *ts) {
		char d[32];
		int n = 0;
		for (const char *p = ts; *p && *p != 'T' && n < 31; p++)
			d[n++] = *p;
		d[n] = '\0';
		fputs("\\publicationdate{", o);
		lx_str(o, d, 0);
		fputs("}\n", o);
	}
	fputs("\\begin{document}\n\\makemilcover\n\\chapter{", o);
	lx_str(o, title, 0);
	fputs("}\n", o);

	if (c->nfm) {
		fputs("\\section{Frontmatter}\n\\begin{center}\n"
		      "\\begin{tabular}{lp{0.62\\textwidth}}\n\\toprule\n"
		      "\\textbf{Field} & \\textbf{Value} \\\\\n\\midrule\n",
		      o);
		for (int i = 0; i < c->nfm; i++) {
			fputs("\\texttt{", o);
			lx_str(o, c->fm[i].key, 1);
			fputs("} & ", o);
			emit_fm_value(o, c->fm[i].val);
			fputs(" \\\\\n", o);
		}
		fputs("\\bottomrule\n\\end{tabular}\n\\end{center}\n", o);
	}

	emit_body(o, c->body);
	fputs("\\end{document}\n", o);
}

static int copy_file(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb");
	if (!in)
		return -1;
	FILE *out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	char buf[8192];
	size_t n;
	int err = 0;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			err = 1;
			break;
		}
	if (ferror(in))
		err = 1;
	fclose(in);
	if (fclose(out) != 0) /* buffered write errors often surface only here */
		err = 1;
	if (err) {
		unlink(dst); /* don't leave a truncated PDF */
		return -1;
	}
	return 0;
}

static void cleanup_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[1100];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	closedir(d);
	rmdir(dir);
}

static void print_tex_errors(const char *dir) {
	char p[1100];
	snprintf(p, sizeof p, "%s/doc.log", dir);
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	char line[1024];
	int shown = 0;
	while (fgets(line, sizeof line, f) && shown < 5)
		if (line[0] == '!') {
			fputs(line, stderr);
			shown++;
		}
	fclose(f);
}

/* Export one concept .md to ./<name>.pdf via milmanual + pdflatex. */
static int run_export(const char *path) {
	char *content = read_file(path);
	if (!content) {
		fprintf(stderr, "okfi: cannot read '%s'\n", path);
		return 1;
	}
	Concept c;
	parse_buffer(&c, base_name(path), content);
	free(content);

	char dir[] = "/tmp/okfi-pdf-XXXXXX";
	if (!mkdtemp(dir)) {
		fprintf(stderr, "okfi: cannot create temp directory\n");
		return 1;
	}
	char texpath[1100];
	snprintf(texpath, sizeof texpath, "%s/doc.tex", dir);
	FILE *f = fopen(texpath, "w");
	if (!f) {
		fprintf(stderr, "okfi: cannot write %s\n", texpath);
		rmdir(dir); /* empty — nothing to inspect */
		return 1;
	}
	emit_doc(f, &c);
	fclose(f);

	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char ti[2048];
	if (xdg && *xdg)
		snprintf(ti, sizeof ti, ".:%s/milstd//:", xdg);
	else
		snprintf(ti, sizeof ti, ".:%s/.config/milstd//:", home ? home : ".");

	int ok = 1;
	for (int pass = 0; pass < 2 && ok; pass++) { /* twice: settle page refs */
		pid_t pid = fork();
		if (pid < 0) {
			ok = 0;
			break;
		}
		if (pid == 0) {
			setenv("TEXINPUTS", ti, 1); /* in the child — don't mutate our env */
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0) {
				dup2(dn, 1);
				dup2(dn, 2);
			}
			execlp("pdflatex", "pdflatex", "-interaction=nonstopmode",
			       "-halt-on-error", "-no-shell-escape", "-output-directory", dir,
			       texpath, (char *)NULL);
			_exit(127);
		}
		int st;
		waitpid(pid, &st, 0);
		if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
			ok = 0;
	}

	char outname[1100];
	snprintf(outname, sizeof outname, "%s", base_name(path));
	char *dot = strrchr(outname, '.');
	if (dot && strcmp(dot, ".md") == 0)
		*dot = '\0';
	strncat(outname, ".pdf", sizeof outname - strlen(outname) - 1);

	char pdfpath[1100];
	snprintf(pdfpath, sizeof pdfpath, "%s/doc.pdf", dir);

	if (ok && copy_file(pdfpath, outname) == 0) {
		fprintf(stderr, "okfi: wrote %s\n", outname);
		cleanup_dir(dir);
		return 0;
	}
	fprintf(stderr, "okfi: pdflatex failed (kept build dir %s)\n", dir);
	print_tex_errors(dir);
	return 1;
}

/* ---- self-check (no curses, no files) ---------------------------------- */

static int selftest(void) {
	Concept c;
	init_styles(0); /* mono attributes, no curses needed */

	parse_buffer(&c, "tables/orders.md",
	             "---\ntype: BigQuery Table\ntitle: Orders\n---\nrow body\n");
	assert(c.type && strcmp(c.type, "BigQuery Table") == 0);
	assert(c.title && strcmp(c.title, "Orders") == 0);
	assert(strcmp(c.body, "row body\n") == 0);
	assert(!c.reserved);

	parse_buffer(&c, "loose.md", "no frontmatter here\nsecond line");
	assert(c.type == NULL && c.nfm == 0);
	assert(strcmp(c.body, "no frontmatter here\nsecond line") == 0);

	parse_buffer(&c, "unclosed.md", "---\ntype: X\nnever closes\n");
	assert(c.type == NULL); /* tolerated: whole file is body */
	assert(strstr(c.body, "type: X") != NULL);

	parse_buffer(&c, "empty-type.md", "---\ntype:\n---\nb");
	assert(c.type && c.type[0] == '\0'); /* present but empty, not a crash */

	parse_buffer(&c, "inline.md", "---\ntype: T\ntags: [sales, revenue]\n---\n");
	int found = 0;
	for (int i = 0; i < c.nfm; i++)
		if (strcmp(c.fm[i].key, "tags") == 0) {
			assert(strcmp(c.fm[i].val, "sales, revenue") == 0);
			found = 1;
		}
	assert(found);

	/* inline markdown styling: markers stripped, attributes applied */
	{
		char tb[256];
		attr_t ab[256];
		int cap = (int)sizeof tb, n;

		n = 0;
		style_inline("a **b** c", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "a b c") == 0);
		assert(ab[2] & A_BOLD); /* the 'b' */

		n = 0;
		style_inline("see `code` now", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "see code now") == 0);
		assert(ab[4] & A_REVERSE); /* first char of "code" */

		n = 0;
		style_inline("[docs](http://x)", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "docs") == 0);
		assert(ab[0] & A_UNDERLINE);

		n = 0;
		style_inline("unclosed *italic", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "unclosed *italic") == 0); /* literal, no crash */

		/* nested backticks: a `` fence embeds a single backtick, space-stripped */
		n = 0;
		style_inline("v `` `b` `` w", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "v `b` w") == 0);
		assert(ab[2] & A_REVERSE); /* the embedded backtick is inside the span */

		/* nested emphasis: a code span inside bold layers both attributes */
		n = 0;
		style_inline("**bold `c`**", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "bold c") == 0);
		assert(ab[0] & A_BOLD);                  /* 'b' of "bold" */
		assert((ab[5] & A_BOLD) && (ab[5] & A_REVERSE)); /* 'c' is bold + code */

		/* adversarial: unbalanced delimiters emit literally, never crash */
		n = 0;
		style_inline("[no close bracket", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "[no close bracket") == 0);
		n = 0;
		style_inline("[t](no close paren", A_NORMAL, tb, ab, &n, cap);
		tb[n] = '\0';
		assert(strcmp(tb, "[t](no close paren") == 0);

		/* a long delimiter run must terminate and stay within the buffer */
		char run[200];
		int rn = 0;
		for (int k = 0; k < 90; k++)
			run[rn++] = '*';
		run[rn] = '\0';
		n = 0;
		style_inline(run, A_NORMAL, tb, ab, &n, cap);
		assert(n < cap); /* bounded, no overflow, returned */
	}

	/* table detection + cell splitting */
	{
		assert(is_table_sep("|---|:--:|"));
		assert(is_table_sep("  --- | ---"));
		assert(!is_table_sep("| Column | Type |")); /* header, not separator */
		assert(!is_table_sep("just a - dash line"));

		char row[] = "| `a` | b c |  |";
		char *cells[8];
		int nc = split_cells(row, cells, 8);
		assert(nc == 3); /* trailing empty cell from the last pipe is dropped */
		assert(strcmp(cells[0], "`a`") == 0);
		assert(strcmp(cells[1], "b c") == 0);
		assert(strcmp(cells[2], "") == 0);

		assert(col_align(":--:") == AL_CENTER);
		assert(col_align("--:") == AL_RIGHT);
		assert(col_align(":--") == AL_LEFT);
		assert(col_align("---") == AL_LEFT);
	}

	/* paragraph reflow: soft-wrapped lines join into one logical block */
	{
		char *para[] = {"are **pre", "served** now", "", "after"};
		char out[256];
		int kind, lvl, nx;
		nx = read_block(para, 0, 4, &kind, &lvl, out, sizeof out);
		assert(kind == LK_PARA);
		assert(strcmp(out, "are **pre served** now") == 0); /* joined w/ space */
		assert(nx == 2);                                    /* stops at blank */

		char *bul[] = {"- item one", "  wrapped tail", "- item two"};
		nx = read_block(bul, 0, 3, &kind, &lvl, out, sizeof out);
		assert(kind == LK_BULLET);
		assert(strcmp(out, "item one wrapped tail") == 0);
		assert(nx == 2); /* next bullet starts a new block */
	}

	/* cross-link resolution against a fake bundle */
	{
		concepts = malloc(sizeof(Concept) * 2);
		memset(concepts, 0, sizeof(Concept) * 2);
		concepts[0].relpath = strdup("index.md");
		concepts[1].relpath = strdup("tables/orders.md");
		nconcepts = 2;

		assert(resolve_link("/tables/orders.md", "index.md") == 1); /* absolute */
		assert(resolve_link("orders.md", "tables/customers.md") == 1); /* sibling */
		assert(resolve_link("./index.md", "log.md") == 0);            /* ./ */
		assert(resolve_link("/tables/orders.md#x", "index.md") == 1); /* anchor */
		assert(resolve_link("/nope.md", "index.md") == -1);           /* broken */

		free(concepts[0].relpath);
		free(concepts[1].relpath);
		free(concepts);
		concepts = NULL;
		nconcepts = 0;
	}

	parse_buffer(&c, "block.md", "---\ntype: T\ntags:\n  - x\n  - y\n---\n");
	found = 0;
	for (int i = 0; i < c.nfm; i++)
		if (strcmp(c.fm[i].key, "tags") == 0) {
			assert(strcmp(c.fm[i].val, "x, y") == 0);
			found = 1;
		}
	assert(found);

	parse_buffer(&c, "index.md", "---\nokf_version: \"0.1\"\n---\n# Root");
	assert(c.reserved);
	assert(c.type == NULL); /* reserved file, no type required */

	/* LaTeX escaping + inline emit (the export floor) */
	{
		char *buf = NULL;
		size_t sz = 0;
		FILE *m = open_memstream(&buf, &sz);
		latex_inline(m, "a **b** `c_d` [t](/x.md) e & f");
		fclose(m);
		assert(strstr(buf, "\\textbf{b}"));
		assert(strstr(buf, "\\texttt{c\\_d}")); /* underscore escaped in code */
		assert(strstr(buf, "\\href{/x.md}{t}"));
		assert(strstr(buf, "e \\& f")); /* ampersand escaped in prose */
		free(buf);

		buf = NULL;
		sz = 0;
		m = open_memstream(&buf, &sz);
		latex_inline(m, "`--mono`");
		fclose(m);
		assert(strstr(buf, "-{}-{}mono")); /* hyphens broken in \texttt */
		free(buf);

		buf = NULL;
		sz = 0;
		m = open_memstream(&buf, &sz);
		latex_inline(m, "a `` ` `` b"); /* code span holding a literal backtick */
		fclose(m);
		assert(strstr(buf, "\\textasciigrave{}")); /* not a quote ligature */
		free(buf);

		buf = NULL;
		sz = 0;
		m = open_memstream(&buf, &sz);
		latex_inline(m, "**bold `c` and _it_**"); /* nested spans recurse */
		fclose(m);
		assert(strstr(buf, "\\textbf{bold \\texttt{c} and \\emph{it}}"));
		free(buf);

		/* frontmatter URL: clean one keeps \url; one with `}` is escaped */
		buf = NULL;
		sz = 0;
		m = open_memstream(&buf, &sz);
		emit_fm_value(m, "https://ok/path");
		fclose(m);
		assert(strstr(buf, "\\url{https://ok/path}"));
		free(buf);

		buf = NULL;
		sz = 0;
		m = open_memstream(&buf, &sz);
		emit_fm_value(m, "https://x/a}b"); /* would break out of \url{} */
		fclose(m);
		assert(!strstr(buf, "\\url{")); /* escaped instead, floor holds */
		assert(strstr(buf, "\\}"));
		free(buf);
	}

	printf("selftest: ok\n");
	return 0;
}

static void usage(void) {
	fprintf(stderr,
	        "okfi — OKF catalog browser\n"
	        "usage: okfi [--mono|--no-color] [--root DIR]... [bundle-dir]\n"
	        "       okfi --new-bundle DIR\n"
	        "       okfi --new-concept BUNDLE NAME [TYPE]\n"
	        "       okfi --export-pdf CONCEPT.md\n"
	        "       okfi --selftest\n"
	        "\n"
	        "With no bundle-dir, browse all bundles discovered under the search\n"
	        "roots in your config ($XDG_CONFIG_HOME/okfi/config). With a bundle-dir,\n"
	        "open that bundle directly.\n");
}

int main(int argc, char **argv) {
	const char *direct = NULL;
	int force_mono = 0;

	load_config();

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--selftest") == 0)
			return selftest();
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		}
		if (strcmp(argv[i], "--export-pdf") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "okfi: --export-pdf needs a concept .md path\n");
				return 2;
			}
			return run_export(argv[i + 1]);
		}
		if (strcmp(argv[i], "--new-bundle") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "okfi: --new-bundle needs a directory\n");
				return 2;
			}
			new_bundle_at(argv[i + 1]);
			fprintf(stderr, "okfi: seeded bundle at %s\n", argv[i + 1]);
			return 0;
		}
		if (strcmp(argv[i], "--new-concept") == 0) {
			if (i + 2 >= argc) {
				fprintf(stderr,
				        "okfi: --new-concept needs BUNDLE NAME [TYPE]\n");
				return 2;
			}
			const char *type = (i + 3 < argc) ? argv[i + 3] : "Concept";
			if (create_concept_file(argv[i + 1], argv[i + 2], type) != 0) {
				fprintf(stderr, "okfi: could not create concept (exists/reserved?)\n");
				return 1;
			}
			char desc[300];
			snprintf(desc, sizeof desc, "Added `%s`.", argv[i + 2]);
			log_prepend(argv[i + 1], "Creation", desc);
			fprintf(stderr, "okfi: created %s/%s\n", argv[i + 1], argv[i + 2]);
			return 0;
		}
		if (strcmp(argv[i], "--root") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "okfi: --root needs a directory\n");
				return 2;
			}
			cfg_add_root(argv[++i]);
		} else if (strcmp(argv[i], "--mono") == 0 ||
		           strcmp(argv[i], "--no-color") == 0) {
			force_mono = 1;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "okfi: unknown option '%s'\n", argv[i]);
			usage();
			return 2;
		} else {
			direct = argv[i];
		}
	}

	/* first run with no config: seed sensible default search roots and save */
	if (cfg_nroots == 0 && !direct) {
		seed_default_roots();
		save_config();
	}

	setlocale(LC_ALL, ""); /* before initscr: UTF-8 in ncursesw */
	run_ui(direct, force_mono);
	return 0;
}
