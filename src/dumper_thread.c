/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2014  Tomas Mlcoch
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "dumper_thread.h"
#include "error.h"
#include "misc.h"
#include "parsepkg.h"
#include "xml_dump.h"

#define MAX_TASK_BUFFER_LEN 20


struct BufferedTask {
    long id;                        // ID of the task
    struct cr_XmlStruct res;        // XML for primary, filelists and other
    cr_Package *pkg;                // Package structure
    char *location_href;            // location_href path
    int pkg_from_md;                // If true - package structure if from
                                    // old metadata and must not be freed!
                                    // If false - package is from file and
                                    // it must be freed!
};


static gint
buf_task_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    CR_UNUSED(data);
    const struct BufferedTask *task_a = a;
    const struct BufferedTask *task_b = b;
    if (task_a->id < task_b->id)  return -1;
    if (task_a->id == task_b->id) return 0;
    return 1;
}


static void
write_pkg(long id,
          struct cr_XmlStruct res,
          cr_Package *pkg,
          struct UserData *udata)
{
    GError *tmp_err = NULL;

    // Write primary data
    g_mutex_lock(udata->mutex_pri);
    while (udata->id_pri != id)
        g_cond_wait (udata->cond_pri, udata->mutex_pri);
    ++udata->id_pri;
    cr_xmlfile_add_chunk(udata->pri_f, (const char *) res.primary, &tmp_err);
    if (tmp_err) {
        g_critical("Cannot add primary chunk:\n%s\nError: %s",
                   res.primary, tmp_err->message);
        g_clear_error(&tmp_err);
    }

    if (udata->pri_db) {
        cr_db_add_pkg(udata->pri_db, pkg, &tmp_err);
        if (tmp_err) {
            g_critical("Cannot add record of %s (%s) to primary db: %s",
                       pkg->name, pkg->pkgId, tmp_err->message);
            g_clear_error(&tmp_err);
        }
    }

    g_cond_broadcast(udata->cond_pri);
    g_mutex_unlock(udata->mutex_pri);

    // Write fielists data
    g_mutex_lock(udata->mutex_fil);
    while (udata->id_fil != id)
        g_cond_wait (udata->cond_fil, udata->mutex_fil);
    ++udata->id_fil;
    cr_xmlfile_add_chunk(udata->fil_f, (const char *) res.filelists, &tmp_err);
    if (tmp_err) {
        g_critical("Cannot add filelists chunk:\n%s\nError: %s",
                   res.filelists, tmp_err->message);
        g_clear_error(&tmp_err);
    }

    if (udata->fil_db) {
        cr_db_add_pkg(udata->fil_db, pkg, &tmp_err);
        if (tmp_err) {
            g_critical("Cannot add record of %s (%s) to filelists db: %s",
                       pkg->name, pkg->pkgId, tmp_err->message);
            g_clear_error(&tmp_err);
        }
    }

    g_cond_broadcast(udata->cond_fil);
    g_mutex_unlock(udata->mutex_fil);

    // Write other data
    g_mutex_lock(udata->mutex_oth);
    while (udata->id_oth != id)
        g_cond_wait (udata->cond_oth, udata->mutex_oth);
    ++udata->id_oth;
    cr_xmlfile_add_chunk(udata->oth_f, (const char *) res.other, &tmp_err);
    if (tmp_err) {
        g_critical("Cannot add other chunk:\n%s\nError: %s",
                   res.other, tmp_err->message);
        g_clear_error(&tmp_err);
    }

    if (udata->oth_db) {
        cr_db_add_pkg(udata->oth_db, pkg, NULL);
        if (tmp_err) {
            g_critical("Cannot add record of %s (%s) to other db: %s",
                       pkg->name, pkg->pkgId, tmp_err->message);
            g_clear_error(&tmp_err);
        }
    }

    g_cond_broadcast(udata->cond_oth);
    g_mutex_unlock(udata->mutex_oth);
}

static char *
get_checksum(const char *filename,
             cr_ChecksumType type,
             const char *cachedir,
             GError **err)
{
    GError *tmp_err = NULL;
    char *checksum = NULL;

    checksum = cr_checksum_file(filename, type, &tmp_err);
    if (!checksum) {
        g_propagate_prefixed_error(err, tmp_err,
                                   "Error while checksum calculation: ");
    }
    return checksum;
}

static cr_Package *
load_rpm(const char *filename,
         cr_ChecksumType checksum_type,
         const char *checksum_cachedir,
         const char *location_href,
         const char *location_base,
         int changelog_limit,
         struct stat *stat_buf,
         GError **err)
{
    cr_Package *pkg = NULL;
    GError *tmp_err = NULL;

    assert(filename);
    assert(!err || *err == NULL);

    // Get a package object
    pkg = cr_package_from_rpm_base(filename, changelog_limit, err);
    if (!pkg)
        goto errexit;

    pkg->location_href = cr_safe_string_chunk_insert(pkg->chunk, location_href);
    pkg->location_base = cr_safe_string_chunk_insert(pkg->chunk, location_base);

    // Get checksum type string
    pkg->checksum_type = cr_safe_string_chunk_insert(pkg->chunk,
                                        cr_checksum_name_str(checksum_type));

    // Get file stat
    if (!stat_buf) {
        struct stat stat_buf_own;
        if (stat(filename, &stat_buf_own) == -1) {
            g_warning("%s: stat(%s) error (%s)", __func__,
                      filename, strerror(errno));
            g_set_error(err,  CR_PARSEPKG_ERROR, CRE_IO, "stat(%s) failed: %s",
                        filename, strerror(errno));
            goto errexit;
        }
        pkg->time_file    = stat_buf_own.st_mtime;
        pkg->size_package = stat_buf_own.st_size;
    } else {
        pkg->time_file    = stat_buf->st_mtime;
        pkg->size_package = stat_buf->st_size;
    }

    // Compute checksum
    char *checksum = get_checksum(filename, checksum_type,
                                  checksum_cachedir, &tmp_err);
    if (!checksum)
        goto errexit;
    pkg->pkgId = cr_safe_string_chunk_insert(pkg->chunk, checksum);
    free(checksum);

    // Get header range
    struct cr_HeaderRangeStruct hdr_r = cr_get_header_byte_range(filename,
                                                                 &tmp_err);
    if (tmp_err) {
        g_propagate_prefixed_error(err, tmp_err,
                                   "Error while determinig header range: ");
        goto errexit;
    }

    pkg->rpm_header_start = hdr_r.start;
    pkg->rpm_header_end = hdr_r.end;

    return pkg;

errexit:
    cr_package_free(pkg);
    return NULL;
}


void
cr_dumper_thread(gpointer data, gpointer user_data)
{
    GError *tmp_err = NULL;
    gboolean old_used = FALSE;  // To use old metadata?
    cr_Package *md  = NULL;     // Package from loaded MetaData
    cr_Package *pkg = NULL;     // Package from file
    struct stat stat_buf;       // Struct with info from stat() on file
    struct cr_XmlStruct res;    // Structure for generated XML

    struct UserData *udata = (struct UserData *) user_data;
    struct PoolTask *task  = (struct PoolTask *) data;

    // get location_href without leading part of path (path to repo)
    // including '/' char
    const char *location_href = task->full_path + udata->repodir_name_len;
    const char *location_base = udata->location_base;

    // Get stat info about file
    if (udata->old_metadata && !(udata->skip_stat)) {
        if (stat(task->full_path, &stat_buf) == -1) {
            g_critical("Stat() on %s: %s", task->full_path, strerror(errno));
            goto task_cleanup;
        }
    }

    // Update stuff
    if (udata->old_metadata) {
        // We have old metadata
        md = (cr_Package *) g_hash_table_lookup(
                                cr_metadata_hashtable(udata->old_metadata),
                                task->filename);

        if (md) {
            g_debug("CACHE HIT %s", task->filename);

            if (udata->skip_stat) {
                old_used = TRUE;
            } else if (stat_buf.st_mtime == md->time_file
                       && stat_buf.st_size == md->size_package
                       && !strcmp(udata->checksum_type_str, md->checksum_type))
            {
                old_used = TRUE;
            } else {
                g_debug("%s metadata are obsolete -> generating new",
                        task->filename);
            }

            if (old_used) {
                // We have usable old data, but we have to set proper locations
                // WARNING! This two lines destructively modifies content of
                // packages in old metadata.
                md->location_href = (char *) location_href;
                md->location_base = (char *) location_base;
            }
        }
    }

    // Load package and gen XML metadata
    if (!old_used) {
        // Load package from file
        pkg = load_rpm(task->full_path, udata->checksum_type,
                       udata->checksum_cachedir, location_href,
                       udata->location_base, udata->changelog_limit,
                       NULL, &tmp_err);
        assert(pkg || tmp_err);

        if (!pkg) {
            g_warning("Cannot read package: %s: %s",
                      task->full_path, tmp_err->message);
            g_clear_error(&tmp_err);
            goto task_cleanup;
        }

        res = cr_xml_dump(pkg, &tmp_err);
        if (tmp_err) {
            g_critical("Cannot dump XML for %s (%s): %s",
                       pkg->name, pkg->pkgId, tmp_err->message);
            g_clear_error(&tmp_err);
            goto task_cleanup;
        }
    } else {
        // Just gen XML from old loaded metadata
        pkg = md;
        res = cr_xml_dump(md, &tmp_err);
        if (tmp_err) {
            g_critical("Cannot dump XML for %s (%s): %s",
                       md->name, md->pkgId, tmp_err->message);
            g_clear_error(&tmp_err);
            goto task_cleanup;
        }
    }

    // Buffering stuff
    g_mutex_lock(udata->mutex_buffer);

    if (g_queue_get_length(udata->buffer) < MAX_TASK_BUFFER_LEN
        && udata->id_pri != task->id
        && udata->package_count > (task->id + 1))
    {
        // If:
        //  * this isn't our turn
        //  * the buffer isn't full
        //  * this isn't the last task
        // Then: save the task to the buffer

        struct BufferedTask *buf_task = malloc(sizeof(struct BufferedTask));
        buf_task->id  = task->id;
        buf_task->res = res;
        buf_task->pkg = pkg;
        buf_task->location_href = NULL;
        buf_task->pkg_from_md = (pkg == md) ? 1 : 0;

        if (pkg == md) {
            // We MUST store location_href for reused packages who goes to the buffer
            // We don't need to store location_base because it is allocated in
            // user_data during this function calls.

            buf_task->location_href = g_strdup(location_href);
            buf_task->pkg->location_href = buf_task->location_href;
        }

        g_queue_insert_sorted(udata->buffer, buf_task, buf_task_sort_func, NULL);
        g_mutex_unlock(udata->mutex_buffer);

        g_free(task->full_path);
        g_free(task->filename);
        g_free(task->path);
        g_free(task);

        return;
    }

    g_mutex_unlock(udata->mutex_buffer);

    // Dump XML and SQLite
    write_pkg(task->id, res, pkg, udata);

    // Clean up
    if (pkg != md)
        cr_package_free(pkg);
    g_free(res.primary);
    g_free(res.filelists);
    g_free(res.other);

task_cleanup:
    if (udata->id_pri <= task->id) {
        // An error was encountered and we have to wait to increment counters
        g_mutex_lock(udata->mutex_pri);
        while (udata->id_pri != task->id)
            g_cond_wait (udata->cond_pri, udata->mutex_pri);
        ++udata->id_pri;
        g_cond_broadcast(udata->cond_pri);
        g_mutex_unlock(udata->mutex_pri);

        g_mutex_lock(udata->mutex_fil);
        while (udata->id_fil != task->id)
            g_cond_wait (udata->cond_fil, udata->mutex_fil);
        ++udata->id_fil;
        g_cond_broadcast(udata->cond_fil);
        g_mutex_unlock(udata->mutex_fil);

        g_mutex_lock(udata->mutex_oth);
        while (udata->id_oth != task->id)
            g_cond_wait (udata->cond_oth, udata->mutex_oth);
        ++udata->id_oth;
        g_cond_broadcast(udata->cond_oth);
        g_mutex_unlock(udata->mutex_oth);
    }

    g_free(task->full_path);
    g_free(task->filename);
    g_free(task->path);
    g_free(task);

    // Try to write all results from buffer which was waiting for us
    while (1) {
        struct BufferedTask *buf_task;
        g_mutex_lock(udata->mutex_buffer);
        buf_task = g_queue_peek_head(udata->buffer);
        if (buf_task && buf_task->id == udata->id_pri) {
            buf_task = g_queue_pop_head (udata->buffer);
            g_mutex_unlock(udata->mutex_buffer);
            // Dump XML and SQLite
            write_pkg(buf_task->id, buf_task->res, buf_task->pkg, udata);
            // Clean up
            if (!buf_task->pkg_from_md)
                cr_package_free(buf_task->pkg);
            g_free(buf_task->res.primary);
            g_free(buf_task->res.filelists);
            g_free(buf_task->res.other);
            g_free(buf_task->location_href);
            g_free(buf_task);
        } else {
            g_mutex_unlock(udata->mutex_buffer);
            break;
        }
    }

    return;
}


