/* database.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008-2011 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifdef __linux__
# include <mntent.h>
# include <sys/vfs.h>
# include <sys/mount.h>
# include <sys/statvfs.h>
# include <linux/magic.h>
#endif

#include "apk_defines.h"
#include "apk_package.h"
#include "apk_database.h"
#include "apk_applet.h"
#include "apk_ctype.h"
#include "apk_extract.h"
#include "apk_print.h"
#include "apk_openssl.h"
#include "apk_tar.h"
#include "apk_adb.h"
#include "apk_fs.h"

enum {
	APK_DIR_FREE = 0,
	APK_DIR_REMOVE
};

static const char * const apkindex_tar_gz = "APKINDEX.tar.gz";
static const char * const apk_static_cache_dir = "var/cache/apk";
static const char * const apk_world_file = "etc/apk/world";
static const char * const apk_arch_file = "etc/apk/arch";
static const char * const apk_lock_file = "lib/apk/db/lock";

static struct apk_db_acl *apk_default_acl_dir, *apk_default_acl_file;

struct install_ctx {
	struct apk_database *db;
	struct apk_package *pkg;
	struct apk_installed_package *ipkg;

	int script;
	char **script_args;
	unsigned int script_pending : 1;
	unsigned int missing_checksum : 1;

	struct apk_db_dir_instance *diri;
	struct apk_checksum data_csum;
	struct apk_extract_ctx ectx;

	apk_progress_cb cb;
	void *cb_ctx;
	size_t installed_size;
	size_t current_file_size;

	struct hlist_node **diri_node;
	struct hlist_node **file_diri_node;
};

static mode_t apk_db_dir_get_mode(struct apk_database *db, mode_t mode)
{
	// in usermode, return mode that makes the file readable for user
	if (db->usermode) return mode | S_IWUSR | S_IXUSR;
	return mode;
}

static apk_blob_t apk_pkg_ctx(struct apk_package *pkg)
{
	return APK_BLOB_PTR_LEN(pkg->name->name, strlen(pkg->name->name)+1);
}

static apk_blob_t pkg_name_get_key(apk_hash_item item)
{
	return APK_BLOB_STR(((struct apk_name *) item)->name);
}

static void pkg_name_free(struct apk_name *name)
{
	free(name->name);
	apk_provider_array_free(&name->providers);
	apk_name_array_free(&name->rdepends);
	apk_name_array_free(&name->rinstall_if);
	free(name);
}

static const struct apk_hash_ops pkg_name_hash_ops = {
	.node_offset = offsetof(struct apk_name, hash_node),
	.get_key = pkg_name_get_key,
	.hash_key = apk_blob_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) pkg_name_free,
};

static apk_blob_t pkg_info_get_key(apk_hash_item item)
{
	return APK_BLOB_CSUM(((struct apk_package *) item)->csum);
}

static unsigned long csum_hash(apk_blob_t csum)
{
	/* Checksum's highest bits have the most "randomness", use that
	 * directly as hash */
	if (csum.len >= sizeof(uint32_t))
		return get_unaligned32(csum.ptr);
	return 0;
}

static const struct apk_hash_ops pkg_info_hash_ops = {
	.node_offset = offsetof(struct apk_package, hash_node),
	.get_key = pkg_info_get_key,
	.hash_key = csum_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) apk_pkg_free,
};

static apk_blob_t apk_db_dir_get_key(apk_hash_item item)
{
	struct apk_db_dir *dir = (struct apk_db_dir *) item;
	return APK_BLOB_PTR_LEN(dir->name, dir->namelen);
}

static const struct apk_hash_ops dir_hash_ops = {
	.node_offset = offsetof(struct apk_db_dir, hash_node),
	.get_key = apk_db_dir_get_key,
	.hash_key = apk_blob_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) free,
};

struct apk_db_file_hash_key {
	apk_blob_t dirname;
	apk_blob_t filename;
};

static unsigned long apk_db_file_hash_key(apk_blob_t _key)
{
	struct apk_db_file_hash_key *key = (struct apk_db_file_hash_key *) _key.ptr;

	return apk_blob_hash_seed(key->filename, apk_blob_hash(key->dirname));
}

static unsigned long apk_db_file_hash_item(apk_hash_item item)
{
	struct apk_db_file *dbf = (struct apk_db_file *) item;

	return apk_blob_hash_seed(APK_BLOB_PTR_LEN(dbf->name, dbf->namelen),
				  dbf->diri->dir->hash);
}

static int apk_db_file_compare_item(apk_hash_item item, apk_blob_t _key)
{
	struct apk_db_file *dbf = (struct apk_db_file *) item;
	struct apk_db_file_hash_key *key = (struct apk_db_file_hash_key *) _key.ptr;
	struct apk_db_dir *dir = dbf->diri->dir;
	int r;

	r = apk_blob_compare(key->filename,
			     APK_BLOB_PTR_LEN(dbf->name, dbf->namelen));
	if (r != 0)
		return r;

	r = apk_blob_compare(key->dirname,
			     APK_BLOB_PTR_LEN(dir->name, dir->namelen));
	return r;
}

static const struct apk_hash_ops file_hash_ops = {
	.node_offset = offsetof(struct apk_db_file, hash_node),
	.hash_key = apk_db_file_hash_key,
	.hash_item = apk_db_file_hash_item,
	.compare_item = apk_db_file_compare_item,
	.delete_item = (apk_hash_delete_f) free,
};

struct apk_name *apk_db_query_name(struct apk_database *db, apk_blob_t name)
{
	return (struct apk_name *) apk_hash_get(&db->available.names, name);
}

struct apk_name *apk_db_get_name(struct apk_database *db, apk_blob_t name)
{
	struct apk_name *pn;
	unsigned long hash = apk_hash_from_key(&db->available.names, name);

	pn = (struct apk_name *) apk_hash_get_hashed(&db->available.names, name, hash);
	if (pn != NULL)
		return pn;

	pn = calloc(1, sizeof(struct apk_name));
	if (pn == NULL)
		return NULL;

	pn->name = apk_blob_cstr(name);
	apk_provider_array_init(&pn->providers);
	apk_name_array_init(&pn->rdepends);
	apk_name_array_init(&pn->rinstall_if);
	apk_hash_insert_hashed(&db->available.names, pn, hash);
	db->sorted_names = 0;

	return pn;
}

static int cmp_provider(const void *a, const void *b)
{
	const struct apk_provider *pa = a, *pb = b;
	return apk_pkg_cmp_display(pa->pkg, pb->pkg);
}

struct apk_provider_array *apk_name_sorted_providers(struct apk_name *name)
{
	if (!name->providers_sorted) {
		qsort(name->providers->item,  name->providers->num, sizeof name->providers->item[0], cmp_provider);
		name->providers_sorted = 0;
	}
	return name->providers;
}

static struct apk_db_acl *__apk_db_acl_atomize(struct apk_database *db, mode_t mode, uid_t uid, gid_t gid, uint8_t csum_type, const uint8_t *csum_data)
{
	struct apk_db_acl acl = { .mode = mode & 07777, .uid = uid, .gid = gid };
	apk_blob_t *b;

	if (csum_data && csum_type != APK_CHECKSUM_NONE) {
		acl.xattr_csum.type = csum_type;
		memcpy(acl.xattr_csum.data, csum_data, csum_type);
	}

	b = apk_atomize_dup(&db->atoms, APK_BLOB_STRUCT(acl));
	return (struct apk_db_acl *) b->ptr;
}

static struct apk_db_acl *apk_db_acl_atomize(struct apk_database *db, mode_t mode, uid_t uid, gid_t gid)
{
	return __apk_db_acl_atomize(db, mode, uid, gid, 0, 0);
}

static struct apk_db_acl *apk_db_acl_atomize_csum(struct apk_database *db, mode_t mode, uid_t uid, gid_t gid, const struct apk_checksum *xattr_csum)
{
	return __apk_db_acl_atomize(db, mode, uid, gid, xattr_csum->type, xattr_csum->data);
}

static struct apk_db_acl *apk_db_acl_atomize_digest(struct apk_database *db, mode_t mode, uid_t uid, gid_t gid, const struct apk_digest *dig)
{
	return __apk_db_acl_atomize(db, mode, uid, gid, dig->len, dig->data);
}

static int apk_db_dir_mkdir(struct apk_database *db, struct apk_fsdir *d, struct apk_db_acl *acl)
{
	if (db->ctx->flags & APK_SIMULATE) return 0;
	return apk_fsdir_create(d, apk_db_dir_get_mode(db, acl->mode));
}

void apk_db_dir_prepare(struct apk_database *db, struct apk_db_dir *dir, struct apk_db_acl *acl)
{
	struct apk_fsdir d;

	if (dir->namelen == 0) return;
	if (dir->created) return;

	apk_fsdir_get(&d, APK_BLOB_PTR_LEN(dir->name, dir->namelen), db->ctx, APK_BLOB_NULL);
	if (!acl) {
		/* Directory should not exist. Create it. */
		if (apk_db_dir_mkdir(db, &d, dir->owner->acl) == 0)
			dir->permissions_ok = dir->permissions_stale = 1;
		dir->created = 1;
		return;
	}

	switch (apk_fsdir_check(&d, apk_db_dir_get_mode(db, acl->mode), acl->uid, acl->gid)) {
	case -ENOENT:
		apk_db_dir_mkdir(db, &d, dir->owner->acl);
		dir->permissions_stale = 1;
	case 0:
		dir->permissions_ok = 1;
	case APK_FS_DIR_MODIFIED:
	default:
		dir->created = 1;
		break;
	}
}

void apk_db_dir_unref(struct apk_database *db, struct apk_db_dir *dir, int rmdir_mode)
{
	if (--dir->refs > 0) return;
	db->installed.stats.dirs--;
	apk_protected_path_array_free(&dir->protected_paths);
	if (dir->namelen != 0) {
		if (rmdir_mode == APK_DIR_REMOVE) {
			dir->modified = 1;
			if (!(db->ctx->flags & APK_SIMULATE)) {
				struct apk_fsdir d;
				apk_fsdir_get(&d, APK_BLOB_PTR_LEN(dir->name, dir->namelen), db->ctx, APK_BLOB_NULL);
				apk_fsdir_delete(&d);
			}
		}
		apk_db_dir_unref(db, dir->parent, rmdir_mode);
		dir->parent = NULL;
	}
	dir->created = dir->permissions_ok = dir->permissions_stale = 0;
}

struct apk_db_dir *apk_db_dir_ref(struct apk_db_dir *dir)
{
	dir->refs++;
	return dir;
}

struct apk_db_dir *apk_db_dir_query(struct apk_database *db,
				    apk_blob_t name)
{
	return (struct apk_db_dir *) apk_hash_get(&db->installed.dirs, name);
}

struct apk_db_dir *apk_db_dir_get(struct apk_database *db, apk_blob_t name)
{
	struct apk_db_dir *dir;
	struct apk_protected_path_array *ppaths;
	struct apk_protected_path *ppath;
	apk_blob_t bparent;
	unsigned long hash = apk_hash_from_key(&db->installed.dirs, name);
	char *relative_name;

	if (name.len && name.ptr[name.len-1] == '/') name.len--;

	dir = (struct apk_db_dir *) apk_hash_get_hashed(&db->installed.dirs, name, hash);
	if (dir != NULL && dir->refs) return apk_db_dir_ref(dir);
	if (dir == NULL) {
		dir = calloc(1, sizeof(*dir) + name.len + 1);
		dir->rooted_name[0] = '/';
		memcpy(dir->name, name.ptr, name.len);
		dir->name[name.len] = 0;
		dir->namelen = name.len;
		dir->hash = hash;
		apk_protected_path_array_init(&dir->protected_paths);
		apk_hash_insert_hashed(&db->installed.dirs, dir, hash);
	}

	db->installed.stats.dirs++;
	dir->refs = 1;

	if (name.len == 0) {
		dir->parent = NULL;
		dir->has_protected_children = 1;
		ppaths = NULL;
	} else if (apk_blob_rsplit(name, '/', &bparent, NULL)) {
		dir->parent = apk_db_dir_get(db, bparent);
		dir->protect_mode = dir->parent->protect_mode;
		dir->has_protected_children = !apk_protect_mode_none(dir->protect_mode);
		ppaths = dir->parent->protected_paths;
	} else {
		dir->parent = apk_db_dir_get(db, APK_BLOB_NULL);
		ppaths = db->protected_paths;
	}

	if (ppaths == NULL)
		return dir;

	relative_name = strrchr(dir->rooted_name, '/') + 1;
	foreach_array_item(ppath, ppaths) {
		char *slash = strchr(ppath->relative_pattern, '/');
		if (slash != NULL) {
			*slash = 0;
			if (fnmatch(ppath->relative_pattern, relative_name, FNM_PATHNAME) != 0) {
				*slash = '/';
				continue;
			}
			*slash = '/';

			*apk_protected_path_array_add(&dir->protected_paths) = (struct apk_protected_path) {
				.relative_pattern = slash + 1,
				.protect_mode = ppath->protect_mode,
			};
		} else {
			if (fnmatch(ppath->relative_pattern, relative_name, FNM_PATHNAME) != 0)
				continue;

			dir->protect_mode = ppath->protect_mode;
		}
		dir->has_protected_children |= !apk_protect_mode_none(ppath->protect_mode);
	}

	return dir;
}

static struct apk_db_dir_instance *apk_db_diri_new(struct apk_database *db,
						   struct apk_package *pkg,
						   apk_blob_t name,
						   struct hlist_node ***after)
{
	struct apk_db_dir_instance *diri;

	diri = calloc(1, sizeof(struct apk_db_dir_instance));
	if (diri != NULL) {
		hlist_add_after(&diri->pkg_dirs_list, *after);
		*after = &diri->pkg_dirs_list.next;
		diri->dir = apk_db_dir_get(db, name);
		diri->pkg = pkg;
		diri->acl = apk_default_acl_dir;
	}

	return diri;
}

static void apk_db_dir_apply_diri_permissions(struct apk_database *db, struct apk_db_dir_instance *diri)
{
	struct apk_db_dir *dir = diri->dir;

	if (dir->owner && apk_pkg_replaces_dir(dir->owner->pkg, diri->pkg) != APK_PKG_REPLACES_YES)
		return;

	if (dir->permissions_ok) {
		// Check if the ACL changed and the directory needs update
		if (dir->owner == NULL || dir->owner->acl != diri->acl) {
			dir->permissions_stale = 1;
			db->dirperms_stale = 1;
		}
	}
	dir->owner = diri;
}

static void apk_db_diri_free(struct apk_database *db,
			     struct apk_db_dir_instance *diri,
			     int rmdir_mode)
{
	if (diri->dir->owner == diri) {
		diri->dir->owner = NULL;
		db->dirowner_stale = 1;
	}
	apk_db_dir_unref(db, diri->dir, rmdir_mode);
	free(diri);
}

struct apk_db_file *apk_db_file_query(struct apk_database *db,
				      apk_blob_t dir,
				      apk_blob_t name)
{
	struct apk_db_file_hash_key key;

	if (dir.len && dir.ptr[dir.len-1] == '/')
		dir.len--;

	key = (struct apk_db_file_hash_key) {
		.dirname = dir,
		.filename = name,
	};

	return (struct apk_db_file *) apk_hash_get(&db->installed.files,
						   APK_BLOB_BUF(&key));
}

static struct apk_db_file *apk_db_file_new(struct apk_db_dir_instance *diri,
					   apk_blob_t name,
					   struct hlist_node ***after)
{
	struct apk_db_file *file;

	file = malloc(sizeof(*file) + name.len + 1);
	if (file == NULL)
		return NULL;

	memset(file, 0, sizeof(*file));
	memcpy(file->name, name.ptr, name.len);
	file->name[name.len] = 0;
	file->namelen = name.len;

	file->diri = diri;
	file->acl = apk_default_acl_file;
	hlist_add_after(&file->diri_files_list, *after);
	*after = &file->diri_files_list.next;

	return file;
}

static struct apk_db_file *apk_db_file_get(struct apk_database *db,
					   struct apk_db_dir_instance *diri,
					   apk_blob_t name,
					   struct hlist_node ***after)
{
	struct apk_db_file *file;
	struct apk_db_file_hash_key key;
	struct apk_db_dir *dir = diri->dir;
	unsigned long hash;

	key = (struct apk_db_file_hash_key) {
		.dirname = APK_BLOB_PTR_LEN(dir->name, dir->namelen),
		.filename = name,
	};

	hash = apk_blob_hash_seed(name, dir->hash);
	file = (struct apk_db_file *) apk_hash_get_hashed(
		&db->installed.files, APK_BLOB_BUF(&key), hash);
	if (file != NULL)
		return file;

	file = apk_db_file_new(diri, name, after);
	apk_hash_insert_hashed(&db->installed.files, file, hash);
	db->installed.stats.files++;

	return file;
}

static void add_name_to_array(struct apk_name *name, struct apk_name_array **a)
{
	struct apk_name **n;

	foreach_array_item(n, *a)
		if (*n == name) return;
	*apk_name_array_add(a) = name;
}

static void apk_db_pkg_rdepends(struct apk_database *db, struct apk_package *pkg)
{
	struct apk_name *rname;
	struct apk_dependency *d;

	foreach_array_item(d, pkg->depends) {
		rname = d->name;
		rname->is_dependency |= !apk_dep_conflict(d);
		add_name_to_array(pkg->name, &rname->rdepends);
	}
	foreach_array_item(d, pkg->install_if) {
		rname = d->name;
		add_name_to_array(pkg->name, &rname->rinstall_if);
	}
}

static inline void add_provider(struct apk_name *name, struct apk_provider p)
{
	*apk_provider_array_add(&name->providers) = p;
}

struct apk_package *apk_db_pkg_add(struct apk_database *db, struct apk_package *pkg)
{
	struct apk_package *idb;
	struct apk_dependency *dep;

	if (!pkg->name || !pkg->version) return NULL;

	if (!pkg->license) pkg->license = &apk_atom_null;

	// Set as "cached" if installing from specified file
	if (pkg->filename) pkg->repos |= BIT(APK_REPOSITORY_CACHED);

	idb = apk_hash_get(&db->available.packages, APK_BLOB_CSUM(pkg->csum));
	if (idb == NULL) {
		idb = pkg;
		apk_hash_insert(&db->available.packages, pkg);
		add_provider(pkg->name, APK_PROVIDER_FROM_PACKAGE(pkg));
		foreach_array_item(dep, pkg->provides)
			add_provider(dep->name, APK_PROVIDER_FROM_PROVIDES(pkg, dep));
		if (db->open_complete)
			apk_db_pkg_rdepends(db, pkg);
	} else {
		idb->repos |= pkg->repos;
		if (idb->filename == NULL && pkg->filename != NULL) {
			idb->filename = pkg->filename;
			pkg->filename = NULL;
		}
		if (idb->ipkg == NULL && pkg->ipkg != NULL) {
			idb->ipkg = pkg->ipkg;
			idb->ipkg->pkg = idb;
			pkg->ipkg = NULL;
		}
		apk_pkg_free(pkg);
	}
	return idb;
}

static int apk_pkg_format_cache_pkg(apk_blob_t to, struct apk_package *pkg)
{
	/* pkgname-1.0_alpha1.12345678.apk */
	apk_blob_push_blob(&to, APK_BLOB_STR(pkg->name->name));
	apk_blob_push_blob(&to, APK_BLOB_STR("-"));
	apk_blob_push_blob(&to, *pkg->version);
	apk_blob_push_blob(&to, APK_BLOB_STR("."));
	apk_blob_push_hexdump(&to, APK_BLOB_PTR_LEN((char *) pkg->csum.data,
						    APK_CACHE_CSUM_BYTES));
	apk_blob_push_blob(&to, APK_BLOB_STR(".apk"));
	apk_blob_push_blob(&to, APK_BLOB_PTR_LEN("", 1));
	if (APK_BLOB_IS_NULL(to))
		return -ENOBUFS;
	return 0;
}

int apk_repo_format_cache_index(apk_blob_t to, struct apk_repository *repo)
{
	/* APKINDEX.12345678.tar.gz */
	apk_blob_push_blob(&to, APK_BLOB_STR("APKINDEX."));
	apk_blob_push_hexdump(&to, APK_BLOB_PTR_LEN((char *) repo->csum.data, APK_CACHE_CSUM_BYTES));
	apk_blob_push_blob(&to, APK_BLOB_STR(".tar.gz"));
	apk_blob_push_blob(&to, APK_BLOB_PTR_LEN("", 1));
	if (APK_BLOB_IS_NULL(to))
		return -ENOBUFS;
	return 0;
}

int apk_repo_format_real_url(apk_blob_t *default_arch, struct apk_repository *repo,
			     struct apk_package *pkg, char *buf, size_t len,
			     struct apk_url_print *urlp)
{

	apk_blob_t uri = APK_BLOB_STR(repo->url);
	apk_blob_t arch;
	int r;

	if (pkg && pkg->arch) arch = *pkg->arch;
	else arch = *default_arch;

	if (apk_blob_ends_with(uri, APK_BLOB_STR(".adb"))) {
		if (pkg != NULL) {
			apk_blob_rsplit(uri, '/', &uri, NULL);
			r = snprintf(buf, len, BLOB_FMT "/" PKG_FILE_FMT,
				BLOB_PRINTF(uri), PKG_FILE_PRINTF(pkg));
		} else {
			r = snprintf(buf, len, BLOB_FMT, BLOB_PRINTF(uri));
		}
	} else {
		while (uri.len && uri.ptr[uri.len-1] == '/') uri.len--;
		if (pkg != NULL)
			r = snprintf(buf, len, BLOB_FMT "/" BLOB_FMT "/" PKG_FILE_FMT,
				BLOB_PRINTF(uri), BLOB_PRINTF(arch), PKG_FILE_PRINTF(pkg));
		else
			r = snprintf(buf, len, BLOB_FMT "/" BLOB_FMT "/%s",
				BLOB_PRINTF(uri), BLOB_PRINTF(arch), apkindex_tar_gz);
	}

	if (r >= len)
		return -ENOBUFS;

	if (urlp) apk_url_parse(urlp, buf);
	return 0;
}

int apk_repo_format_item(struct apk_database *db, struct apk_repository *repo, struct apk_package *pkg,
			 int *fd, char *buf, size_t len)
{
	if (repo->url == db->repos[APK_REPOSITORY_CACHED].url) {
		if (db->cache_fd < 0) return db->cache_fd;
		*fd = db->cache_fd;
		return apk_pkg_format_cache_pkg(APK_BLOB_PTR_LEN(buf, len), pkg);
	}

	*fd = AT_FDCWD;
	return apk_repo_format_real_url(db->arch, repo, pkg, buf, len, 0);
}

int apk_cache_download(struct apk_database *db, struct apk_repository *repo,
		       struct apk_package *pkg, int autoupdate,
		       apk_progress_cb cb, void *cb_ctx)
{
	struct apk_out *out = &db->ctx->out;
	struct stat st = {0};
	struct apk_url_print urlp;
	struct apk_istream *is;
	struct apk_ostream *os;
	struct apk_extract_ctx ectx;
	char url[PATH_MAX];
	char cacheitem[128];
	int r;
	time_t now = time(NULL);

	if (db->cache_fd < 0) return db->cache_fd;

	if (pkg != NULL)
		r = apk_pkg_format_cache_pkg(APK_BLOB_BUF(cacheitem), pkg);
	else
		r = apk_repo_format_cache_index(APK_BLOB_BUF(cacheitem), repo);
	if (r < 0) return r;

	r = apk_repo_format_real_url(db->arch, repo, pkg, url, sizeof(url), &urlp);
	if (r < 0) return r;

	if (autoupdate && !(db->ctx->force & APK_FORCE_REFRESH)) {
		if (fstatat(db->cache_fd, cacheitem, &st, 0) == 0 &&
		    now - st.st_mtime <= db->ctx->cache_max_age)
			return -EALREADY;
	}
	apk_msg(out, "fetch " URL_FMT, URL_PRINTF(urlp));

	if (db->ctx->flags & APK_SIMULATE) return 0;

	os = apk_ostream_to_file(db->cache_fd, cacheitem, 0644);
	if (IS_ERR(os)) return PTR_ERR(os);

	if (cb) cb(cb_ctx, 0);

	is = apk_istream_from_url(url, apk_db_url_since(db, st.st_mtime));
	is = apk_istream_tee(is, os, autoupdate ? 0 : APK_ISTREAM_TEE_COPY_META, cb, cb_ctx);
	apk_extract_init(&ectx, db->ctx, 0);
	if (pkg) apk_extract_verify_identity(&ectx, &pkg->csum);
	r = apk_extract(&ectx, is);
	if (r == -EALREADY) {
		if (autoupdate) utimensat(db->cache_fd, cacheitem, NULL, 0);
		return r;
	}
	return r;
}

static struct apk_db_dir_instance *find_diri(struct apk_installed_package *ipkg,
					     apk_blob_t dirname,
					     struct apk_db_dir_instance *curdiri,
					     struct hlist_node ***tail)
{
	struct hlist_node *n;
	struct apk_db_dir_instance *diri;

	if (curdiri != NULL &&
	    apk_blob_compare(APK_BLOB_PTR_LEN(curdiri->dir->name,
					      curdiri->dir->namelen),
			     dirname) == 0)
		return curdiri;

	hlist_for_each_entry(diri, n, &ipkg->owned_dirs, pkg_dirs_list) {
		if (apk_blob_compare(APK_BLOB_PTR_LEN(diri->dir->name,
						      diri->dir->namelen), dirname) == 0) {
			if (tail != NULL)
				*tail = hlist_tail_ptr(&diri->owned_files);
			return diri;
		}
	}
	return NULL;
}

int apk_db_read_overlay(struct apk_database *db, struct apk_istream *is)
{
	struct apk_db_dir_instance *diri = NULL;
	struct hlist_node **diri_node = NULL, **file_diri_node = NULL;
	struct apk_package *pkg;
	struct apk_installed_package *ipkg;
	apk_blob_t token = APK_BLOB_STR("\n"), line, bdir, bfile;

	if (IS_ERR(is)) return PTR_ERR(is);

	pkg = apk_pkg_new();
	if (!pkg) goto no_mem;

	ipkg = apk_pkg_install(db, pkg);
	if (ipkg == NULL) {
	no_mem:
		apk_istream_error(is, -ENOMEM);
		goto err;
	}

	diri_node = hlist_tail_ptr(&ipkg->owned_dirs);

	while (apk_istream_get_delim(is, token, &line) == 0) {
		if (!apk_blob_rsplit(line, '/', &bdir, &bfile)) {
			apk_istream_error(is, -APKE_V2PKG_FORMAT);
			break;
		}

		if (bfile.len == 0) {
			diri = apk_db_diri_new(db, pkg, bdir, &diri_node);
			file_diri_node = &diri->owned_files.first;
			diri->dir->created = 1;
		} else {
			diri = find_diri(ipkg, bdir, diri, &file_diri_node);
			if (diri == NULL) {
				diri = apk_db_diri_new(db, pkg, bdir, &diri_node);
				file_diri_node = &diri->owned_files.first;
			}
			(void) apk_db_file_get(db, diri, bfile, &file_diri_node);
		}
	}
err:
	return apk_istream_close(is);
}

static int apk_db_fdb_read(struct apk_database *db, struct apk_istream *is, int repo, unsigned layer)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_package *pkg = NULL;
	struct apk_installed_package *ipkg = NULL;
	struct apk_db_dir_instance *diri = NULL;
	struct apk_db_file *file = NULL;
	struct apk_db_acl *acl;
	struct hlist_node **diri_node = NULL;
	struct hlist_node **file_diri_node = NULL;
	struct apk_checksum xattr_csum;
	apk_blob_t token = APK_BLOB_STR("\n"), l;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int field, r, lineno = 0;

	if (IS_ERR(is)) return PTR_ERR(is);

	while (apk_istream_get_delim(is, token, &l) == 0) {
		lineno++;

		if (l.len < 2) {
			if (pkg == NULL)
				continue;

			if (diri) apk_db_dir_apply_diri_permissions(db, diri);

			if (repo >= 0) {
				pkg->repos |= BIT(repo);
			} else if (repo == -2) {
				pkg->cached_non_repository = 1;
			} else if (repo == -1 && ipkg == NULL) {
				/* Installed package without files */
				ipkg = apk_pkg_install(db, pkg);
			}

			if (apk_db_pkg_add(db, pkg) == NULL)
				goto err_fmt;
			pkg = NULL;
			ipkg = NULL;
			continue;
		}

		/* Get field */
		field = l.ptr[0];
		if (l.ptr[1] != ':') goto err_fmt;
		l.ptr += 2;
		l.len -= 2;

		/* If no package, create new */
		if (pkg == NULL) {
			pkg = apk_pkg_new();
			pkg->layer = layer;
			ipkg = NULL;
			diri = NULL;
			file_diri_node = NULL;
		}

		/* Standard index line? */
		r = apk_pkg_add_info(db, pkg, field, l);
		if (r == 0)
			continue;
		if (r == 1 && repo == -1 && ipkg == NULL) {
			/* Instert to installed database; this needs to
			 * happen after package name has been read, but
			 * before first FDB entry. */
			ipkg = apk_pkg_install(db, pkg);
			diri_node = hlist_tail_ptr(&ipkg->owned_dirs);
		}
		if (repo != -1 || ipkg == NULL)
			continue;

		/* Check FDB special entries */
		switch (field) {
		case 'F':
			if (diri) apk_db_dir_apply_diri_permissions(db, diri);
			if (pkg->name == NULL) goto bad_entry;
			diri = find_diri(ipkg, l, NULL, &diri_node);
			if (!diri) diri = apk_db_diri_new(db, pkg, l, &diri_node);
			file_diri_node = hlist_tail_ptr(&diri->owned_files);
			break;
		case 'a':
			if (file == NULL) goto bad_entry;
		case 'M':
			if (diri == NULL) goto bad_entry;
			uid = apk_blob_pull_uint(&l, 10);
			apk_blob_pull_char(&l, ':');
			gid = apk_blob_pull_uint(&l, 10);
			apk_blob_pull_char(&l, ':');
			mode = apk_blob_pull_uint(&l, 8);
			if (apk_blob_pull_blob_match(&l, APK_BLOB_STR(":")))
				apk_blob_pull_csum(&l, &xattr_csum);
			else
				xattr_csum.type = APK_CHECKSUM_NONE;

			acl = apk_db_acl_atomize_csum(db, mode, uid, gid, &xattr_csum);
			if (field == 'M')
				diri->acl = acl;
			else
				file->acl = acl;
			break;
		case 'R':
			if (diri == NULL) goto bad_entry;
			file = apk_db_file_get(db, diri, l, &file_diri_node);
			break;
		case 'Z':
			if (file == NULL) goto bad_entry;
			apk_blob_pull_csum(&l, &file->csum);
			break;
		case 'r':
			apk_blob_pull_deps(&l, db, &ipkg->replaces);
			break;
		case 'q':
			ipkg->replaces_priority = apk_blob_pull_uint(&l, 10);
			break;
		case 's':
			ipkg->repository_tag = apk_db_get_tag_id(db, l);
			break;
		case 'f':
			for (r = 0; r < l.len; r++) {
				switch (l.ptr[r]) {
				case 'f': ipkg->broken_files = 1; break;
				case 's': ipkg->broken_script = 1; break;
				case 'x': ipkg->broken_xattr = 1; break;
				case 'S': ipkg->sha256_160 = 1; break;
				default:
					if (!(db->ctx->force & APK_FORCE_OLD_APK))
						goto old_apk_tools;
				}
			}
			break;
		default:
			if (r != 0 && !(db->ctx->force & APK_FORCE_OLD_APK))
				goto old_apk_tools;
			/* Installed. So mark the package as installable. */
			pkg->filename = NULL;
			continue;
		}
		if (APK_BLOB_IS_NULL(l)) goto bad_entry;
	}
	return apk_istream_close(is);
old_apk_tools:
	/* Installed db should not have unsupported fields */
	apk_err(out, "This apk-tools is too old to handle installed packages");
	goto err_fmt;
bad_entry:
	apk_err(out, "FDB format error (line %d, entry '%c')", lineno, field);
err_fmt:
	is->err = -APKE_V2DB_FORMAT;
	return apk_istream_close(is);
}

int apk_db_index_read(struct apk_database *db, struct apk_istream *is, int repo)
{
	return apk_db_fdb_read(db, is, repo, 0);
}

static void apk_blob_push_db_acl(apk_blob_t *b, char field, struct apk_db_acl *acl)
{
	char hdr[2] = { field, ':' };

	apk_blob_push_blob(b, APK_BLOB_BUF(hdr));
	apk_blob_push_uint(b, acl->uid, 10);
	apk_blob_push_blob(b, APK_BLOB_STR(":"));
	apk_blob_push_uint(b, acl->gid, 10);
	apk_blob_push_blob(b, APK_BLOB_STR(":"));
	apk_blob_push_uint(b, acl->mode, 8);
	if (acl->xattr_csum.type != APK_CHECKSUM_NONE) {
		apk_blob_push_blob(b, APK_BLOB_STR(":"));
		apk_blob_push_csum(b, &acl->xattr_csum);
	}
	apk_blob_push_blob(b, APK_BLOB_STR("\n"));
}

static int apk_db_fdb_write(struct apk_database *db, struct apk_installed_package *ipkg, struct apk_ostream *os)
{
	struct apk_package *pkg = ipkg->pkg;
	struct apk_db_dir_instance *diri;
	struct apk_db_file *file;
	struct hlist_node *c1, *c2;
	char buf[1024+PATH_MAX];
	apk_blob_t bbuf = APK_BLOB_BUF(buf);
	int r = 0;

	if (IS_ERR(os)) return PTR_ERR(os);

	r = apk_pkg_write_index_header(pkg, os);
	if (r < 0) goto err;

	if (ipkg->replaces->num) {
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("r:"));
		apk_blob_push_deps(&bbuf, db, ipkg->replaces);
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));
	}
	if (ipkg->replaces_priority) {
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("q:"));
		apk_blob_push_uint(&bbuf, ipkg->replaces_priority, 10);
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));
	}
	if (ipkg->repository_tag) {
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("s:"));
		apk_blob_push_blob(&bbuf, db->repo_tags[ipkg->repository_tag].plain_name);
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));
	}
	if (ipkg->broken_files || ipkg->broken_script || ipkg->broken_xattr || ipkg->sha256_160) {
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("f:"));
		if (ipkg->broken_files)
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("f"));
		if (ipkg->broken_script)
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("s"));
		if (ipkg->broken_xattr)
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("x"));
		if (ipkg->sha256_160)
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("S"));
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));
	}
	hlist_for_each_entry(diri, c1, &ipkg->owned_dirs, pkg_dirs_list) {
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("F:"));
		apk_blob_push_blob(&bbuf, APK_BLOB_PTR_LEN(diri->dir->name, diri->dir->namelen));
		apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));

		if (diri->acl != apk_default_acl_dir)
			apk_blob_push_db_acl(&bbuf, 'M', diri->acl);

		bbuf = apk_blob_pushed(APK_BLOB_BUF(buf), bbuf);
		if (APK_BLOB_IS_NULL(bbuf)) {
			r = -ENOBUFS;
			goto err;
		}
		r = apk_ostream_write(os, bbuf.ptr, bbuf.len);
		if (r < 0) goto err;
		bbuf = APK_BLOB_BUF(buf);

		hlist_for_each_entry(file, c2, &diri->owned_files, diri_files_list) {
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("R:"));
			apk_blob_push_blob(&bbuf, APK_BLOB_PTR_LEN(file->name, file->namelen));
			apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));

			if (file->acl != apk_default_acl_file)
				apk_blob_push_db_acl(&bbuf, 'a', file->acl);

			if (file->csum.type != APK_CHECKSUM_NONE) {
				apk_blob_push_blob(&bbuf, APK_BLOB_STR("Z:"));
				apk_blob_push_csum(&bbuf, &file->csum);
				apk_blob_push_blob(&bbuf, APK_BLOB_STR("\n"));
			}

			bbuf = apk_blob_pushed(APK_BLOB_BUF(buf), bbuf);
			if (APK_BLOB_IS_NULL(bbuf)) {
				r = -ENOBUFS;
				goto err;
			}
			r = apk_ostream_write(os, bbuf.ptr, bbuf.len);
			if (r < 0) goto err;
			bbuf = APK_BLOB_BUF(buf);
		}
	}
	r = apk_ostream_write(os, "\n", 1);
err:
	if (r < 0) apk_ostream_cancel(os, r);
	return r;
}

static int apk_db_scriptdb_write(struct apk_database *db, struct apk_installed_package *ipkg, struct apk_ostream *os)
{
	struct apk_package *pkg = ipkg->pkg;
	struct apk_file_info fi;
	char filename[256];
	apk_blob_t bfn;
	int r, i;

	if (IS_ERR(os)) return PTR_ERR(os);

	for (i = 0; i < APK_SCRIPT_MAX; i++) {
		if (!ipkg->script[i].ptr) continue;

		fi = (struct apk_file_info) {
			.name = filename,
			.size = ipkg->script[i].len,
			.mode = 0755 | S_IFREG,
			.mtime = pkg->build_time,
		};
		/* The scripts db expects file names in format:
		 * pkg-version.<hexdump of package checksum>.action */
		bfn = APK_BLOB_BUF(filename);
		apk_blob_push_blob(&bfn, APK_BLOB_STR(pkg->name->name));
		apk_blob_push_blob(&bfn, APK_BLOB_STR("-"));
		apk_blob_push_blob(&bfn, *pkg->version);
		apk_blob_push_blob(&bfn, APK_BLOB_STR("."));
		apk_blob_push_csum(&bfn, &pkg->csum);
		apk_blob_push_blob(&bfn, APK_BLOB_STR("."));
		apk_blob_push_blob(&bfn, APK_BLOB_STR(apk_script_types[i]));
		apk_blob_push_blob(&bfn, APK_BLOB_PTR_LEN("", 1));

		r = apk_tar_write_entry(os, &fi, ipkg->script[i].ptr);
		if (r < 0) {
			apk_ostream_cancel(os, -APKE_V2DB_FORMAT);
			break;
		}
	}

	return r;
}

static int apk_read_script_archive_entry(void *ctx,
					 const struct apk_file_info *ae,
					 struct apk_istream *is)
{
	struct apk_database *db = (struct apk_database *) ctx;
	struct apk_package *pkg;
	char *fncsum, *fnaction;
	struct apk_checksum csum;
	apk_blob_t blob;
	int type;

	if (!S_ISREG(ae->mode))
		return 0;

	/* The scripts db expects file names in format:
	 * pkgname-version.<hexdump of package checksum>.action */
	fnaction = memrchr(ae->name, '.', strlen(ae->name));
	if (fnaction == NULL || fnaction == ae->name)
		return 0;
	fncsum = memrchr(ae->name, '.', fnaction - ae->name - 1);
	if (fncsum == NULL)
		return 0;
	fnaction++;
	fncsum++;

	/* Parse it */
	type = apk_script_type(fnaction);
	if (type == APK_SCRIPT_INVALID)
		return 0;
	blob = APK_BLOB_PTR_PTR(fncsum, fnaction - 2);
	apk_blob_pull_csum(&blob, &csum);

	/* Attach script */
	pkg = apk_db_get_pkg(db, &csum);
	if (pkg != NULL && pkg->ipkg != NULL)
		apk_ipkg_add_script(pkg->ipkg, is, type, ae->size);

	return 0;
}

static int parse_triggers(void *ctx, apk_blob_t blob)
{
	struct apk_installed_package *ipkg = ctx;

	if (blob.len == 0)
		return 0;

	*apk_string_array_add(&ipkg->triggers) = apk_blob_cstr(blob);
	return 0;
}

static int apk_db_triggers_write(struct apk_database *db, struct apk_installed_package *ipkg, struct apk_ostream *os)
{
	char buf[APK_BLOB_CHECKSUM_BUF];
	apk_blob_t bfn;
	char **trigger;

	if (IS_ERR(os)) return PTR_ERR(os);
	if (!ipkg->triggers || ipkg->triggers->num == 0) return 0;

	bfn = APK_BLOB_BUF(buf);
	apk_blob_push_csum(&bfn, &ipkg->pkg->csum);
	bfn = apk_blob_pushed(APK_BLOB_BUF(buf), bfn);
	apk_ostream_write(os, bfn.ptr, bfn.len);

	foreach_array_item(trigger, ipkg->triggers) {
		apk_ostream_write(os, " ", 1);
		apk_ostream_write_string(os, *trigger);
	}
	apk_ostream_write(os, "\n", 1);
	return 0;
}

static int apk_db_triggers_read(struct apk_database *db, struct apk_istream *is)
{
	struct apk_checksum csum;
	struct apk_package *pkg;
	struct apk_installed_package *ipkg;
	apk_blob_t l;

	if (IS_ERR(is)) return PTR_ERR(is);

	while (apk_istream_get_delim(is, APK_BLOB_STR("\n"), &l) == 0) {
		apk_blob_pull_csum(&l, &csum);
		apk_blob_pull_char(&l, ' ');

		pkg = apk_db_get_pkg(db, &csum);
		if (pkg == NULL || pkg->ipkg == NULL)
			continue;

		ipkg = pkg->ipkg;
		apk_blob_for_each_segment(l, " ", parse_triggers, ipkg);
		if (ipkg->triggers->num != 0 &&
		    !list_hashed(&ipkg->trigger_pkgs_list))
			list_add_tail(&ipkg->trigger_pkgs_list,
				      &db->installed.triggers);
	}
	return apk_istream_close(is);
}

static int apk_db_read_layer(struct apk_database *db, unsigned layer)
{
	apk_blob_t blob, world;
	int r, fd, ret = 0, flags = db->ctx->open_flags;

	/* Read:
	 * 1. world
	 * 2. installed packages db
	 * 3. triggers db
	 * 4. scripts db
	 */

	fd = openat(db->root_fd, apk_db_layer_name(layer), O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -errno;

	if (!(flags & APK_OPENF_NO_WORLD)) {
		if (layer == APK_DB_LAYER_ROOT)
			ret = apk_blob_from_file(db->root_fd, apk_world_file, &world);
		else
			ret = apk_blob_from_file(fd, "world", &world);

		if (!ret) {
			blob = apk_blob_trim(world);
			apk_blob_pull_deps(&blob, db, &db->world);
			free(world.ptr);
		} else if (layer == APK_DB_LAYER_ROOT) {
			ret = -ENOENT;
		}
	}

	if (!(flags & APK_OPENF_NO_INSTALLED)) {
		r = apk_db_fdb_read(db, apk_istream_from_file(fd, "installed"), -1, layer);
		if (!ret && r != -ENOENT) ret = r;
		r = apk_db_triggers_read(db, apk_istream_from_file(fd, "triggers"));
		if (!ret && r != -ENOENT) ret = r;
	}

	if (!(flags & APK_OPENF_NO_SCRIPTS)) {
		r = apk_tar_parse(apk_istream_from_file(fd, "scripts.tar"),
				  apk_read_script_archive_entry, db, db->id_cache);
		if (!ret && r != -ENOENT) ret = r;
	}

	close(fd);
	return ret;
}

static int apk_db_index_write_nr_cache(struct apk_database *db)
{
	struct apk_package_array *pkgs;
	struct apk_package **ppkg;
	struct apk_ostream *os;

	if (!apk_db_cache_active(db)) return 0;

	/* Write list of installed non-repository packages to
	 * cached index file */
	os = apk_ostream_to_file(db->cache_fd, "installed", 0644);
	if (IS_ERR(os)) return PTR_ERR(os);

	pkgs = apk_db_sorted_installed_packages(db);
	foreach_array_item(ppkg, pkgs) {
		struct apk_package *pkg = *ppkg;
		if ((pkg->repos == BIT(APK_REPOSITORY_CACHED) ||
		     (pkg->repos == 0 && !pkg->installed_size))) {
			if (apk_pkg_write_index_entry(pkg, os) < 0) break;
		}
	}
	return apk_ostream_close(os);
}

static int add_protected_path(void *ctx, apk_blob_t blob)
{
	struct apk_database *db = (struct apk_database *) ctx;
	int protect_mode = APK_PROTECT_NONE;

	/* skip empty lines and comments */
	if (blob.len == 0)
		return 0;

	switch (blob.ptr[0]) {
	case '#':
		return 0;
	case '-':
		protect_mode = APK_PROTECT_IGNORE;
		break;
	case '+':
		protect_mode = APK_PROTECT_CHANGED;
		break;
	case '@':
		protect_mode = APK_PROTECT_SYMLINKS_ONLY;
		break;
	case '!':
		protect_mode = APK_PROTECT_ALL;
		break;
	default:
		protect_mode = APK_PROTECT_CHANGED;
		goto no_mode_char;
	}
	blob.ptr++;
	blob.len--;

no_mode_char:
	/* skip leading and trailing path separators */
	while (blob.len && blob.ptr[0] == '/')
		blob.ptr++, blob.len--;
	while (blob.len && blob.ptr[blob.len-1] == '/')
		blob.len--;

	*apk_protected_path_array_add(&db->protected_paths) = (struct apk_protected_path) {
		.relative_pattern = apk_blob_cstr(blob),
		.protect_mode = protect_mode,
	};

	return 0;
}

static int file_ends_with_dot_list(const char *file)
{
	const char *ext = strrchr(file, '.');
	if (ext == NULL || strcmp(ext, ".list") != 0)
		return FALSE;
	return TRUE;
}

static int add_protected_paths_from_file(void *ctx, int dirfd, const char *file)
{
	struct apk_database *db = (struct apk_database *) ctx;
	apk_blob_t blob;

	if (!file_ends_with_dot_list(file))
		return 0;

	if (apk_blob_from_file(dirfd, file, &blob))
		return 0;

	apk_blob_for_each_segment(blob, "\n", add_protected_path, db);
	free(blob.ptr);

	return 0;
}

static void handle_alarm(int sig)
{
}

static void mark_in_cache(struct apk_database *db, int static_cache, int dirfd, const char *name, struct apk_package *pkg)
{
	if (pkg == NULL)
		return;

	pkg->repos |= BIT(APK_REPOSITORY_CACHED);
}

static int add_repos_from_file(void *ctx, int dirfd, const char *file)
{
	struct apk_database *db = (struct apk_database *) ctx;
	struct apk_out *out = &db->ctx->out;
	apk_blob_t blob;

	if (dirfd != AT_FDCWD && dirfd != db->root_fd) {
		/* loading from repositories.d; check extension */
		if (!file_ends_with_dot_list(file))
			return 0;
	}

	if (apk_blob_from_file(dirfd, file, &blob)) {
		if (dirfd != AT_FDCWD) return 0;
		apk_err(out, "failed to read repositories: %s", file);
		apk_msg(out, "NOTE: --repositories-file is relative to the startup directory since apk 2.12.0_rc2");
		return -ENOENT;
	}

	apk_blob_for_each_segment(blob, "\n", apk_db_add_repository, db);
	free(blob.ptr);

	return 0;
}

static void apk_db_setup_repositories(struct apk_database *db, const char *cache_dir)
{
	/* This is the SHA-1 of the string 'cache'. Repo hashes like this
	 * are truncated to APK_CACHE_CSUM_BYTES and always use SHA-1. */
	db->repos[APK_REPOSITORY_CACHED] = (struct apk_repository) {
		.url = cache_dir,
		.csum.data = {
			0xb0,0x35,0x92,0x80,0x6e,0xfa,0xbf,0xee,0xb7,0x09,
			0xf5,0xa7,0x0a,0x7c,0x17,0x26,0x69,0xb0,0x05,0x38 },
		.csum.type = APK_CHECKSUM_SHA1,
	};

	db->num_repos = APK_REPOSITORY_FIRST_CONFIGURED;
	db->local_repos |= BIT(APK_REPOSITORY_CACHED);
	db->available_repos |= BIT(APK_REPOSITORY_CACHED);

	db->num_repo_tags = 1;
}

static int apk_db_name_rdepends(apk_hash_item item, void *pctx)
{
	struct apk_name *name = item, *rname;
	struct apk_provider *p;
	struct apk_dependency *dep;
	struct apk_name *touched[128];
	unsigned num_virtual = 0, num_touched = 0;

	foreach_array_item(p, name->providers) {
		num_virtual += (p->pkg->name != name);
		foreach_array_item(dep, p->pkg->depends) {
			rname = dep->name;
			rname->is_dependency |= !apk_dep_conflict(dep);
			if (!(rname->state_int & 1)) {
				if (!rname->state_int) {
					if (num_touched < ARRAY_SIZE(touched))
						touched[num_touched] = rname;
					num_touched++;
				}
				rname->state_int |= 1;
				*apk_name_array_add(&rname->rdepends) = name;
			}
		}
		foreach_array_item(dep, p->pkg->install_if) {
			rname = dep->name;
			if (!(rname->state_int & 2)) {
				if (!rname->state_int) {
					if (num_touched < ARRAY_SIZE(touched))
						touched[num_touched] = rname;
					num_touched++;
				}
				rname->state_int |= 2;
				*apk_name_array_add(&rname->rinstall_if) = name;
			}
		}
	}
	if (num_virtual == 0)
		name->priority = 0;
	else if (num_virtual != name->providers->num)
		name->priority = 1;
	else
		name->priority = 2;

	if (num_touched > ARRAY_SIZE(touched)) {
		foreach_array_item(p, name->providers) {
			foreach_array_item(dep, p->pkg->depends)
				dep->name->state_int = 0;
			foreach_array_item(dep, p->pkg->install_if)
				dep->name->state_int = 0;
		}
	} else for (unsigned i = 0; i < num_touched; i++)
		touched[i]->state_int = 0;

	return 0;
}

static inline int setup_static_cache(struct apk_database *db, struct apk_ctx *ac)
{
	db->cache_dir = apk_static_cache_dir;
	db->cache_fd = openat(db->root_fd, db->cache_dir, O_RDONLY | O_CLOEXEC);
	if (db->cache_fd < 0) {
		apk_make_dirs(db->root_fd, db->cache_dir, 0755, 0755);
		db->cache_fd = openat(db->root_fd, db->cache_dir, O_RDONLY | O_CLOEXEC);
		if (db->cache_fd < 0) {
			if (ac->open_flags & APK_OPENF_WRITE) return -EROFS;
			db->cache_fd = -APKE_CACHE_NOT_AVAILABLE;
		}
	}

	return 0;
}

#ifdef __linux__
static int detect_tmpfs_root(struct apk_database *db)
{
	struct statfs stfs;

	return fstatfs(db->root_fd, &stfs) == 0 && stfs.f_type == TMPFS_MAGIC;
}

static unsigned long map_statfs_flags(unsigned long f_flag)
{
	unsigned long mnt_flags = 0;
	if (f_flag & ST_RDONLY) mnt_flags |= MS_RDONLY;
	if (f_flag & ST_NOSUID) mnt_flags |= MS_NOSUID;
	if (f_flag & ST_NODEV)  mnt_flags |= MS_NODEV;
	if (f_flag & ST_NOEXEC) mnt_flags |= MS_NOEXEC;
	if (f_flag & ST_NOATIME) mnt_flags |= MS_NOATIME;
	if (f_flag & ST_NODIRATIME)mnt_flags |= MS_NODIRATIME;
#ifdef ST_RELATIME
	if (f_flag & ST_RELATIME) mnt_flags |= MS_RELATIME;
#endif
	if (f_flag & ST_SYNCHRONOUS) mnt_flags |= MS_SYNCHRONOUS;
	if (f_flag & ST_MANDLOCK) mnt_flags |= ST_MANDLOCK;
	return mnt_flags;
}

static char *find_mountpoint(int atfd, const char *rel_path)
{
	struct mntent *me;
	struct stat st;
	FILE *f;
	char *ret = NULL;
	dev_t dev;

	if (fstatat(atfd, rel_path, &st, 0) != 0)
		return NULL;
	dev = st.st_dev;

	f = setmntent("/proc/mounts", "r");
	if (f == NULL)
		return NULL;
	while ((me = getmntent(f)) != NULL) {
		if (strcmp(me->mnt_fsname, "rootfs") == 0)
			continue;
		if (fstatat(atfd, me->mnt_dir, &st, 0) == 0 &&
		    st.st_dev == dev) {
			ret = strdup(me->mnt_dir);
			break;
		}
	}
	endmntent(f);

	return ret;
}

static int setup_cache(struct apk_database *db, struct apk_ctx *ac)
{
	struct apk_out *out = &ac->out;
	int fd;
	struct statfs stfs;

	fd = openat(db->root_fd, ac->cache_dir, O_RDONLY | O_CLOEXEC);
	if (fd >= 0 && fstatfs(fd, &stfs) == 0) {
		db->cache_dir = ac->cache_dir;
		db->cache_fd = fd;
		db->cache_remount_flags = map_statfs_flags(stfs.f_flags);
		if ((ac->open_flags & (APK_OPENF_WRITE | APK_OPENF_CACHE_WRITE)) &&
		    (db->cache_remount_flags & MS_RDONLY) != 0) {
			/* remount cache read/write */
			db->cache_remount_dir = find_mountpoint(db->root_fd, db->cache_dir);
			if (db->cache_remount_dir == NULL) {
				apk_warn(out, "Unable to find cache directory mount point");
			} else if (mount(0, db->cache_remount_dir, 0, MS_REMOUNT | (db->cache_remount_flags & ~MS_RDONLY), 0) != 0) {
				free(db->cache_remount_dir);
				db->cache_remount_dir = NULL;
				return -EROFS;
			}
		}
	} else {
		if (fd >= 0) close(fd);
		if (setup_static_cache(db, ac) < 0) return -EROFS;
	}

	return 0;
}

static void remount_cache(struct apk_database *db)
{
	if (db->cache_remount_dir) {
		mount(0, db->cache_remount_dir, 0, MS_REMOUNT | db->cache_remount_flags, 0);
		free(db->cache_remount_dir);
		db->cache_remount_dir = NULL;
	}
}

static int mount_proc(struct apk_database *db)
{
	struct statfs stfs;

	/* mount /proc */
	if (asprintf(&db->root_proc_dir, "%s/proc", db->ctx->root) == -1)
		return -1;
	if (statfs(db->root_proc_dir, &stfs) != 0) {
		if (errno == ENOENT) mkdir(db->root_proc_dir, 0555);
		stfs.f_type = 0;
	}
	if (stfs.f_type != PROC_SUPER_MAGIC) {
		mount("proc", db->root_proc_dir, "proc", 0, 0);
	} else {
		/* was already mounted. prevent umount on close */
		free(db->root_proc_dir);
		db->root_proc_dir = NULL;
	}

	return 0;
}

static void unmount_proc(struct apk_database *db)
{
	if (db->root_proc_dir) {
		umount2(db->root_proc_dir, MNT_DETACH|UMOUNT_NOFOLLOW);
		free(db->root_proc_dir);
		db->root_proc_dir = NULL;
	}
}
#else
static int detect_tmpfs_root(struct apk_database *db)
{
	(void) db;
	return 0;
}

static int setup_cache(struct apk_database *db, struct apk_ctx *ac)
{
	return setup_static_cache(db, ac);
}

static void remount_cache(struct apk_database *db)
{
	(void) db;
}

static int mount_proc(struct apk_database *db)
{
	(void) db;
	return 0;
}

static void unmount_proc(struct apk_database *db)
{
	(void) db;
}
#endif

const char *apk_db_layer_name(int layer)
{
	switch (layer) {
	case APK_DB_LAYER_ROOT: return "lib/apk/db";
	case APK_DB_LAYER_UVOL: return "lib/apk/db-uvol";
	default:
		assert("invalid layer");
		return 0;
	}
}

void apk_db_init(struct apk_database *db)
{
	memset(db, 0, sizeof(*db));
	apk_hash_init(&db->available.names, &pkg_name_hash_ops, 20000);
	apk_hash_init(&db->available.packages, &pkg_info_hash_ops, 10000);
	apk_hash_init(&db->installed.dirs, &dir_hash_ops, 20000);
	apk_hash_init(&db->installed.files, &file_hash_ops, 200000);
	apk_atom_init(&db->atoms);
	list_init(&db->installed.packages);
	list_init(&db->installed.triggers);
	apk_dependency_array_init(&db->world);
	apk_protected_path_array_init(&db->protected_paths);
	apk_name_array_init(&db->available.sorted_names);
	apk_package_array_init(&db->installed.sorted_packages);
	db->permanent = 1;
	db->root_fd = -1;
}

int apk_db_open(struct apk_database *db, struct apk_ctx *ac)
{
	struct apk_out *out = &ac->out;
	const char *msg = NULL;
	apk_blob_t blob;
	int r = -1, i;

	apk_default_acl_dir = apk_db_acl_atomize(db, 0755, 0, 0);
	apk_default_acl_file = apk_db_acl_atomize(db, 0644, 0, 0);

	db->ctx = ac;
	if (ac->open_flags == 0) {
		msg = "Invalid open flags (internal error)";
		goto ret_r;
	}
	if ((ac->open_flags & APK_OPENF_WRITE) &&
	    !(ac->open_flags & APK_OPENF_NO_AUTOUPDATE) &&
	    !(ac->flags & APK_NO_NETWORK))
		db->autoupdate = 1;

	apk_db_setup_repositories(db, ac->cache_dir);
	db->root_fd = apk_ctx_fd_root(ac);
	db->cache_fd = -APKE_CACHE_NOT_AVAILABLE;
	db->permanent = !detect_tmpfs_root(db);
	db->usermode = !!(ac->open_flags & APK_OPENF_USERMODE);

	if (!(ac->open_flags & APK_OPENF_CREATE)) {
		// Autodetect usermode from the installeddb owner
		struct stat st;
		if (fstatat(db->root_fd, apk_db_layer_name(APK_DB_LAYER_ROOT), &st, 0) == 0 &&
		    st.st_uid != 0)
			db->usermode = 1;
	}
	if (db->usermode) db->extract_flags |= APK_FSEXTRACTF_NO_CHOWN | APK_FSEXTRACTF_NO_SYS_XATTRS;

	if (ac->arch && (ac->root_set || (ac->open_flags & APK_OPENF_ALLOW_ARCH))) {
		db->arch = apk_atomize(&db->atoms, APK_BLOB_STR(ac->arch));
		db->write_arch = ac->root_set;
	} else {
		apk_blob_t arch;
		if (!apk_blob_from_file(db->root_fd, apk_arch_file, &arch)) {
			db->arch = apk_atomize_dup(&db->atoms, apk_blob_trim(arch));
			free(arch.ptr);
		} else {
			db->arch = apk_atomize(&db->atoms, APK_BLOB_STR(APK_DEFAULT_ARCH));
			db->write_arch = 1;
		}
	}

	db->id_cache = apk_ctx_get_id_cache(ac);

	if (ac->open_flags & APK_OPENF_WRITE) {
		msg = "Unable to lock database";
		db->lock_fd = openat(db->root_fd, apk_lock_file,
				     O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (db->lock_fd < 0) {
			if (!(ac->open_flags & APK_OPENF_CREATE))
				goto ret_errno;
		} else if (flock(db->lock_fd, LOCK_EX | LOCK_NB) < 0) {
			struct sigaction sa, old_sa;

			if (!ac->lock_wait) goto ret_errno;

			apk_msg(out, "Waiting for repository lock");
			memset(&sa, 0, sizeof sa);
			sa.sa_handler = handle_alarm;
			sa.sa_flags   = SA_RESETHAND;
			sigaction(SIGALRM, &sa, &old_sa);

			alarm(ac->lock_wait);
			if (flock(db->lock_fd, LOCK_EX) < 0)
				goto ret_errno;

			alarm(0);
			sigaction(SIGALRM, &old_sa, NULL);
		}

		if (mount_proc(db) < 0)
			goto ret_errno;
	}

	if (!APK_BLOB_IS_NULL(ac->protected_paths)) {
		apk_blob_for_each_segment(ac->protected_paths, "\n", add_protected_path, db);
	} else {
		blob = APK_BLOB_STR("+etc\n" "@etc/init.d\n" "!etc/apk\n");
		apk_blob_for_each_segment(blob, "\n", add_protected_path, db);

		apk_dir_foreach_file(openat(db->root_fd, "etc/apk/protected_paths.d", O_RDONLY | O_CLOEXEC),
				     add_protected_paths_from_file, db);
	}

	/* figure out where to have the cache */
	if (!(db->ctx->flags & APK_NO_CACHE)) {
		if ((r = setup_cache(db, ac)) < 0) {
			apk_err(out, "Unable to setup the cache");
			goto ret_r;
		}
	}

	if (db->ctx->flags & APK_OVERLAY_FROM_STDIN) {
		db->ctx->flags &= ~APK_OVERLAY_FROM_STDIN;
		apk_db_read_overlay(db, apk_istream_from_fd(STDIN_FILENO));
	}

	if ((db->ctx->open_flags & APK_OPENF_NO_STATE) != APK_OPENF_NO_STATE) {
		for (i = 0; i < APK_DB_LAYER_NUM; i++) {
			r = apk_db_read_layer(db, i);
			if (r) {
				if (i != APK_DB_LAYER_ROOT) continue;
				if (!(r == -ENOENT && (ac->open_flags & APK_OPENF_CREATE))) {
					msg = "Unable to read database";
					goto ret_r;
				}
			}
			db->active_layers |= BIT(i);
		}
	}

	if (!(ac->open_flags & APK_OPENF_NO_INSTALLED_REPO)) {
		if (apk_db_cache_active(db)) {
			apk_db_index_read(db, apk_istream_from_file(db->cache_fd, "installed"), -2);
		}
	}

	if (!(ac->open_flags & APK_OPENF_NO_CMDLINE_REPOS)) {
		char **repo;
		foreach_array_item(repo, ac->repository_list)
			apk_db_add_repository(db, APK_BLOB_STR(*repo));
	}

	if (!(ac->open_flags & APK_OPENF_NO_SYS_REPOS)) {
		if (ac->repositories_file == NULL) {
			add_repos_from_file(db, db->root_fd, "etc/apk/repositories");
			apk_dir_foreach_file(openat(db->root_fd, "etc/apk/repositories.d", O_RDONLY | O_CLOEXEC),
					     add_repos_from_file, db);
		} else {
			add_repos_from_file(db, AT_FDCWD, ac->repositories_file);
		}

		if (db->repositories.updated > 0)
			apk_db_index_write_nr_cache(db);
	}

	apk_hash_foreach(&db->available.names, apk_db_name_rdepends, db);

	if (apk_db_cache_active(db) && (ac->open_flags & (APK_OPENF_NO_REPOS|APK_OPENF_NO_INSTALLED)) == 0)
		apk_db_cache_foreach_item(db, mark_in_cache, 0);

	db->open_complete = 1;

	if (db->compat_newfeatures) {
		apk_warn(out,
			"This apk-tools is OLD! Some packages %s.",
			db->compat_notinstallable ? "are not installable" : "might not function properly");
	}

	ac->db = db;
	return 0;

ret_errno:
	r = -errno;
ret_r:
	if (msg != NULL)
		apk_err(out, "%s: %s", msg, apk_error_str(-r));
	apk_db_close(db);

	return r;
}

struct write_ctx {
	struct apk_database *db;
	int fd;
};

static int apk_db_write_layers(struct apk_database *db)
{
	struct layer_data {
		int fd;
		struct apk_ostream *installed, *scripts, *triggers;
	} layers[APK_DB_LAYER_NUM] = {0};
	struct apk_ostream *os;
	struct apk_package **ppkg;
	struct apk_package_array *pkgs;
	int i, r, rr = 0;

	for (i = 0; i < APK_DB_LAYER_NUM; i++) {
		struct layer_data *ld = &layers[i];
		if (!(db->active_layers & BIT(i))) continue;

		ld->fd = openat(db->root_fd, apk_db_layer_name(i), O_RDONLY | O_CLOEXEC);
		if (ld->fd < 0) {
			if (i == 0) return -errno;
			continue;
		}
		ld->installed = apk_ostream_to_file(ld->fd, "installed", 0644);
		ld->scripts   = apk_ostream_to_file(ld->fd, "scripts.tar", 0644);
		ld->triggers  = apk_ostream_to_file(ld->fd, "triggers", 0644);

		if (i == 0)
			os = apk_ostream_to_file(db->root_fd, apk_world_file, 0644);
		else
			os = apk_ostream_to_file(ld->fd, "world", 0644);
		if (IS_ERR(os)) {
			if (!rr) rr = PTR_ERR(os);
			continue;
		}
		apk_deps_write_layer(db, db->world, os, APK_BLOB_PTR_LEN("\n", 1), i);
		apk_ostream_write(os, "\n", 1);
		r = apk_ostream_close(os);
		if (!rr) rr = r;
	}

	pkgs = apk_db_sorted_installed_packages(db);
	foreach_array_item(ppkg, pkgs) {
		struct apk_package *pkg = *ppkg;
		struct layer_data *ld = &layers[pkg->layer];
		if (!ld->fd) continue;
		apk_db_fdb_write(db, pkg->ipkg, ld->installed);
		apk_db_scriptdb_write(db, pkg->ipkg, ld->scripts);
		apk_db_triggers_write(db, pkg->ipkg, ld->triggers);
	}

	for (i = 0; i < APK_DB_LAYER_NUM; i++) {
		struct layer_data *ld = &layers[i];
		if (!(db->active_layers & BIT(i))) continue;

		if (!IS_ERR(ld->installed))
			r = apk_ostream_close(ld->installed);
		else	r = PTR_ERR(ld->installed);
		if (!rr) rr = r;

		if (!IS_ERR(ld->scripts)) {
			apk_tar_write_entry(ld->scripts, NULL, NULL);
			r = apk_ostream_close(ld->scripts);
		} else	r = PTR_ERR(ld->scripts);
		if (!rr) rr = r;

		if (!IS_ERR(ld->triggers))
			r = apk_ostream_close(ld->triggers);
		else	r = PTR_ERR(ld->triggers);
		if (!rr) rr = r;

		close(ld->fd);
	}
	return rr;
}

int apk_db_write_config(struct apk_database *db)
{
	struct apk_out *out = &db->ctx->out;
	int r, rr = 0;

	if ((db->ctx->flags & APK_SIMULATE) || db->ctx->root == NULL)
		return 0;

	if (db->ctx->open_flags & APK_OPENF_CREATE) {
		apk_make_dirs(db->root_fd, "lib/apk/db", 0755, 0755);
		apk_make_dirs(db->root_fd, "etc/apk", 0755, 0755);
	} else if (db->lock_fd == 0) {
		apk_err(out, "Refusing to write db without write lock!");
		return -1;
	}

	if (db->write_arch)
		apk_blob_to_file(db->root_fd, apk_arch_file, *db->arch, APK_BTF_ADD_EOL);

	r = apk_db_write_layers(db);
	if (!rr ) rr = r;

	r = apk_db_index_write_nr_cache(db);
	if (r < 0 && !rr) rr = r;

	if (rr) {
		apk_err(out, "System state may be inconsistent: failed to write database: %s",
			apk_error_str(rr));
	}
	return rr;
}

void apk_db_close(struct apk_database *db)
{
	struct apk_installed_package *ipkg;
	struct apk_db_dir_instance *diri;
	struct apk_protected_path *ppath;
	struct hlist_node *dc, *dn;
	int i;

	/* Cleaning up the directory tree will cause mode, uid and gid
	 * of all modified (package providing that directory got removed)
	 * directories to be reset. */
	list_for_each_entry(ipkg, &db->installed.packages, installed_pkgs_list) {
		hlist_for_each_entry_safe(diri, dc, dn, &ipkg->owned_dirs, pkg_dirs_list) {
			apk_db_diri_free(db, diri, APK_DIR_FREE);
		}
	}

	for (i = APK_REPOSITORY_FIRST_CONFIGURED; i < db->num_repos; i++) {
		free((void*) db->repos[i].url);
		free(db->repos[i].description.ptr);
	}
	foreach_array_item(ppath, db->protected_paths)
		free(ppath->relative_pattern);
	apk_protected_path_array_free(&db->protected_paths);

	apk_dependency_array_free(&db->world);

	apk_name_array_free(&db->available.sorted_names);
	apk_package_array_free(&db->installed.sorted_packages);
	apk_hash_free(&db->available.packages);
	apk_hash_free(&db->available.names);
	apk_hash_free(&db->installed.files);
	apk_hash_free(&db->installed.dirs);
	apk_atom_free(&db->atoms);

	unmount_proc(db);
	remount_cache(db);

	if (db->cache_fd > 0) close(db->cache_fd);
	if (db->lock_fd > 0) close(db->lock_fd);
}

int apk_db_get_tag_id(struct apk_database *db, apk_blob_t tag)
{
	int i;

	if (APK_BLOB_IS_NULL(tag))
		return APK_DEFAULT_REPOSITORY_TAG;

	if (tag.ptr[0] == '@') {
		for (i = 1; i < db->num_repo_tags; i++)
			if (apk_blob_compare(db->repo_tags[i].tag, tag) == 0)
				return i;
	} else {
		for (i = 1; i < db->num_repo_tags; i++)
			if (apk_blob_compare(db->repo_tags[i].plain_name, tag) == 0)
				return i;
	}
	if (i >= ARRAY_SIZE(db->repo_tags))
		return -1;

	db->num_repo_tags++;

	if (tag.ptr[0] == '@') {
		db->repo_tags[i].tag = *apk_atomize_dup(&db->atoms, tag);
	} else {
		char *tmp = alloca(tag.len + 1);
		tmp[0] = '@';
		memcpy(&tmp[1], tag.ptr, tag.len);
		db->repo_tags[i].tag = *apk_atomize_dup(&db->atoms, APK_BLOB_PTR_LEN(tmp, tag.len+1));
	}

	db->repo_tags[i].plain_name = db->repo_tags[i].tag;
	apk_blob_pull_char(&db->repo_tags[i].plain_name, '@');

	return i;
}

static int fire_triggers(apk_hash_item item, void *ctx)
{
	struct apk_database *db = (struct apk_database *) ctx;
	struct apk_db_dir *dbd = (struct apk_db_dir *) item;
	struct apk_installed_package *ipkg;
	int i;

	list_for_each_entry(ipkg, &db->installed.triggers, trigger_pkgs_list) {
		if (!ipkg->run_all_triggers && !dbd->modified)
			continue;

		for (i = 0; i < ipkg->triggers->num; i++) {
			if (ipkg->triggers->item[i][0] != '/')
				continue;

			if (fnmatch(ipkg->triggers->item[i], dbd->rooted_name,
				    FNM_PATHNAME) != 0)
				continue;

			/* And place holder for script name */
			if (ipkg->pending_triggers->num == 0) {
				*apk_string_array_add(&ipkg->pending_triggers) =
					NULL;
				db->pending_triggers++;
			}
			*apk_string_array_add(&ipkg->pending_triggers) =
				dbd->rooted_name;
			break;
		}
	}

	return 0;
}

int apk_db_fire_triggers(struct apk_database *db)
{
	apk_hash_foreach(&db->installed.dirs, fire_triggers, db);
	return db->pending_triggers;
}

int apk_db_run_script(struct apk_database *db, char *fn, char **argv)
{
	char buf[APK_EXIT_STATUS_MAX_SIZE];
	struct apk_out *out = &db->ctx->out;
	int status;
	pid_t pid;
	static char * const clean_environment[] = {
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		NULL
	};

	pid = fork();
	if (pid == -1) {
		apk_err(out, "%s: fork: %s", basename(fn), strerror(errno));
		return -2;
	}
	if (pid == 0) {
		umask(0022);

		if (fchdir(db->root_fd) != 0) {
			apk_err(out, "%s: fchdir: %s", basename(fn), strerror(errno));
			exit(127);
		}

		if (!(db->ctx->flags & APK_NO_CHROOT) && chroot(".") != 0) {
			apk_err(out, "%s: chroot: %s", basename(fn), strerror(errno));
			exit(127);
		}

		execve(fn, argv, (db->ctx->flags & APK_PRESERVE_ENV) ? environ : clean_environment);
		exit(127); /* should not get here */
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);

	if (apk_exit_status_str(status, buf, sizeof buf)) {
		apk_err(out, "%s: script %s", basename(fn), buf);
		return -1;
	}
	return 0;
}

struct update_permissions_ctx {
	struct apk_database *db;
	unsigned int errors;
};

static int update_permissions(apk_hash_item item, void *pctx)
{
	struct update_permissions_ctx *ctx = pctx;
	struct apk_database *db = ctx->db;
	struct apk_db_dir *dir = (struct apk_db_dir *) item;
	struct apk_db_acl *acl;
	struct apk_fsdir d;

	if (dir->refs == 0) return 0;
	if (!dir->permissions_stale) return 0;

	acl = dir->owner->acl;
	apk_fsdir_get(&d, APK_BLOB_PTR_LEN(dir->name, dir->namelen), db->ctx, APK_BLOB_NULL);
	if (apk_fsdir_update_perms(&d, apk_db_dir_get_mode(db, acl->mode), acl->uid, acl->gid) == 0) {
		dir->modified = 1;
		dir->permissions_stale = 0;
	} else {
		ctx->errors++;
	}
	return 0;
}

int apk_db_update_directory_permissions(struct apk_database *db)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_installed_package *ipkg;
	struct apk_db_dir_instance *diri;
	struct hlist_node *dc;
	struct update_permissions_ctx ctx = {
		.db = db,
	};

	if (db->dirowner_stale) {
		list_for_each_entry(ipkg, &db->installed.packages, installed_pkgs_list) {
			hlist_for_each_entry(diri, dc, &ipkg->owned_dirs, pkg_dirs_list)
				apk_db_dir_apply_diri_permissions(db, diri);
		}
		db->dirowner_stale = 0;
	}
	if (db->dirperms_stale) {
		if (!(db->ctx->flags & APK_SIMULATE))
			apk_hash_foreach(&db->installed.dirs, update_permissions, &ctx);
		db->dirperms_stale = 0;
	}
	if (ctx.errors) apk_err(out, "%d errors updating directory permissions", ctx.errors);
	return ctx.errors;
}

int apk_db_cache_active(struct apk_database *db)
{
	return db->cache_fd > 0 && db->cache_dir != apk_static_cache_dir;
}

struct foreach_cache_item_ctx {
	struct apk_database *db;
	apk_cache_item_cb cb;
	int static_cache;
};

static int foreach_cache_file(void *pctx, int dirfd, const char *name)
{
	struct foreach_cache_item_ctx *ctx = (struct foreach_cache_item_ctx *) pctx;
	struct apk_database *db = ctx->db;
	struct apk_package *pkg = NULL;
	struct apk_provider *p0;
	apk_blob_t b = APK_BLOB_STR(name), bname, bver;

	if (apk_pkg_parse_name(b, &bname, &bver) == 0) {
		/* Package - search for it */
		struct apk_name *name = apk_db_get_name(db, bname);
		char tmp[PATH_MAX];
		if (name == NULL)
			goto no_pkg;

		foreach_array_item(p0, name->providers) {
			if (p0->pkg->name != name)
				continue;

			apk_pkg_format_cache_pkg(APK_BLOB_BUF(tmp), p0->pkg);
			if (apk_blob_compare(b, APK_BLOB_STR(tmp)) == 0) {
				pkg = p0->pkg;
				break;
			}
		}
	}
no_pkg:
	ctx->cb(db, ctx->static_cache, dirfd, name, pkg);

	return 0;
}

int apk_db_cache_foreach_item(struct apk_database *db, apk_cache_item_cb cb, int static_cache)
{
	struct foreach_cache_item_ctx ctx = { db, cb, static_cache };

	if (static_cache) {
		struct stat st1, st2;
		int fd = openat(db->root_fd, apk_static_cache_dir, O_RDONLY | O_CLOEXEC);
		if (fd < 0) return fd;
		/* Do not handle static cache as static cache if the explicit
		 * cache is enabled at the static cache location */
		if (fstat(fd, &st1) == 0 && fstat(db->cache_fd, &st2) == 0 &&
		    st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino) {
			close(fd);
			return 0;
		}
		return apk_dir_foreach_file(fd, foreach_cache_file, &ctx);
	}
	if (db->cache_fd < 0) return db->cache_fd;
	return apk_dir_foreach_file(dup(db->cache_fd), foreach_cache_file, &ctx);
}

int apk_db_permanent(struct apk_database *db)
{
	return db->permanent;
}

int apk_db_check_world(struct apk_database *db, struct apk_dependency_array *world)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_dependency *dep;
	int bad = 0, tag;

	if (db->ctx->force & APK_FORCE_BROKEN_WORLD)
		return 0;

	foreach_array_item(dep, world) {
		tag = dep->repository_tag;
		if (tag == 0 || db->repo_tags[tag].allowed_repos != 0)
			continue;
		if (tag < 0)
			tag = 0;
		apk_warn(out, "The repository tag for world dependency '%s" BLOB_FMT "' does not exist",
			dep->name->name, BLOB_PRINTF(db->repo_tags[tag].tag));
		bad++;
	}

	return bad;
}

struct apk_package *apk_db_get_pkg(struct apk_database *db,
				   struct apk_checksum *csum)
{
	return apk_hash_get(&db->available.packages, APK_BLOB_CSUM(*csum));
}

struct apk_package *apk_db_get_file_owner(struct apk_database *db,
					  apk_blob_t filename)
{
	struct apk_db_file *dbf;
	struct apk_db_file_hash_key key;

	if (filename.len && filename.ptr[0] == '/')
		filename.len--, filename.ptr++;

	if (!apk_blob_rsplit(filename, '/', &key.dirname, &key.filename)) {
		key.dirname = APK_BLOB_NULL;
		key.filename = filename;
	}

	dbf = (struct apk_db_file *) apk_hash_get(&db->installed.files,
						  APK_BLOB_BUF(&key));
	if (dbf == NULL)
		return NULL;

	return dbf->diri->pkg;
}

unsigned int apk_db_get_pinning_mask_repos(struct apk_database *db, unsigned short pinning_mask)
{
	unsigned int repository_mask = 0;
	int i;

	for (i = 0; i < db->num_repo_tags && pinning_mask; i++) {
		if (!(BIT(i) & pinning_mask))
			continue;
		pinning_mask &= ~BIT(i);
		repository_mask |= db->repo_tags[i].allowed_repos;
	}
	return repository_mask;
}

struct apk_repository *apk_db_select_repo(struct apk_database *db,
					  struct apk_package *pkg)
{
	unsigned int repos;
	int i;

	/* Select repositories to use */
	repos = pkg->repos & db->available_repos;
	if (repos == 0)
		return NULL;

	if (repos & db->local_repos)
		repos &= db->local_repos;

	/* Pick first repository providing this package */
	for (i = APK_REPOSITORY_FIRST_CONFIGURED; i < APK_MAX_REPOS; i++) {
		if (repos & BIT(i))
			return &db->repos[i];
	}
	return &db->repos[APK_REPOSITORY_CACHED];
}

struct apkindex_ctx {
	struct apk_database *db;
	struct apk_extract_ctx ectx;
	int repo, found;
};

static int load_v2index(struct apk_extract_ctx *ectx, apk_blob_t *desc, struct apk_istream *is)
{
	struct apkindex_ctx *ctx = container_of(ectx, struct apkindex_ctx, ectx);
	struct apk_repository *repo = &ctx->db->repos[ctx->repo];

	repo->description = *desc;
	*desc = APK_BLOB_NULL;
	return apk_db_index_read(ctx->db, is, ctx->repo);
}

static int load_v3index(struct apk_extract_ctx *ectx, struct adb_obj *ndx)
{
	struct apkindex_ctx *ctx = container_of(ectx, struct apkindex_ctx, ectx);
	struct apk_database *db = ctx->db;
	struct apk_repository *repo = &db->repos[ctx->repo];
	struct apk_package *pkg;
	struct adb_obj pkgs, pkginfo;
	int i;

	repo->description = apk_blob_dup(adb_ro_blob(ndx, ADBI_NDX_DESCRIPTION));
	adb_ro_obj(ndx, ADBI_NDX_PACKAGES, &pkgs);

	for (i = ADBI_FIRST; i <= adb_ra_num(&pkgs); i++) {
		adb_ro_obj(&pkgs, i, &pkginfo);
		pkg = apk_pkg_new();
		if (!pkg) return -ENOMEM;
		apk_pkg_from_adb(db, pkg, &pkginfo);
		pkg->repos |= BIT(ctx->repo);
		if (!apk_db_pkg_add(db, pkg)) return -APKE_ADB_SCHEMA;
	}

	return 0;
}

static const struct apk_extract_ops extract_index = {
	.v2index = load_v2index,
	.v3index = load_v3index,
};

static int load_index(struct apk_database *db, struct apk_istream *is, int repo)
{
	struct apkindex_ctx ctx = {
		.db = db,
		.repo = repo,
	};
	if (IS_ERR(is)) return PTR_ERR(is);
	apk_extract_init(&ctx.ectx, db->ctx, &extract_index);
	return apk_extract(&ctx.ectx, is);
}

int apk_db_index_read_file(struct apk_database *db, const char *file, int repo)
{
	return load_index(db, apk_istream_from_file(AT_FDCWD, file), repo);
}

int apk_db_repository_check(struct apk_database *db)
{
	if (db->ctx->force & APK_FORCE_MISSING_REPOSITORIES) return 0;
	if (!db->repositories.stale && !db->repositories.unavailable) return 0;
	apk_err(&db->ctx->out,
		"Not continuing due to stale/unavailable repositories."
		"Use --force-missing-repositories to continue.");
	return -1;
}

int apk_db_add_repository(apk_database_t _db, apk_blob_t _repository)
{
	struct apk_database *db = _db.db;
	struct apk_out *out = &db->ctx->out;
	struct apk_repository *repo;
	struct apk_url_print urlp;
	apk_blob_t brepo, btag;
	int repo_num, r, tag_id = 0, atfd = AT_FDCWD, update_error = 0;
	char buf[PATH_MAX], *url;
	const char *error_action = "constructing url";

	brepo = _repository;
	btag = APK_BLOB_NULL;
	if (brepo.ptr == NULL || brepo.len == 0 || *brepo.ptr == '#')
		return 0;

	if (brepo.ptr[0] == '@') {
		apk_blob_cspn(brepo, APK_CTYPE_REPOSITORY_SEPARATOR, &btag, &brepo);
		apk_blob_spn(brepo, APK_CTYPE_REPOSITORY_SEPARATOR, NULL, &brepo);
		tag_id = apk_db_get_tag_id(db, btag);
	}

	url = apk_blob_cstr(brepo);
	for (repo_num = 0; repo_num < db->num_repos; repo_num++) {
		repo = &db->repos[repo_num];
		if (strcmp(url, repo->url) == 0) {
			db->repo_tags[tag_id].allowed_repos |=
				BIT(repo_num) & db->available_repos;
			free(url);
			return 0;
		}
	}
	if (db->num_repos >= APK_MAX_REPOS) {
		free(url);
		return -1;
	}

	repo_num = db->num_repos++;
	repo = &db->repos[repo_num];
	*repo = (struct apk_repository) {
		.url = url,
	};

	int is_remote = (apk_url_local_file(repo->url) == NULL);

	r = apk_repo_format_real_url(db->arch, repo, NULL, buf, sizeof(buf), &urlp);
	if (r != 0) goto err;

	error_action = "opening";
	apk_blob_checksum(APK_BLOB_STR(buf), apk_checksum_default(), &repo->csum);

	if (is_remote) {
		if (!(db->ctx->flags & APK_NO_NETWORK))
			db->available_repos |= BIT(repo_num);
		if (db->ctx->flags & APK_NO_CACHE) {
			error_action = "fetching";
			apk_msg(out, "fetch " URL_FMT, URL_PRINTF(urlp));
		} else {
			error_action = "opening from cache";
			if (db->autoupdate) {
				update_error = apk_cache_download(db, repo, NULL, 1, NULL, NULL);
				switch (update_error) {
				case 0:
					db->repositories.updated++;
					break;
				case -EALREADY:
					update_error = 0;
					break;
				}
			}
			r = apk_repo_format_cache_index(APK_BLOB_BUF(buf), repo);
			if (r != 0) goto err;
			atfd = db->cache_fd;
		}
	} else {
		db->local_repos |= BIT(repo_num);
		db->available_repos |= BIT(repo_num);
	}
	r = load_index(db, apk_istream_from_fd_url(atfd, buf, apk_db_url_since(db, 0)), repo_num);

err:
	if (r || update_error) {
		if (is_remote) {
			if (r) db->repositories.unavailable++;
			else db->repositories.stale++;
		}
		apk_url_parse(&urlp, repo->url);
		if (update_error)
			error_action = r ? "updating and opening" : "updating";
		else
			update_error = r;
		apk_warn(out, "%s " URL_FMT ": %s", error_action, URL_PRINTF(urlp),
			apk_error_str(update_error));
	}

	if (r != 0) {
		db->available_repos &= ~BIT(repo_num);
	} else {
		db->repo_tags[tag_id].allowed_repos |= BIT(repo_num);
	}

	return 0;
}

static void extract_cb(void *_ctx, size_t bytes_done)
{
	struct install_ctx *ctx = (struct install_ctx *) _ctx;
	if (!ctx->cb)
		return;
	ctx->cb(ctx->cb_ctx, min(ctx->installed_size + bytes_done, ctx->pkg->installed_size));
}

static void apk_db_run_pending_script(struct install_ctx *ctx)
{
	if (!ctx->script_pending) return;
	ctx->script_pending = FALSE;
	apk_ipkg_run_script(ctx->ipkg, ctx->db, ctx->script, ctx->script_args);
}

static int read_info_line(void *_ctx, apk_blob_t line)
{
	struct install_ctx *ctx = (struct install_ctx *) _ctx;
	struct apk_installed_package *ipkg = ctx->ipkg;
	struct apk_database *db = ctx->db;
	apk_blob_t l, r;

	if (line.ptr == NULL || line.len < 1 || line.ptr[0] == '#')
		return 0;

	if (!apk_blob_split(line, APK_BLOB_STR(" = "), &l, &r))
		return 0;

	if (apk_blob_compare(APK_BLOB_STR("replaces"), l) == 0) {
		apk_blob_pull_deps(&r, db, &ipkg->replaces);
	} else if (apk_blob_compare(APK_BLOB_STR("replaces_priority"), l) == 0) {
		ipkg->replaces_priority = apk_blob_pull_uint(&r, 10);
	} else if (apk_blob_compare(APK_BLOB_STR("triggers"), l) == 0) {
		apk_string_array_resize(&ipkg->triggers, 0);
		apk_blob_for_each_segment(r, " ", parse_triggers, ctx->ipkg);

		if (ctx->ipkg->triggers->num != 0 &&
		    !list_hashed(&ipkg->trigger_pkgs_list))
			list_add_tail(&ipkg->trigger_pkgs_list,
				      &db->installed.triggers);
	} else {
		apk_extract_v2_control(&ctx->ectx, l, r);
	}
	return 0;
}

static struct apk_db_dir_instance *apk_db_install_directory_entry(struct install_ctx * ctx, apk_blob_t dir)
{
	struct apk_database *db = ctx->db;
	struct apk_package *pkg = ctx->pkg;
	struct apk_installed_package *ipkg = pkg->ipkg;
	struct apk_db_dir_instance *diri;

	if (ctx->diri_node == NULL)
		ctx->diri_node = hlist_tail_ptr(&ipkg->owned_dirs);
	ctx->diri = diri = apk_db_diri_new(db, pkg, dir, &ctx->diri_node);
	ctx->file_diri_node = hlist_tail_ptr(&diri->owned_files);

	return diri;
}

static int contains_control_character(const char *str)
{
	for (const uint8_t *p = (const uint8_t *) str; *p; p++) {
		if (*p < 0x20 || *p == 0x7f) return 1;
	}
	return 0;
}

static int need_checksum(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFIFO:
		return FALSE;
	default:
		return TRUE;
	}
}

static int apk_db_install_v2meta(struct apk_extract_ctx *ectx, struct apk_istream *is)
{
	struct install_ctx *ctx = container_of(ectx, struct install_ctx, ectx);
	apk_blob_t l, token = APK_BLOB_STR("\n");
	int r;

	while (apk_istream_get_delim(is, token, &l) == 0) {
		r = read_info_line(ctx, l);
		if (r < 0) return r;
	}

	return 0;
}

static int apk_db_install_v3meta(struct apk_extract_ctx *ectx, struct adb_obj *pkg)
{
	static const int script_type_to_field[] = {
		[APK_SCRIPT_PRE_INSTALL]	= ADBI_SCRPT_PREINST,
		[APK_SCRIPT_POST_INSTALL]	= ADBI_SCRPT_POSTINST,
		[APK_SCRIPT_PRE_DEINSTALL]	= ADBI_SCRPT_PREDEINST,
		[APK_SCRIPT_POST_DEINSTALL]	= ADBI_SCRPT_POSTDEINST,
		[APK_SCRIPT_PRE_UPGRADE]	= ADBI_SCRPT_PREUPGRADE,
		[APK_SCRIPT_POST_UPGRADE]	= ADBI_SCRPT_POSTUPGRADE,
		[APK_SCRIPT_TRIGGER]		= ADBI_SCRPT_TRIGGER,
	};
	struct install_ctx *ctx = container_of(ectx, struct install_ctx, ectx);
	struct apk_database *db = ctx->db;
	struct apk_installed_package *ipkg = ctx->ipkg;
	struct adb_obj scripts, triggers, pkginfo, obj;
	int i;

	// Extract the information not available in index
	adb_ro_obj(pkg, ADBI_PKG_PKGINFO, &pkginfo);
	apk_deps_from_adb(&ipkg->replaces, db, adb_ro_obj(&pkginfo, ADBI_PI_REPLACES, &obj));
	ipkg->replaces_priority = adb_ro_int(pkg, ADBI_PKG_REPLACES_PRIORITY);
	ipkg->v3 = 1;

	adb_ro_obj(pkg, ADBI_PKG_SCRIPTS, &scripts);
	for (i = 0; i < ARRAY_SIZE(script_type_to_field); i++) {
		apk_blob_t b = adb_ro_blob(&scripts, script_type_to_field[i]);
		if (APK_BLOB_IS_NULL(b)) continue;
		apk_ipkg_assign_script(ipkg, i, apk_blob_dup(b));
		ctx->script_pending |= (i == ctx->script);
	}

	apk_string_array_resize(&ipkg->triggers, 0);
	adb_ro_obj(pkg, ADBI_PKG_TRIGGERS, &triggers);
	for (i = ADBI_FIRST; i <= adb_ra_num(&triggers); i++)
		*apk_string_array_add(&ipkg->triggers) = apk_blob_cstr(adb_ro_blob(&triggers, i));
	if (ctx->ipkg->triggers->num != 0 && !list_hashed(&ipkg->trigger_pkgs_list))
		list_add_tail(&ipkg->trigger_pkgs_list, &db->installed.triggers);

	return 0;
}

static int apk_db_install_script(struct apk_extract_ctx *ectx, unsigned int type, size_t size, struct apk_istream *is)
{
	struct install_ctx *ctx = container_of(ectx, struct install_ctx, ectx);
	struct apk_package *pkg = ctx->pkg;
	apk_ipkg_add_script(pkg->ipkg, is, type, size);
	ctx->script_pending |= (type == ctx->script);
	return 0;
}

static int apk_db_install_file(struct apk_extract_ctx *ectx, const struct apk_file_info *ae, struct apk_istream *is)
{
	struct install_ctx *ctx = container_of(ectx, struct install_ctx, ectx);
	static const char dot1[] = "/./", dot2[] = "/../";
	struct apk_database *db = ctx->db;
	struct apk_ctx *ac = db->ctx;
	struct apk_out *out = &ac->out;
	struct apk_package *pkg = ctx->pkg, *opkg;
	struct apk_installed_package *ipkg = pkg->ipkg;
	apk_blob_t name = APK_BLOB_STR(ae->name), bdir, bfile;
	struct apk_db_dir_instance *diri = ctx->diri;
	struct apk_db_file *file, *link_target_file = NULL;
	int ret = 0, r;

	apk_db_run_pending_script(ctx);
	if (ae->name[0] == '.') return 0;

	/* Sanity check the file name */
	if (ae->name[0] == '/' || contains_control_character(ae->name) ||
	    strncmp(ae->name, &dot1[1], 2) == 0 ||
	    strncmp(ae->name, &dot2[1], 3) == 0 ||
	    strstr(ae->name, dot1) || strstr(ae->name, dot2)) {
		apk_warn(out, PKG_VER_FMT": ignoring malicious file %s",
			PKG_VER_PRINTF(pkg), ae->name);
		ipkg->broken_files = 1;
		return 0;
	}

	/* Installable entry */
	ctx->current_file_size = apk_calc_installed_size(ae->size);
	if (!S_ISDIR(ae->mode)) {
		if (!apk_blob_rsplit(name, '/', &bdir, &bfile)) {
			bdir = APK_BLOB_NULL;
			bfile = name;
		}

		/* Make sure the file is part of the cached directory tree */
		diri = ctx->diri = find_diri(ipkg, bdir, diri, &ctx->file_diri_node);
		if (diri == NULL) {
			if (!APK_BLOB_IS_NULL(bdir)) {
				apk_err(out, PKG_VER_FMT": "BLOB_FMT": no dirent in archive",
					PKG_VER_PRINTF(pkg), BLOB_PRINTF(name));
				ipkg->broken_files = 1;
				return 0;
			}
			diri = apk_db_install_directory_entry(ctx, bdir);
		}

		/* Check hard link target to exist in this package */
		if (S_ISREG(ae->mode) && ae->link_target) {
			do {
				struct apk_db_file *lfile;
				struct apk_db_dir_instance *ldiri;
				struct hlist_node *n;
				apk_blob_t hldir, hlfile;

				if (!apk_blob_rsplit(APK_BLOB_STR(ae->link_target),
						     '/', &hldir, &hlfile))
					break;

				ldiri = find_diri(ipkg, hldir, diri, NULL);
				if (ldiri == NULL)
					break;

				hlist_for_each_entry(lfile, n, &ldiri->owned_files,
						     diri_files_list) {
					if (apk_blob_compare(APK_BLOB_PTR_LEN(lfile->name, lfile->namelen),
							     hlfile) == 0) {
						link_target_file = lfile;
						break;
					}
				}
			} while (0);

			if (!link_target_file) {
				apk_err(out, PKG_VER_FMT": "BLOB_FMT": no hard link target (%s) in archive",
					PKG_VER_PRINTF(pkg), BLOB_PRINTF(name), ae->link_target);
				ipkg->broken_files = 1;
				return 0;
			}
		}

		opkg = NULL;
		file = apk_db_file_query(db, bdir, bfile);
		if (file != NULL) {
			opkg = file->diri->pkg;
			switch (apk_pkg_replaces_file(opkg, pkg)) {
			case APK_PKG_REPLACES_CONFLICT:
				if (db->ctx->force & APK_FORCE_OVERWRITE) {
					apk_warn(out, PKG_VER_FMT": overwriting %s owned by "PKG_VER_FMT".",
						PKG_VER_PRINTF(pkg), ae->name, PKG_VER_PRINTF(opkg));
					break;
				}
				apk_err(out, PKG_VER_FMT": trying to overwrite %s owned by "PKG_VER_FMT".",
					PKG_VER_PRINTF(pkg), ae->name, PKG_VER_PRINTF(opkg));
				ipkg->broken_files = 1;
			case APK_PKG_REPLACES_NO:
				return 0;
			case APK_PKG_REPLACES_YES:
				break;
			}
		}

		if (opkg != pkg) {
			/* Create the file entry without adding it to hash */
			file = apk_db_file_new(diri, bfile, &ctx->file_diri_node);
		}

		apk_dbg2(out, "%s", ae->name);

		/* Extract the file with temporary name */
		file->acl = apk_db_acl_atomize_digest(db, ae->mode, ae->uid, ae->gid, &ae->xattr_digest);
		r = apk_fs_extract(ac, ae, is, extract_cb, ctx, db->extract_flags, apk_pkg_ctx(pkg));
		switch (r) {
		case 0:
			// Hardlinks need special care for checksum
			if (link_target_file)
				memcpy(&file->csum, &link_target_file->csum, sizeof file->csum);
			else
				apk_checksum_from_digest(&file->csum, &ae->digest);

			if (ipkg->v3 && S_ISLNK(ae->mode)) {
				struct apk_digest d;
				apk_digest_calc(&d, APK_DIGEST_SHA256,
						ae->link_target, strlen(ae->link_target));
				ipkg->sha256_160 = 1;
				file->csum.type = APK_CHECKSUM_SHA1;
				memcpy(file->csum.data, d.data, file->csum.type);
			} else if (file->csum.type == APK_CHECKSUM_NONE && ae->digest.alg == APK_DIGEST_SHA256) {
				ipkg->sha256_160 = 1;
				file->csum.type = APK_CHECKSUM_SHA1;
				memcpy(file->csum.data, ae->digest.data, file->csum.type);
			} else if (link_target_file == NULL && need_checksum(ae->mode) && !ctx->missing_checksum) {
				if (ae->digest.alg == APK_DIGEST_NONE) {
					apk_warn(out,
						PKG_VER_FMT": support for packages without embedded "
						"checksums will be dropped in apk-tools 3.",
						PKG_VER_PRINTF(pkg));
					ipkg->broken_files = 1;
					ctx->missing_checksum = 1;
				} else if (file->csum.type == APK_CHECKSUM_NONE) {
					apk_warn(out,
						PKG_VER_FMT": unknown v3 checksum",
						PKG_VER_PRINTF(pkg));
					ipkg->broken_files = 1;
					ctx->missing_checksum = 1;
				}
			}
			break;
		case -ENOTSUP:
			ipkg->broken_xattr = 1;
			break;
		case -ENOSPC:
			ret = r;
		case -APKE_UVOL_ROOT:
		case -APKE_UVOL_NOT_AVAILABLE:
		default:
			ipkg->broken_files = 1;
			break;
		}
	} else {
		struct apk_db_acl *expected_acl;

		apk_dbg2(out, "%s (dir)", ae->name);
		if (name.ptr[name.len-1] == '/') name.len--;

		diri = ctx->diri = find_diri(ipkg, name, NULL, &ctx->file_diri_node);
		if (!diri) diri = apk_db_install_directory_entry(ctx, name);
		diri->acl = apk_db_acl_atomize_digest(db, ae->mode, ae->uid, ae->gid, &ae->xattr_digest);
		expected_acl = diri->dir->owner ? diri->dir->owner->acl : NULL;
		apk_db_dir_apply_diri_permissions(db, diri);
		apk_db_dir_prepare(db, diri->dir, expected_acl);

	}
	ctx->installed_size += ctx->current_file_size;

	return ret;
}

static const struct apk_extract_ops extract_installer = {
	.v2meta = apk_db_install_v2meta,
	.v3meta = apk_db_install_v3meta,
	.script = apk_db_install_script,
	.file = apk_db_install_file,
};

static int apk_db_audit_file(struct apk_fsdir *d, apk_blob_t filename, struct apk_db_file *dbf)
{
	struct apk_file_info fi;
	int r;

	// Check file first
	r = apk_fsdir_file_info(d, filename, APK_FI_NOFOLLOW | APK_FI_DIGEST(apk_dbf_digest(dbf)), &fi);
	if (r != 0 || !dbf || dbf->csum.type == APK_CHECKSUM_NONE) return r != -ENOENT;
	if (apk_digest_cmp_csum(&fi.digest, &dbf->csum) != 0) return 1;
	return 0;
}

static void apk_db_purge_pkg(struct apk_database *db,
			     struct apk_installed_package *ipkg,
			     int is_installed)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_db_dir_instance *diri;
	struct apk_db_file *file;
	struct apk_db_file_hash_key key;
	struct apk_fsdir d;
	struct hlist_node *dc, *dn, *fc, *fn;
	unsigned long hash;
	int ctrl = is_installed ? APK_FS_CTRL_DELETE : APK_FS_CTRL_CANCEL;

	hlist_for_each_entry_safe(diri, dc, dn, &ipkg->owned_dirs, pkg_dirs_list) {
		apk_blob_t dirname = APK_BLOB_PTR_LEN(diri->dir->name, diri->dir->namelen);
		if (is_installed) diri->dir->modified = 1;
		apk_fsdir_get(&d, dirname, db->ctx, apk_pkg_ctx(ipkg->pkg));

		hlist_for_each_entry_safe(file, fc, fn, &diri->owned_files, diri_files_list) {
			key = (struct apk_db_file_hash_key) {
				.dirname = dirname,
				.filename = APK_BLOB_PTR_LEN(file->name, file->namelen),
			};
			hash = apk_blob_hash_seed(key.filename, diri->dir->hash);
			if (!is_installed ||
			    apk_protect_mode_none(diri->dir->protect_mode) ||
			    (db->ctx->flags & APK_PURGE) ||
			    apk_db_audit_file(&d, key.filename, file) == 0)
				apk_fsdir_file_control(&d, key.filename, ctrl);

			apk_dbg2(out, DIR_FILE_FMT, DIR_FILE_PRINTF(diri->dir, file));
			__hlist_del(fc, &diri->owned_files.first);
			if (is_installed) {
				apk_hash_delete_hashed(&db->installed.files, APK_BLOB_BUF(&key), hash);
				db->installed.stats.files--;
			}
		}
		__hlist_del(dc, &ipkg->owned_dirs.first);
		apk_db_diri_free(db, diri, APK_DIR_REMOVE);
	}
}

static uint8_t apk_db_migrate_files_for_priority(struct apk_database *db,
						 struct apk_installed_package *ipkg,
						 uint8_t priority)
{
	struct apk_out *out = &db->ctx->out;
	struct apk_db_dir_instance *diri;
	struct apk_db_dir *dir;
	struct apk_db_file *file, *ofile;
	struct apk_db_file_hash_key key;
	struct hlist_node *dc, *dn, *fc, *fn;
	struct apk_fsdir d;
	unsigned long hash;
	apk_blob_t dirname;
	int r, ctrl, inetc;
	uint8_t dir_priority, next_priority = 0xff;

	hlist_for_each_entry_safe(diri, dc, dn, &ipkg->owned_dirs, pkg_dirs_list) {
		dir = diri->dir;
		dirname = APK_BLOB_PTR_LEN(dir->name, dir->namelen);
		apk_fsdir_get(&d, dirname, db->ctx, apk_pkg_ctx(ipkg->pkg));
		dir_priority = apk_fsdir_priority(&d);
		if (dir_priority != priority) {
			if (dir_priority > priority && dir_priority < next_priority)
				next_priority = dir_priority;
			continue;
		}
		// Used for passwd/group check later
		inetc = !apk_blob_compare(dirname, APK_BLOB_STRLIT("etc"));

		dir->modified = 1;
		hlist_for_each_entry_safe(file, fc, fn, &diri->owned_files, diri_files_list) {
			key = (struct apk_db_file_hash_key) {
				.dirname = dirname,
				.filename = APK_BLOB_PTR_LEN(file->name, file->namelen),
			};

			hash = apk_blob_hash_seed(key.filename, dir->hash);

			/* check for existing file */
			ofile = (struct apk_db_file *) apk_hash_get_hashed(
				&db->installed.files, APK_BLOB_BUF(&key), hash);

			ctrl = APK_FS_CTRL_COMMIT;
			if (ofile && ofile->diri->pkg->name == NULL) {
				// File was from overlay, delete the package's version
				ctrl = APK_FS_CTRL_CANCEL;
			} else if (!apk_protect_mode_none(diri->dir->protect_mode) &&
				   apk_db_audit_file(&d, key.filename, ofile) != 0) {
				// Protected directory, and a file without db entry
				// or with local modifications. Keep the filesystem file.
				// Determine if the package's file should be kept as .apk-new
				if ((db->ctx->flags & APK_CLEAN_PROTECTED) ||
				    apk_db_audit_file(&d, key.filename, file) == 0) {
					// No .apk-new files allowed, or the file on disk has the same
					// hash as the file from new package. Keep the on disk one.
					ctrl = APK_FS_CTRL_CANCEL;
				} else {
					// All files differ. Use the package's file as .apk-new.
					ctrl = APK_FS_CTRL_APKNEW;
				}
			}

			// Commit changes
			r = apk_fsdir_file_control(&d, key.filename, ctrl);
			if (r < 0) {
				apk_err(out, PKG_VER_FMT": failed to commit " DIR_FILE_FMT ": %s",
					PKG_VER_PRINTF(ipkg->pkg),
					DIR_FILE_PRINTF(diri->dir, file),
					apk_error_str(r));
				ipkg->broken_files = 1;
			} else if (inetc && ctrl == APK_FS_CTRL_COMMIT) {
				// This is called when we successfully migrated the files
				// in the filesystem; we explicitly do not care about apk-new
				// or cancel cases, as that does not change the original file
				if (!apk_blob_compare(key.filename, APK_BLOB_STRLIT("passwd")) ||
				    !apk_blob_compare(key.filename, APK_BLOB_STRLIT("group"))) {
					// Reset the idcache because we have a new passwd/group
					apk_id_cache_reset(db->id_cache);
				}
			}

			// Claim ownership of the file in db
			if (ofile != file) {
				if (ofile != NULL) {
					hlist_del(&ofile->diri_files_list,
						&ofile->diri->owned_files);
					apk_hash_delete_hashed(&db->installed.files,
							       APK_BLOB_BUF(&key), hash);
				} else
					db->installed.stats.files++;

				apk_hash_insert_hashed(&db->installed.files, file, hash);
			}
		}
	}
	return next_priority;
}

static void apk_db_migrate_files(struct apk_database *db,
				 struct apk_installed_package *ipkg)
{
	for (uint8_t prio = APK_FS_PRIO_DISK; prio != 0xff; )
		prio = apk_db_migrate_files_for_priority(db, ipkg, prio);
}

static int apk_db_unpack_pkg(struct apk_database *db,
			     struct apk_installed_package *ipkg,
			     int upgrade, apk_progress_cb cb, void *cb_ctx,
			     char **script_args)
{
	struct apk_out *out = &db->ctx->out;
	struct install_ctx ctx;
	struct apk_istream *is = NULL;
	struct apk_repository *repo;
	struct apk_package *pkg = ipkg->pkg;
	char file[PATH_MAX];
	char cacheitem[128];
	int r, filefd = AT_FDCWD, need_copy = FALSE;

	if (pkg->filename == NULL) {
		repo = apk_db_select_repo(db, pkg);
		if (repo == NULL) {
			r = -APKE_PACKAGE_NOT_FOUND;
			goto err_msg;
		}
		r = apk_repo_format_item(db, repo, pkg, &filefd, file, sizeof(file));
		if (r < 0)
			goto err_msg;
		if (!(pkg->repos & db->local_repos))
			need_copy = TRUE;
	} else {
		if (strlcpy(file, pkg->filename, sizeof file) >= sizeof file) {
			r = -ENAMETOOLONG;
			goto err_msg;
		}
		need_copy = TRUE;
	}
	if (!apk_db_cache_active(db))
		need_copy = FALSE;

	is = apk_istream_from_fd_url(filefd, file, apk_db_url_since(db, 0));
	if (IS_ERR(is)) {
		r = PTR_ERR(is);
		if (r == -ENOENT && pkg->filename == NULL)
			r = -APKE_INDEX_STALE;
		goto err_msg;
	}
	if (need_copy) {
		struct apk_istream *origis = is;
		apk_pkg_format_cache_pkg(APK_BLOB_BUF(cacheitem), pkg);
		is = apk_istream_tee(is, apk_ostream_to_file(db->cache_fd, cacheitem, 0644),
			APK_ISTREAM_TEE_COPY_META|APK_ISTREAM_TEE_OPTIONAL, NULL, NULL);
		if (is == origis)
			apk_warn(out, PKG_VER_FMT": unable to cache package",
				 PKG_VER_PRINTF(pkg));
	}

	ctx = (struct install_ctx) {
		.db = db,
		.pkg = pkg,
		.ipkg = ipkg,
		.script = upgrade ?
			APK_SCRIPT_PRE_UPGRADE : APK_SCRIPT_PRE_INSTALL,
		.script_args = script_args,
		.cb = cb,
		.cb_ctx = cb_ctx,
	};
	apk_extract_init(&ctx.ectx, db->ctx, &extract_installer);
	apk_extract_verify_identity(&ctx.ectx, &pkg->csum);
	r = apk_extract(&ctx.ectx, is);
	if (need_copy && r == 0) pkg->repos |= BIT(APK_REPOSITORY_CACHED);
	if (r != 0) goto err_msg;

	apk_db_run_pending_script(&ctx);
	return 0;
err_msg:
	apk_err(out, PKG_VER_FMT": %s", PKG_VER_PRINTF(pkg), apk_error_str(r));
	return r;
}

int apk_db_install_pkg(struct apk_database *db, struct apk_package *oldpkg,
		       struct apk_package *newpkg, apk_progress_cb cb, void *cb_ctx)
{
	char *script_args[] = { NULL, NULL, NULL, NULL };
	struct apk_installed_package *ipkg;
	int r = 0;

	/* Upgrade script gets two args: <new-pkg> <old-pkg> */
	if (oldpkg != NULL && newpkg != NULL) {
		script_args[1] = apk_blob_cstr(*newpkg->version);
		script_args[2] = apk_blob_cstr(*oldpkg->version);
	} else {
		script_args[1] = apk_blob_cstr(*(oldpkg ? oldpkg->version : newpkg->version));
	}

	/* Just purging? */
	if (oldpkg != NULL && newpkg == NULL) {
		ipkg = oldpkg->ipkg;
		if (ipkg == NULL)
			goto ret_r;
		apk_ipkg_run_script(ipkg, db, APK_SCRIPT_PRE_DEINSTALL, script_args);
		apk_db_purge_pkg(db, ipkg, TRUE);
		apk_ipkg_run_script(ipkg, db, APK_SCRIPT_POST_DEINSTALL, script_args);
		apk_pkg_uninstall(db, oldpkg);
		goto ret_r;
	}

	/* Install the new stuff */
	ipkg = apk_pkg_install(db, newpkg);
	ipkg->run_all_triggers = 1;
	ipkg->broken_script = 0;
	ipkg->broken_files = 0;
	ipkg->broken_xattr = 0;
	if (ipkg->triggers->num != 0) {
		list_del(&ipkg->trigger_pkgs_list);
		list_init(&ipkg->trigger_pkgs_list);
		apk_string_array_free(&ipkg->triggers);
	}

	if (newpkg->installed_size != 0) {
		r = apk_db_unpack_pkg(db, ipkg, (oldpkg != NULL),
				      cb, cb_ctx, script_args);
		if (r != 0) {
			if (oldpkg != newpkg)
				apk_db_purge_pkg(db, ipkg, FALSE);
			apk_pkg_uninstall(db, newpkg);
			goto ret_r;
		}
		apk_db_migrate_files(db, ipkg);
	}

	if (oldpkg != NULL && oldpkg != newpkg && oldpkg->ipkg != NULL) {
		apk_db_purge_pkg(db, oldpkg->ipkg, TRUE);
		apk_pkg_uninstall(db, oldpkg);
	}

	apk_ipkg_run_script(
		ipkg, db,
		(oldpkg == NULL) ? APK_SCRIPT_POST_INSTALL : APK_SCRIPT_POST_UPGRADE,
		script_args);

	if (ipkg->broken_files || ipkg->broken_script)
		r = -1;
ret_r:
	free(script_args[1]);
	free(script_args[2]);
	return r;
}

struct match_ctx {
	struct apk_database *db;
	struct apk_string_array *filter;
	apk_db_foreach_name_cb cb;
	void *cb_ctx;
};

static int apk_string_match(const char *str, struct apk_string_array *filter, const char **res)
{
	char **pmatch;

	foreach_array_item(pmatch, filter) {
		if (fnmatch(*pmatch, str, FNM_CASEFOLD) == 0) {
			*res = *pmatch;
			return 1;
		}
	}
	return 0;
}

static int apk_name_match(struct apk_name *name, struct apk_string_array *filter, const char **res)
{
	if (!filter) {
		*res = NULL;
		return 1;
	}
	return apk_string_match(name->name, filter, res);
}

static int apk_pkg_match(struct apk_package *pkg, struct apk_string_array *filter, const char **res, int provides)
{
	struct apk_dependency *d;

	if (apk_name_match(pkg->name, filter, res)) return 1;
	if (!provides) return 0;
	foreach_array_item(d, pkg->provides) {
		if (apk_string_match(d->name->name, filter, res)) return 1;
	}
	return 0;
}

static int match_names(apk_hash_item item, void *pctx)
{
	struct match_ctx *ctx = (struct match_ctx *) pctx;
	struct apk_name *name = (struct apk_name *) item;
	const char *match;

	if (apk_name_match(name, ctx->filter, &match))
		return ctx->cb(ctx->db, match, name, ctx->cb_ctx);
	return 0;
}

int apk_db_foreach_matching_name(
	struct apk_database *db, struct apk_string_array *filter,
	apk_db_foreach_name_cb cb, void *ctx)
{
	char **pmatch;
	struct apk_name *name;
	struct match_ctx mctx = {
		.db = db,
		.cb = cb,
		.cb_ctx = ctx,
	};
	int r;

	if (!filter || !filter->num) goto all;

	mctx.filter = filter;
	foreach_array_item(pmatch, filter)
		if (strchr(*pmatch, '*') != NULL)
			goto all;

	foreach_array_item(pmatch, filter) {
		name = (struct apk_name *) apk_hash_get(&db->available.names, APK_BLOB_STR(*pmatch));
		r = cb(db, *pmatch, name, ctx);
		if (r) return r;
	}
	return 0;

all:
	return apk_hash_foreach(&db->available.names, match_names, &mctx);
}

static int cmp_name(const void *a, const void *b)
{
	const struct apk_name * const* na = a, * const* nb = b;
	return apk_name_cmp_display(*na, *nb);
}

static int cmp_package(const void *a, const void *b)
{
	const struct apk_package * const* pa = a, * const* pb = b;
	return apk_pkg_cmp_display(*pa, *pb);
}

struct add_name_ctx {
	struct apk_name_array *a;
	size_t i;
};

static int add_name(apk_hash_item item, void *ctx)
{
	struct apk_name *name = (struct apk_name *) item;
	struct add_name_ctx *a = ctx;
	a->a->item[a->i++] = name;
	return 0;
}

static struct apk_name_array *apk_db_sorted_names(struct apk_database *db)
{
	if (!db->sorted_names) {
		apk_name_array_resize(&db->available.sorted_names, db->available.names.num_items);

		struct add_name_ctx ctx = { .a = db->available.sorted_names, .i = 0 };
		apk_hash_foreach(&db->available.names, add_name, &ctx);

		qsort(db->available.sorted_names->item, db->available.sorted_names->num,
		      sizeof(db->available.sorted_names->item[0]), cmp_name);
		db->sorted_names = 1;
	}
	return db->available.sorted_names;
}

struct apk_package_array *apk_db_sorted_installed_packages(struct apk_database *db)
{
	struct apk_installed_package *ipkg;
	int n = 0;

	if (!db->sorted_installed_packages) {
		db->sorted_installed_packages = 1;
		apk_package_array_resize(&db->installed.sorted_packages, db->installed.stats.packages);
		list_for_each_entry(ipkg, &db->installed.packages, installed_pkgs_list)
			db->installed.sorted_packages->item[n++] = ipkg->pkg;
		qsort(db->installed.sorted_packages->item, db->installed.sorted_packages->num,
		      sizeof db->installed.sorted_packages->item[0], cmp_package);
	}
	return db->installed.sorted_packages;
}

int apk_db_foreach_sorted_name(struct apk_database *db, struct apk_string_array *filter,
			       apk_db_foreach_name_cb cb, void *cb_ctx)
{
	int r, walk_all = 0;
	char **pmatch;
	const char *match;
	struct apk_name *name;
	struct apk_name *results[128], **res;
	size_t i, num_res = 0;

	if (filter && filter->num) {
		foreach_array_item(pmatch, filter) {
			name = (struct apk_name *) apk_hash_get(&db->available.names, APK_BLOB_STR(*pmatch));
			if (strchr(*pmatch, '*')) {
				walk_all = 1;
				continue;
			}
			if (!name) {
				cb(db, *pmatch, NULL, cb_ctx);
				continue;
			}
			if (walk_all) continue;
			if (num_res >= ARRAY_SIZE(results)) {
				walk_all = 1;
				continue;
			}
			results[num_res++] = name;
		}
	} else {
		filter = NULL;
		walk_all = 1;
	}

	if (walk_all) {
		struct apk_name_array *a = apk_db_sorted_names(db);
		res = a->item;
		num_res = a->num;
	} else {
		qsort(results, num_res, sizeof results[0], cmp_name);
		res = results;
	}

	for (i = 0; i < num_res; i++) {
		name = res[i];
		if (apk_name_match(name, filter, &match)) {
			r = cb(db, match, name, cb_ctx);
			if (r) return r;
		}
	}
	return 0;
}

int __apk_db_foreach_sorted_package(struct apk_database *db, struct apk_string_array *filter,
				    apk_db_foreach_package_cb cb, void *cb_ctx, int provides)
{
	char **pmatch;
	const char *match;
	struct apk_name *name;
	struct apk_package *results[128];
	struct apk_provider *p;
	size_t i, num_res = 0;
	int r;

	if (!filter || !filter->num) {
		filter = NULL;
		goto walk_all;
	}

	foreach_array_item(pmatch, filter) {
		name = (struct apk_name *) apk_hash_get(&db->available.names, APK_BLOB_STR(*pmatch));
		if (strchr(*pmatch, '*')) goto walk_all;
		if (!name) {
			cb(db, *pmatch, NULL, cb_ctx);
			continue;
		}

		foreach_array_item(p, name->providers) {
			if (!provides && p->pkg->name != name) continue;
			if (p->pkg->seen) continue;
			p->pkg->seen = 1;
			if (num_res >= ARRAY_SIZE(results)) goto walk_all;
			results[num_res++] = p->pkg;
		}
	}
	for (i = 0; i < num_res; i++) results[i]->seen = 0;

	qsort(results, num_res, sizeof results[0], cmp_package);
	for (i = 0; i < num_res; i++) {
		if (apk_pkg_match(results[i], filter, &match, provides)) {
			r = cb(db, match, results[i], cb_ctx);
			if (r) return r;
		}
	}
	return 0;

walk_all:
	for (i = 0; i < num_res; i++) results[i]->seen = 0;

	struct apk_name_array *a = apk_db_sorted_names(db);
	for (i = 0; i < a->num; i++) {
		name = a->item[i];
		apk_name_sorted_providers(name);
		foreach_array_item(p, name->providers) {
			if (p->pkg->name != name) continue;
			if (apk_pkg_match(p->pkg, filter, &match, provides)) {
				r = cb(db, match, p->pkg, cb_ctx);
				if (r) return r;
			}
		}
	}
	return 0;
}
