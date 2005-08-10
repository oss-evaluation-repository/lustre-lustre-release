/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light Super operations
 *
 *  Copyright (c) 2002-2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/queue.h>

#ifndef __CYGWIN__
# include <sys/statvfs.h>
#else
# include <sys/statfs.h>
#endif

#ifdef HAVE_XTIO_H
#include <xtio.h>
#endif
#include <sysio.h>
#include <mount.h>
#include <inode.h>
#include <fs.h>
#ifdef HAVE_FILE_H
#include <file.h>
#endif

#undef LIST_HEAD
#include "llite_lib.h"

#ifndef MAY_EXEC
#  define MAY_EXEC        1
#  define MAY_WRITE       2
#  define MAY_READ        4
#endif

#define S_IXUGO (S_IXUSR|S_IXGRP|S_IXOTH)

static int ll_permission(struct inode *inode, int mask)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        mode_t mode = lli->lli_st_mode;

        if (current->fsuid == lli->lli_st_uid)
                mode >>= 6;
        else if (in_group_p(lli->lli_st_gid))
                mode >>= 3;

        if ((mode & mask & (MAY_READ|MAY_WRITE|MAY_EXEC)) == mask)
                return 0;

        if ((mask & (MAY_READ|MAY_WRITE)) ||
            (lli->lli_st_mode & S_IXUGO))
                if (capable(CAP_DAC_OVERRIDE))
                        return 0;

        if (mask == MAY_READ ||
            (S_ISDIR(lli->lli_st_mode) && !(mask & MAY_WRITE))) {
                if (capable(CAP_DAC_READ_SEARCH))
                        return 0;
        }

        return -EACCES;
}

static void llu_fsop_gone(struct filesys *fs)
{
        struct llu_sb_info *sbi = (struct llu_sb_info *)fs->fs_private;
        struct obd_device *obd = class_exp2obd(sbi->ll_md_exp);
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        int next = 0;
        ENTRY;

        list_del(&sbi->ll_conn_chain);
        obd_disconnect(sbi->ll_dt_exp, 0);
        obd_disconnect(sbi->ll_md_exp, 0);

        while ((obd = class_devices_in_group(&sbi->ll_sb_uuid, &next)) != NULL)
        {
                int err;
        
                lustre_cfg_bufs_reset(&bufs, obd->obd_name);
                lcfg = lustre_cfg_new(LCFG_CLEANUP, &bufs);
                err = class_process_config(lcfg);
                if (err) {
                        CERROR("cleanup failed: %s\n", obd->obd_name);
                }
                
                lcfg->lcfg_command = LCFG_DETACH; 
                err = class_process_config(lcfg);
                lustre_cfg_free(lcfg);
                if (err) {
                        CERROR("detach failed: %s\n", obd->obd_name);
                }
        }

        obd_disconnect(sbi->ll_md_exp, 0);
        OBD_FREE(sbi, sizeof(*sbi));
        EXIT;
}

struct inode_ops llu_inode_ops;

void llu_update_inode(struct inode *inode, struct mds_body *body,
                      struct lov_stripe_md *lsm)
{
        struct llu_inode_info *lli = llu_i2info(inode);

        LASSERT ((lsm != NULL) == ((body->valid & OBD_MD_FLEASIZE) != 0));
        if (lsm != NULL) {
                if (lli->lli_smd == NULL) {
                        lli->lli_smd = lsm;
                        lli->lli_maxbytes = lsm->lsm_maxbytes;
                        if (lli->lli_maxbytes > PAGE_CACHE_MAXBYTES)
                                lli->lli_maxbytes = PAGE_CACHE_MAXBYTES;
                } else {
                        if (memcmp(lli->lli_smd, lsm, sizeof(*lsm))) {
                                CERROR("lsm mismatch for inode %ld\n",
                                       lli->lli_st_ino);
                                LBUG();
                        }
                }
        }

        id_assign_fid(&lli->lli_id, &body->id1);
        
	if ((body->valid & OBD_MD_FLID) || (body->valid & OBD_MD_FLGENER))
                id_assign_stc(&lli->lli_id, &body->id1);
        if (body->valid & OBD_MD_FLID)
                lli->lli_st_ino = id_ino(&body->id1);
        if (body->valid & OBD_MD_FLGENER)
                lli->lli_st_generation = id_gen(&body->id1);

        if (body->valid & OBD_MD_FLATIME)
                LTIME_S(lli->lli_st_atime) = body->atime;
        if (body->valid & OBD_MD_FLMTIME)
                LTIME_S(lli->lli_st_mtime) = body->mtime;
        if (body->valid & OBD_MD_FLCTIME)
                LTIME_S(lli->lli_st_ctime) = body->ctime;
        if (body->valid & OBD_MD_FLMODE)
                lli->lli_st_mode = (lli->lli_st_mode & S_IFMT)|(body->mode & ~S_IFMT);
        if (body->valid & OBD_MD_FLTYPE)
                lli->lli_st_mode = (lli->lli_st_mode & ~S_IFMT)|(body->mode & S_IFMT);
        if (body->valid & OBD_MD_FLUID)
                lli->lli_st_uid = body->uid;
        if (body->valid & OBD_MD_FLGID)
                lli->lli_st_gid = body->gid;
        if (body->valid & OBD_MD_FLFLAGS)
                lli->lli_st_flags = body->flags;
        if (body->valid & OBD_MD_FLNLINK)
                lli->lli_st_nlink = body->nlink;
        if (body->valid & OBD_MD_FLRDEV)
                lli->lli_st_rdev = body->rdev;
        if (body->valid & OBD_MD_FLSIZE)
                lli->lli_st_size = body->size;
        if (body->valid & OBD_MD_FLBLOCKS)
                lli->lli_st_blocks = body->blocks;
}

void obdo_to_inode(struct inode *dst, struct obdo *src, obd_valid valid)
{
        struct llu_inode_info *lli = llu_i2info(dst);

        valid &= src->o_valid;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE, "valid "LPX64", cur time %lu/%lu, new %lu/%lu\n",
                       src->o_valid, 
                       LTIME_S(lli->lli_st_mtime), LTIME_S(lli->lli_st_ctime),
                       (long)src->o_mtime, (long)src->o_ctime);

        if (valid & OBD_MD_FLATIME)
                LTIME_S(lli->lli_st_atime) = src->o_atime;
        if (valid & OBD_MD_FLMTIME)
                LTIME_S(lli->lli_st_mtime) = src->o_mtime;
        if (valid & OBD_MD_FLCTIME && src->o_ctime > LTIME_S(lli->lli_st_ctime))
                LTIME_S(lli->lli_st_ctime) = src->o_ctime;
        if (valid & OBD_MD_FLSIZE)
                lli->lli_st_size = src->o_size;
        if (valid & OBD_MD_FLBLOCKS) /* allocation of space */
                lli->lli_st_blocks = src->o_blocks;
        if (valid & OBD_MD_FLBLKSZ)
                lli->lli_st_blksize = src->o_blksize;
        if (valid & OBD_MD_FLTYPE)
                lli->lli_st_mode = (lli->lli_st_mode & ~S_IFMT) | (src->o_mode & S_IFMT);
        if (valid & OBD_MD_FLMODE)
                lli->lli_st_mode = (lli->lli_st_mode & S_IFMT) | (src->o_mode & ~S_IFMT);
        if (valid & OBD_MD_FLUID)
                lli->lli_st_uid = src->o_uid;
        if (valid & OBD_MD_FLGID)
                lli->lli_st_gid = src->o_gid;
        if (valid & OBD_MD_FLFLAGS)
                lli->lli_st_flags = src->o_flags;
        if (valid & OBD_MD_FLGENER)
                lli->lli_st_generation = src->o_generation;
}

#define S_IRWXUGO       (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO       (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)

void obdo_from_inode(struct obdo *dst, struct inode *src, obd_valid valid)
{
        struct llu_inode_info *lli = llu_i2info(src);
        obd_valid newvalid = 0;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE, "valid "LPX64", new time %lu/%lu\n",
                       valid, LTIME_S(lli->lli_st_mtime), 
                       LTIME_S(lli->lli_st_ctime));

        if (valid & OBD_MD_FLATIME) {
                dst->o_atime = LTIME_S(lli->lli_st_atime);
                newvalid |= OBD_MD_FLATIME;
        }
        if (valid & OBD_MD_FLMTIME) {
                dst->o_mtime = LTIME_S(lli->lli_st_mtime);
                newvalid |= OBD_MD_FLMTIME;
        }
        if (valid & OBD_MD_FLCTIME) {
                dst->o_ctime = LTIME_S(lli->lli_st_ctime);
                newvalid |= OBD_MD_FLCTIME;
        }
        if (valid & OBD_MD_FLSIZE) {
                dst->o_size = lli->lli_st_size;
                newvalid |= OBD_MD_FLSIZE;
        }
        if (valid & OBD_MD_FLBLOCKS) {  /* allocation of space (x512 bytes) */
                dst->o_blocks = lli->lli_st_blocks;
                newvalid |= OBD_MD_FLBLOCKS;
        }
        if (valid & OBD_MD_FLBLKSZ) {   /* optimal block size */
                dst->o_blksize = lli->lli_st_blksize;
                newvalid |= OBD_MD_FLBLKSZ;
        }
        if (valid & OBD_MD_FLTYPE) {
                dst->o_mode = (dst->o_mode & S_IALLUGO)|(lli->lli_st_mode & S_IFMT);
                newvalid |= OBD_MD_FLTYPE;
        }
        if (valid & OBD_MD_FLMODE) {
                dst->o_mode = (dst->o_mode & S_IFMT)|(lli->lli_st_mode & S_IALLUGO);
                newvalid |= OBD_MD_FLMODE;
        }
        if (valid & OBD_MD_FLUID) {
                dst->o_uid = lli->lli_st_uid;
                newvalid |= OBD_MD_FLUID;
        }
        if (valid & OBD_MD_FLGID) {
                dst->o_gid = lli->lli_st_gid;
                newvalid |= OBD_MD_FLGID;
        }
        if (valid & OBD_MD_FLFLAGS) {
                dst->o_flags = lli->lli_st_flags;
                newvalid |= OBD_MD_FLFLAGS;
        }
        if (valid & OBD_MD_FLGENER) {
                dst->o_generation = lli->lli_st_generation;
                newvalid |= OBD_MD_FLGENER;
        }

        dst->o_valid |= newvalid;
}

/*
 * really does the getattr on the inode and updates its fields
 */
int llu_inode_getattr(struct inode *inode, struct lov_stripe_md *lsm)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        struct obd_export *exp = llu_i2dtexp(inode);
        struct ptlrpc_request_set *set;
        struct obdo oa;
        obd_valid refresh_valid;
        int rc;
        ENTRY;

        LASSERT(lsm);
        LASSERT(lli);

        memset(&oa, 0, sizeof oa);
        oa.o_id = lsm->lsm_object_id;
        oa.o_mode = S_IFREG;
        oa.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLSIZE |
                OBD_MD_FLBLOCKS | OBD_MD_FLBLKSZ | OBD_MD_FLMTIME |
                OBD_MD_FLCTIME;

        set = ptlrpc_prep_set();
        if (set == NULL) {
                CERROR ("ENOMEM allocing request set\n");
                rc = -ENOMEM;
        } else {
                rc = obd_getattr_async(exp, &oa, lsm, set);
                if (rc == 0)
                        rc = ptlrpc_set_wait(set);
                ptlrpc_set_destroy(set);
        }
        if (rc)
                RETURN(rc);

        refresh_valid = OBD_MD_FLBLOCKS | OBD_MD_FLBLKSZ | OBD_MD_FLMTIME | 
                        OBD_MD_FLCTIME | OBD_MD_FLSIZE;

        /* We set this flag in commit write as we extend the file size.  When
         * the bit is set and the lock is canceled that covers the file size,
         * we clear the bit.  This is enough to protect the window where our
         * local size extension is needed for writeback.  However, it relies on
         * behaviour that won't be true in the near future.  This assumes that
         * all getattr callers get extent locks, which they currnetly do.  It
         * also assumes that we only send discarding asts for {0,eof} truncates
         * as is currently the case.  This will have to be replaced by the
         * proper eoc communication between clients and the ost, which is on
         * its way. */
        if (test_bit(LLI_F_PREFER_EXTENDED_SIZE, &lli->lli_flags)) {
                if (oa.o_size < lli->lli_st_size)
                        refresh_valid &= ~OBD_MD_FLSIZE;
                else 
                        clear_bit(LLI_F_PREFER_EXTENDED_SIZE, &lli->lli_flags);
        }

        obdo_refresh_inode(inode, &oa, refresh_valid);

        RETURN(0);
}

static struct inode *llu_new_inode(struct filesys *fs,
                                   struct lustre_id *id)
{
	struct inode *inode;
        struct intnl_stat stat;
        struct llu_inode_info *lli;

        OBD_ALLOC(lli, sizeof(*lli));
        if (!lli)
                return NULL;

        /* initialize lli here */
        lli->lli_sbi = llu_fs2sbi(fs);
        lli->lli_smd = NULL;
        lli->lli_symlink_name = NULL;
        lli->lli_flags = 0;
        lli->lli_maxbytes = (__u64)(~0UL);
        lli->lli_file_data = NULL;

        lli->lli_sysio_fid.fid_data = &lli->lli_id;
        lli->lli_sysio_fid.fid_len = sizeof(lli->lli_id);

        memcpy(&lli->lli_id, id, sizeof(*id));

#warning "fill @stat by desired attributes of new inode before using_sysio_i_new()"
        memset(&stat, 0, sizeof(stat));
        stat.st_ino = id_ino(id);
        
        /* file identifier is needed by functions like _sysio_i_find() */
	inode = _sysio_i_new(fs, &lli->lli_sysio_fid,
                             &stat, 0, &llu_inode_ops, lli);

	if (!inode)
		OBD_FREE(lli, sizeof(*lli));

        return inode;
}

static int llu_have_md_lock(struct inode *inode, __u64 lockpart)
{
        struct llu_sb_info *sbi = llu_i2sbi(inode);
        struct llu_inode_info *lli = llu_i2info(inode);
        struct lustre_handle lockh;
        struct ldlm_res_id res_id = { .name = {0} };
        struct obd_device *obddev;
        ldlm_policy_data_t policy = { .l_inodebits = { lockpart } };
        int flags;
        ENTRY;

        LASSERT(inode);

        obddev = sbi->ll_md_exp->exp_obd;
        res_id.name[0] = id_fid(&lli->lli_id);
        res_id.name[1] = id_group(&lli->lli_id);

        CDEBUG(D_INFO, "trying to match res "LPU64"\n", res_id.name[0]);

        /* FIXME use LDLM_FL_TEST_LOCK instead */
        flags = LDLM_FL_BLOCK_GRANTED | LDLM_FL_CBPENDING;
        if (ldlm_lock_match(obddev->obd_namespace, flags, &res_id, LDLM_IBITS,
                            &policy, LCK_PR, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PR);
                RETURN(1);
        }

        if (ldlm_lock_match(obddev->obd_namespace, flags, &res_id, LDLM_IBITS,
                            &policy, LCK_PW, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PW);
                RETURN(1);
        }
        RETURN(0);
}

static int llu_inode_revalidate(struct inode *inode)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        struct lov_stripe_md *lsm = NULL;
        ENTRY;

        if (!inode) {
                CERROR("REPORT THIS LINE TO PETER\n");
                RETURN(0);
        }

        if (!llu_have_md_lock(inode, MDS_INODELOCK_UPDATE)) {
                struct lustre_md md;
                struct ptlrpc_request *req = NULL;
                struct llu_sb_info *sbi = llu_i2sbi(inode);
                struct lustre_id id;
                __u64 valid = 0;
                int rc, ealen = 0;

                /* Why don't we update all valid MDS fields here, if we're doing
                 * an RPC anyways?  -phil */
                if (S_ISREG(lli->lli_st_mode)) {
                        ealen = obd_size_diskmd(sbi->ll_dt_exp, NULL);
                        valid |= OBD_MD_FLEASIZE;
                }
                ll_inode2id(&id, inode);

                /* XXX: capa is NULL here, is it correct? */
                rc = mdc_getattr(sbi->ll_md_exp, &id, valid, NULL, NULL,
                                 0, ealen, NULL, &req);
                if (rc) {
                        CERROR("failure %d inode %lu\n", rc, lli->lli_st_ino);
                        RETURN(-abs(rc));
                }
                rc = mdc_req2lustre_md(sbi->ll_md_exp, req, 0, 
                                       sbi->ll_dt_exp, &md);

                /* XXX Too paranoid? */
                if (((md.body->valid ^ valid) & OBD_MD_FLEASIZE) &&
                    !((md.body->valid & OBD_MD_FLNLINK) &&
                      (md.body->nlink == 0))) {
                        CERROR("Asked for %s eadata but got %s (%d)\n",
                               (valid & OBD_MD_FLEASIZE) ? "some" : "no",
                               (md.body->valid & OBD_MD_FLEASIZE) ? "some":"none",
                                md.body->eadatasize);
                }
                if (rc) {
                        ptlrpc_req_finished(req);
                        RETURN(rc);
                }


                llu_update_inode(inode, md.body, md.lsm);
                if (md.lsm != NULL && llu_i2info(inode)->lli_smd != md.lsm)
                        obd_free_memmd(sbi->ll_dt_exp, &md.lsm);

                if (md.body->valid & OBD_MD_FLSIZE)
                        set_bit(LLI_F_HAVE_MDS_SIZE_LOCK,
                                &llu_i2info(inode)->lli_flags);
                ptlrpc_req_finished(req);
        }

        lsm = llu_i2info(inode)->lli_smd;
        if (!lsm)       /* object not yet allocated, don't validate size */
                RETURN(0);

        /* ll_glimpse_size will prefer locally cached writes if they extend
         * the file */
        RETURN(llu_glimpse_size(inode));
}

static void copy_stat_buf_lli(struct llu_inode_info *lli,
                              struct intnl_stat *b)
{
        b->st_dev = lli->lli_st_dev;
        b->st_ino = lli->lli_st_ino;
        b->st_mode = lli->lli_st_mode;
        b->st_nlink = lli->lli_st_nlink;
        b->st_uid = lli->lli_st_uid;
        b->st_gid = lli->lli_st_gid;
        b->st_rdev = lli->lli_st_rdev;
        b->st_size = lli->lli_st_size;
        b->st_blksize = lli->lli_st_blksize;
        b->st_blocks = lli->lli_st_blocks;
        b->st_atime = lli->lli_st_atime;
        b->st_mtime = lli->lli_st_mtime;
        b->st_ctime = lli->lli_st_ctime;
}

static void copy_stat_buf(struct inode *ino, struct intnl_stat *b)
{
        struct llu_inode_info *lli = llu_i2info(ino);
        copy_stat_buf_lli(lli, b);
}

static int llu_iop_getattr(struct pnode *pno,
                           struct inode *ino,
                           struct intnl_stat *b)
{
        int rc;
        ENTRY;

        liblustre_wait_event(0);

        if (!ino) {
                LASSERT(pno);
                LASSERT(pno->p_base->pb_ino);
                ino = pno->p_base->pb_ino;
        } else {
                LASSERT(!pno || pno->p_base->pb_ino == ino);
        }

        /* libsysio might call us directly without intent lock,
         * we must re-fetch the attrs here
         */
        rc = llu_inode_revalidate(ino);
        if (!rc) {
                copy_stat_buf(ino, b);
                LASSERT(!llu_i2info(ino)->lli_it);
        }

        RETURN(rc);
}

static int null_if_equal(struct ldlm_lock *lock, void *data)
{
        if (data == lock->l_ast_data) {
                lock->l_ast_data = NULL;

                if (lock->l_req_mode != lock->l_granted_mode)
                        LDLM_ERROR(lock,"clearing inode with ungranted lock\n");
        }

        return LDLM_ITER_CONTINUE;
}

void llu_clear_inode(struct inode *inode)
{
        struct lustre_id id;
        struct llu_inode_info *lli = llu_i2info(inode);
        struct llu_sb_info *sbi = llu_i2sbi(inode);
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%lu(%p)\n", lli->lli_st_ino,
               lli->lli_st_generation, inode);

        ll_inode2id(&id, inode);
        clear_bit(LLI_F_HAVE_MDS_SIZE_LOCK, &(lli->lli_flags));
        mdc_change_cbdata(sbi->ll_md_exp, &id, null_if_equal, inode);

        if (lli->lli_smd)
                obd_change_cbdata(sbi->ll_dt_exp, lli->lli_smd,
                                  null_if_equal, inode);

        if (lli->lli_smd) {
                obd_free_memmd(sbi->ll_dt_exp, &lli->lli_smd);
                lli->lli_smd = NULL;
        }

        if (lli->lli_symlink_name) {
                OBD_FREE(lli->lli_symlink_name,
                         strlen(lli->lli_symlink_name) + 1);
                lli->lli_symlink_name = NULL;
        }

        EXIT;
}

void llu_iop_gone(struct inode *inode)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        ENTRY;

        liblustre_wait_event(0);
        llu_clear_inode(inode);

        OBD_FREE(lli, sizeof(*lli));
        EXIT;
}

static int inode_setattr(struct inode * inode, struct iattr * attr)
{
        unsigned int ia_valid = attr->ia_valid;
        struct llu_inode_info *lli = llu_i2info(inode);
        int error = 0;

        if (ia_valid & ATTR_SIZE) {
                error = llu_vmtruncate(inode, attr->ia_size);
                if (error)
                        goto out;
        }

        if (ia_valid & ATTR_UID)
                lli->lli_st_uid = attr->ia_uid;
        if (ia_valid & ATTR_GID)
                lli->lli_st_gid = attr->ia_gid;
        if (ia_valid & ATTR_ATIME)
                lli->lli_st_atime = attr->ia_atime;
        if (ia_valid & ATTR_MTIME)
                lli->lli_st_mtime = attr->ia_mtime;
        if (ia_valid & ATTR_CTIME)
                lli->lli_st_ctime = attr->ia_ctime;
        if (ia_valid & ATTR_MODE) {
                lli->lli_st_mode = attr->ia_mode;
                if (!in_group_p(lli->lli_st_gid) && !capable(CAP_FSETID))
                        lli->lli_st_mode &= ~S_ISGID;
        }
        /* mark_inode_dirty(inode); */
out:
        return error;
}

/* If this inode has objects allocated to it (lsm != NULL), then the OST
 * object(s) determine the file size and mtime.  Otherwise, the MDS will
 * keep these values until such a time that objects are allocated for it.
 * We do the MDS operations first, as it is checking permissions for us.
 * We don't to the MDS RPC if there is nothing that we want to store there,
 * otherwise there is no harm in updating mtime/atime on the MDS if we are
 * going to do an RPC anyways.
 *
 * If we are doing a truncate, we will send the mtime and ctime updates
 * to the OST with the punch RPC, otherwise we do an explicit setattr RPC.
 * I don't believe it is possible to get e.g. ATTR_MTIME_SET and ATTR_SIZE
 * at the same time.
 */
int llu_setattr_raw(struct inode *inode, struct iattr *attr)
{
        struct lov_stripe_md *lsm = llu_i2info(inode)->lli_smd;
        struct llu_sb_info *sbi = llu_i2sbi(inode);
        struct llu_inode_info *lli = llu_i2info(inode);
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int ia_valid = attr->ia_valid;
        int rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu\n", lli->lli_st_ino);

        if (ia_valid & ATTR_SIZE) {
                if (attr->ia_size > ll_file_maxbytes(inode)) {
                        CDEBUG(D_INODE, "file too large %llu > "LPU64"\n",
                               attr->ia_size, ll_file_maxbytes(inode));
                        RETURN(-EFBIG);
                }

                attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
        }

        /* We mark all of the fields "set" so MDS/OST does not re-set them */
        if (attr->ia_valid & ATTR_CTIME) {
                attr->ia_ctime = CURRENT_TIME;
                attr->ia_valid |= ATTR_CTIME_SET;
        }
        if (!(ia_valid & ATTR_ATIME_SET) && (attr->ia_valid & ATTR_ATIME)) {
                attr->ia_atime = CURRENT_TIME;
                attr->ia_valid |= ATTR_ATIME_SET;
        }
        if (!(ia_valid & ATTR_MTIME_SET) && (attr->ia_valid & ATTR_MTIME)) {
                attr->ia_mtime = CURRENT_TIME;
                attr->ia_valid |= ATTR_MTIME_SET;
        }

        if (attr->ia_valid & (ATTR_MTIME | ATTR_CTIME))
                CDEBUG(D_INODE, "setting mtime %lu, ctime %lu, now = %lu\n",
                       LTIME_S(attr->ia_mtime), LTIME_S(attr->ia_ctime),
                       LTIME_S(CURRENT_TIME));
        if (lsm)
                attr->ia_valid &= ~ATTR_SIZE;

        /* If only OST attributes being set on objects, don't do MDS RPC.
         * In that case, we need to check permissions and update the local
         * inode ourselves so we can call obdo_from_inode() always. */
        if (ia_valid & (lsm ? ~(ATTR_SIZE | ATTR_FROM_OPEN | ATTR_RAW) : ~0)) {
                struct lustre_md md;
                llu_prepare_mdc_data(&op_data, inode, NULL, NULL, 0, 0);

                rc = mdc_setattr(sbi->ll_md_exp, &op_data,
                                 attr, NULL, 0, NULL, 0, NULL, 0, &request);
                
                if (rc) {
                        ptlrpc_req_finished(request);
                        if (rc != -EPERM && rc != -EACCES)
                                CERROR("mdc_setattr fails: rc = %d\n", rc);
                        RETURN(rc);
                }

                rc = mdc_req2lustre_md(sbi->ll_md_exp, request, 0, 
                                       sbi->ll_dt_exp, &md);
                if (rc) {
                        ptlrpc_req_finished(request);
                        RETURN(rc);
                }

                /* Won't invoke vmtruncate as we already cleared ATTR_SIZE,
                 * but needed to set timestamps backwards on utime. */
                inode_setattr(inode, attr);
                llu_update_inode(inode, md.body, md.lsm);
                ptlrpc_req_finished(request);

                if (!md.lsm || !S_ISREG(lli->lli_st_mode)) {
                        CDEBUG(D_INODE, "no lsm: not setting attrs on OST\n");
                        RETURN(0);
                }
        } else {
                /* The OST doesn't check permissions, but the alternative is
                 * a gratuitous RPC to the MDS.  We already rely on the client
                 * to do read/write/truncate permission checks, so is mtime OK?
                 */
                if (ia_valid & (ATTR_MTIME | ATTR_ATIME)) {
                        /* from sys_utime() */
                        if (!(ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET))) {
                                if (current->fsuid != lli->lli_st_uid &&
                                    (rc = ll_permission(inode, MAY_WRITE)) != 0)
                                        RETURN(rc);
                        } else {
				/* from inode_change_ok() */
				if (current->fsuid != lli->lli_st_uid &&
				    !capable(CAP_FOWNER))
					RETURN(-EPERM);
                        }
                }

                /* Won't invoke vmtruncate, as we already cleared ATTR_SIZE */
                inode_setattr(inode, attr);
        }

        if (ia_valid & ATTR_SIZE) {
                ldlm_policy_data_t policy = { .l_extent = {attr->ia_size,
                                                           OBD_OBJECT_EOF} };
                struct lustre_handle lockh = { 0 };
                int err, ast_flags = 0;
                /* XXX when we fix the AST intents to pass the discard-range
                 * XXX extent, make ast_flags always LDLM_AST_DISCARD_DATA
                 * XXX here. */
                if (attr->ia_size == 0)
                        ast_flags = LDLM_AST_DISCARD_DATA;

                rc = llu_extent_lock(NULL, inode, lsm, LCK_PW, &policy,
                                     &lockh, ast_flags);
                if (rc != ELDLM_OK) {
                        if (rc > 0)
                                RETURN(-ENOLCK);
                        RETURN(rc);
                }

                rc = llu_vmtruncate(inode, attr->ia_size);

                /* unlock now as we don't mind others file lockers racing with
                 * the mds updates below? */
                err = llu_extent_unlock(NULL, inode, lsm, LCK_PW, &lockh);
                if (err) {
                        CERROR("llu_extent_unlock failed: %d\n", err);
                        if (!rc)
                                rc = err;
                }
        } else if (ia_valid & (ATTR_MTIME | ATTR_MTIME_SET)) {
                struct obdo oa;

                CDEBUG(D_INODE, "set mtime on OST inode %lu to %lu\n",
                       lli->lli_st_ino, LTIME_S(attr->ia_mtime));
                oa.o_id = lsm->lsm_object_id;
                oa.o_valid = OBD_MD_FLID;
                obdo_from_inode(&oa, inode, OBD_MD_FLTYPE | OBD_MD_FLATIME |
                                            OBD_MD_FLMTIME | OBD_MD_FLCTIME);
                rc = obd_setattr(sbi->ll_dt_exp, &oa, lsm, NULL);
                if (rc)
                        CERROR("obd_setattr fails: rc=%d\n", rc);
        }
        RETURN(rc);
}

/* here we simply act as a thin layer to glue it with
 * llu_setattr_raw(), which is copy from kernel
 */
static int llu_iop_setattr(struct pnode *pno,
                           struct inode *ino,
                           unsigned mask,
                           struct intnl_stat *stbuf)
{
        struct iattr iattr;
        ENTRY;

        liblustre_wait_event(0);

        LASSERT(!(mask & ~(SETATTR_MTIME | SETATTR_ATIME | 
                           SETATTR_UID | SETATTR_GID |
                           SETATTR_LEN | SETATTR_MODE)));
        memset(&iattr, 0, sizeof(iattr));

        if (mask & SETATTR_MODE) {
                iattr.ia_mode = stbuf->st_mode;
                iattr.ia_valid |= ATTR_MODE;
        }
        if (mask & SETATTR_MTIME) {
                iattr.ia_mtime = stbuf->st_mtime;
                iattr.ia_valid |= ATTR_MTIME;
        }
        if (mask & SETATTR_ATIME) {
                iattr.ia_atime = stbuf->st_atime;
                iattr.ia_valid |= ATTR_ATIME;
        }
        if (mask & SETATTR_UID) {
                iattr.ia_uid = stbuf->st_uid;
                iattr.ia_valid |= ATTR_UID;
        }
        if (mask & SETATTR_GID) {
                iattr.ia_gid = stbuf->st_gid;
                iattr.ia_valid |= ATTR_GID;
        }
        if (mask & SETATTR_LEN) {
                iattr.ia_size = stbuf->st_size; /* XXX signed expansion problem */
                iattr.ia_valid |= ATTR_SIZE;
        }

        iattr.ia_valid |= ATTR_RAW;

        RETURN(llu_setattr_raw(ino, &iattr));
}

#define EXT2_LINK_MAX           32000

static int llu_iop_symlink_raw(struct pnode *pno, const char *tgt)
{
        struct inode *dir = pno->p_base->pb_parent->pb_ino;
        struct qstr *qstr = &pno->p_base->pb_name;
        const char *name = qstr->name;
        int len = qstr->len;
        struct ptlrpc_request *request = NULL;
        struct llu_sb_info *sbi = llu_i2sbi(dir);
        struct mdc_op_data op_data;
        int err = -EMLINK;
        ENTRY;

        if (llu_i2info(dir)->lli_st_nlink >= EXT2_LINK_MAX)
                RETURN(err);

        llu_prepare_mdc_data(&op_data, dir, NULL, name, len, 0);
        err = mdc_create(sbi->ll_md_exp, &op_data,
                         tgt, strlen(tgt) + 1, S_IFLNK | S_IRWXUGO,
                         current->fsuid, current->fsgid, 0, &request);
        ptlrpc_req_finished(request);
        RETURN(err);
}

static int llu_readlink_internal(struct inode *inode,
                                 struct ptlrpc_request **request,
                                 char **symname)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        struct llu_sb_info *sbi = llu_i2sbi(inode);
        struct lustre_id id;
        struct mds_body *body;
        int rc, symlen = lli->lli_st_size + 1;
        ENTRY;

        *request = NULL;

        if (lli->lli_symlink_name) {
                *symname = lli->lli_symlink_name;
                CDEBUG(D_INODE, "using cached symlink %s\n", *symname);
                RETURN(0);
        }

        ll_inode2id(&id, inode);

        /* XXX: capa is NULL here, is it correct? */
        rc = mdc_getattr(sbi->ll_md_exp, &id, OBD_MD_LINKNAME, NULL, 0,
                         0, symlen, NULL, request);
        if (rc) {
                CERROR("inode %lu: rc = %d\n", lli->lli_st_ino, rc);
                RETURN(rc);
        }

        body = lustre_msg_buf ((*request)->rq_repmsg, 0, sizeof (*body));
        LASSERT (body != NULL);
        LASSERT_REPSWABBED (*request, 0);

        if ((body->valid & OBD_MD_LINKNAME) == 0) {
                CERROR ("OBD_MD_LINKNAME not set on reply\n");
                GOTO (failed, rc = -EPROTO);
        }
        
        LASSERT (symlen != 0);
        if (body->eadatasize != symlen) {
                CERROR ("inode %lu: symlink length %d not expected %d\n",
                        lli->lli_st_ino, body->eadatasize - 1, symlen - 1);
                GOTO (failed, rc = -EPROTO);
        }

        *symname = lustre_msg_buf ((*request)->rq_repmsg, 1, symlen);
        if (*symname == NULL ||
            strnlen (*symname, symlen) != symlen - 1) {
                /* not full/NULL terminated */
                CERROR ("inode %lu: symlink not NULL terminated string"
                        "of length %d\n", lli->lli_st_ino, symlen - 1);
                GOTO (failed, rc = -EPROTO);
        }

        OBD_ALLOC(lli->lli_symlink_name, symlen);
        /* do not return an error if we cannot cache the symlink locally */
        if (lli->lli_symlink_name)
                memcpy(lli->lli_symlink_name, *symname, symlen);

        RETURN(0);

 failed:
        ptlrpc_req_finished (*request);
        RETURN (-EPROTO);
}

static int llu_iop_readlink(struct pnode *pno, char *data, size_t bufsize)
{
        struct inode *inode = pno->p_base->pb_ino;
        struct ptlrpc_request *request;
        char *symname;
        int rc;
        ENTRY;

        rc = llu_readlink_internal(inode, &request, &symname);
        if (rc)
                GOTO(out, rc);

        LASSERT(symname);
        strncpy(data, symname, bufsize);

        ptlrpc_req_finished(request);
 out:
        RETURN(rc);
}

static int llu_iop_mknod_raw(struct pnode *pno,
                             mode_t mode,
                             dev_t dev)
{
        struct ptlrpc_request *request = NULL;
        struct inode *dir = pno->p_parent->p_base->pb_ino;
        struct llu_sb_info *sbi = llu_i2sbi(dir);
        struct mdc_op_data op_data;
        int err = -EMLINK;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu\n",
               (int)pno->p_base->pb_name.len, pno->p_base->pb_name.name,
               llu_i2info(dir)->lli_st_ino);

        if (llu_i2info(dir)->lli_st_nlink >= EXT2_LINK_MAX)
                RETURN(err);

        mode &= ~current->fs->umask;

        switch (mode & S_IFMT) {
        case 0:
        case S_IFREG:
                mode |= S_IFREG; /* for mode = 0 case, fallthrough */
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                llu_prepare_mdc_data(&op_data, dir, NULL,
                                     pno->p_base->pb_name.name,
                                     pno->p_base->pb_name.len,
                                     0);
                err = mdc_create(sbi->ll_md_exp, &op_data, NULL, 0, mode,
                                 current->fsuid, current->fsgid, dev, &request);
                ptlrpc_req_finished(request);
                break;
        case S_IFDIR:
                err = -EPERM;
                break;
        default:
                err = -EINVAL;
        }
        RETURN(err);
}

static int llu_iop_link_raw(struct pnode *old, struct pnode *new)
{
        struct inode *src = old->p_base->pb_ino;
        struct inode *dir = new->p_parent->p_base->pb_ino;
        const char *name = new->p_base->pb_name.name;
        int namelen = new->p_base->pb_name.len;
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int rc;
        ENTRY;

        LASSERT(src);
        LASSERT(dir);

        liblustre_wait_event(0);
        llu_prepare_mdc_data(&op_data, src, dir, name, namelen, 0);
        rc = mdc_link(llu_i2sbi(src)->ll_md_exp, &op_data, &request);
        ptlrpc_req_finished(request);
        liblustre_wait_event(0);

        RETURN(rc);
}

/*
 * libsysio will clear the inode immediately after return
 */
static int llu_iop_unlink_raw(struct pnode *pno)
{
        struct inode *dir = pno->p_base->pb_parent->pb_ino;
        struct qstr *qstr = &pno->p_base->pb_name;
        const char *name = qstr->name;
        int len = qstr->len;
        struct inode *target = pno->p_base->pb_ino;
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int rc;
        ENTRY;

        LASSERT(target);

        liblustre_wait_event(0);
        llu_prepare_mdc_data(&op_data, dir, NULL, name, len, 0);
        rc = mdc_unlink(llu_i2sbi(dir)->ll_md_exp, &op_data, &request);
        if (!rc)
                rc = llu_objects_destroy(request, dir);
        ptlrpc_req_finished(request);
        liblustre_wait_event(0);
        RETURN(rc);
}

static int llu_iop_rename_raw(struct pnode *old, struct pnode *new)
{
        struct inode *src = old->p_parent->p_base->pb_ino;
        struct inode *tgt = new->p_parent->p_base->pb_ino;
        const char *oldname = old->p_base->pb_name.name;
        int oldnamelen = old->p_base->pb_name.len;
        const char *newname = new->p_base->pb_name.name;
        int newnamelen = new->p_base->pb_name.len;
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int rc;
        ENTRY;

        LASSERT(src);
        LASSERT(tgt);

        llu_prepare_mdc_data(&op_data, src, tgt, NULL, 0, 0);
        rc = mdc_rename(llu_i2sbi(src)->ll_md_exp, &op_data,
                        oldname, oldnamelen, newname, newnamelen,
                        &request);
        if (!rc) {
                rc = llu_objects_destroy(request, src);
        }

        ptlrpc_req_finished(request);

        RETURN(rc);
}

#ifdef _HAVE_STATVFS
static int llu_statfs_internal(struct llu_sb_info *sbi,
                               struct obd_statfs *osfs,
                               unsigned long max_age)
{
        struct obd_statfs obd_osfs;
        int rc;
        ENTRY;

        rc = obd_statfs(class_exp2obd(sbi->ll_md_exp), osfs, max_age);
        if (rc) {
                CERROR("mdc_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        CDEBUG(D_SUPER, "MDC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               osfs->os_bavail, osfs->os_blocks, osfs->os_ffree,osfs->os_files);

        rc = obd_statfs(class_exp2obd(sbi->ll_dt_exp), &obd_osfs, max_age);
        if (rc) {
                CERROR("obd_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        CDEBUG(D_SUPER, "OSC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               obd_osfs.os_bavail, obd_osfs.os_blocks, obd_osfs.os_ffree,
               obd_osfs.os_files);

        osfs->os_blocks = obd_osfs.os_blocks;
        osfs->os_bfree = obd_osfs.os_bfree;
        osfs->os_bavail = obd_osfs.os_bavail;

        /* If we don't have as many objects free on the OST as inodes
         * on the MDS, we reduce the total number of inodes to
         * compensate, so that the "inodes in use" number is correct.
         */
        if (obd_osfs.os_ffree < osfs->os_ffree) {
                osfs->os_files = (osfs->os_files - osfs->os_ffree) +
                        obd_osfs.os_ffree;
                osfs->os_ffree = obd_osfs.os_ffree;
        }

        RETURN(rc);
}

static int llu_statfs(struct llu_sb_info *sbi, struct statfs *sfs)
{
        struct obd_statfs osfs;
        int rc;

        CDEBUG(D_VFSTRACE, "VFS Op:\n");

        /* For now we will always get up-to-date statfs values, but in the
         * future we may allow some amount of caching on the client (e.g.
         * from QOS or lprocfs updates). */
        rc = llu_statfs_internal(sbi, &osfs, jiffies - 1);
        if (rc)
                return rc;

        statfs_unpack(sfs, &osfs);

        if (sizeof(sfs->f_blocks) == 4) {
                while (osfs.os_blocks > ~0UL) {
                        sfs->f_bsize <<= 1;

                        osfs.os_blocks >>= 1;
                        osfs.os_bfree >>= 1;
                        osfs.os_bavail >>= 1;
                }
        }

        sfs->f_blocks = osfs.os_blocks;
        sfs->f_bfree = osfs.os_bfree;
        sfs->f_bavail = osfs.os_bavail;

        return 0;
}

static int llu_iop_statvfs(struct pnode *pno,
                           struct inode *ino,
                           struct intnl_statvfs *buf)
{
        struct statfs fs;
        int rc;
        ENTRY;

        liblustre_wait_event(0);

#ifndef __CYGWIN__
        LASSERT(pno->p_base->pb_ino);
        rc = llu_statfs(llu_i2sbi(pno->p_base->pb_ino), &fs);
        if (rc)
                RETURN(rc);

        /* from native driver */
        buf->f_bsize = fs.f_bsize;  /* file system block size */
        buf->f_frsize = fs.f_bsize; /* file system fundamental block size */
        buf->f_blocks = fs.f_blocks;
        buf->f_bfree = fs.f_bfree;
        buf->f_bavail = fs.f_bavail;
        buf->f_files = fs.f_files;  /* Total number serial numbers */
        buf->f_ffree = fs.f_ffree;  /* Number free serial numbers */
        buf->f_favail = fs.f_ffree; /* Number free ser num for non-privileged*/
        buf->f_fsid = fs.f_fstc.__val[1];
        buf->f_flag = 0;            /* No equiv in statfs; maybe use type? */
        buf->f_namemax = fs.f_namelen;
#endif

        RETURN(0);
}
#endif /* _HAVE_STATVFS */

static int llu_iop_mkdir_raw(struct pnode *pno, mode_t mode)
{
        struct inode *dir = pno->p_base->pb_parent->pb_ino;
        struct qstr *qstr = &pno->p_base->pb_name;
        const char *name = qstr->name;
        int len = qstr->len;
        struct ptlrpc_request *request = NULL;
        struct llu_inode_info *lli = llu_i2info(dir);
        struct mdc_op_data op_data;
        int err = -EMLINK;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%lu(%p)\n",
               len, name, lli->lli_st_ino, lli->lli_st_generation, dir);

        if (lli->lli_st_nlink >= EXT2_LINK_MAX)
                RETURN(err);

        mode = (mode & (S_IRWXUGO|S_ISVTX) & ~current->fs->umask) | S_IFDIR;
        llu_prepare_mdc_data(&op_data, dir, NULL, name, len, 0);
        err = mdc_create(llu_i2sbi(dir)->ll_md_exp, &op_data, NULL, 0, mode,
                         current->fsuid, current->fsgid, 0, &request);
        ptlrpc_req_finished(request);
        RETURN(err);
}

static int llu_iop_rmdir_raw(struct pnode *pno)
{
        struct inode *dir = pno->p_base->pb_parent->pb_ino;
        struct qstr *qstr = &pno->p_base->pb_name;
        const char *name = qstr->name;
        int len = qstr->len;
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        struct llu_inode_info *lli = llu_i2info(dir);
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%lu(%p)\n",
               len, name, lli->lli_st_ino, lli->lli_st_generation, dir);

        llu_prepare_mdc_data(&op_data, dir, NULL, name, len, S_IFDIR);
        rc = mdc_unlink(llu_i2sbi(dir)->ll_md_exp, &op_data, &request);
        ptlrpc_req_finished(request);

        RETURN(rc);
}

#ifdef O_DIRECT
#define FCNTL_FLMASK (O_APPEND|O_NONBLOCK|O_ASYNC|O_DIRECT)
#else
#define FCNTL_FLMASK (O_APPEND|O_NONBLOCK|O_ASYNC)
#endif
#define FCNTL_FLMASK_INVALID (O_NONBLOCK|O_ASYNC)

static int llu_iop_fcntl(struct inode *ino, int cmd, va_list ap, int *rtn)
{
        struct llu_inode_info *lli = llu_i2info(ino);
        long flags;

        switch (cmd) {
        case F_GETFL:
                *rtn = lli->lli_open_flags;
                return 0;
        case F_SETFL:
                flags = va_arg(ap, long);
                flags &= FCNTL_FLMASK;
                if (flags & FCNTL_FLMASK_INVALID) {
                        CERROR("liblustre don't support O_NONBLOCK, O_ASYNC, "
                               "and O_DIRECT on file descriptor\n");
                        *rtn = -1;
                        return EINVAL;
                }
                lli->lli_open_flags = (int) flags;
                *rtn = 0;
                return 0;
        }

        CERROR("unsupported fcntl cmd %x\n", cmd);
        *rtn = -1;
        return ENOSYS;
}

static int llu_get_grouplock(struct inode *inode, unsigned long arg)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        struct ll_file_data *fd = lli->lli_file_data;
        ldlm_policy_data_t policy = { .l_extent = { .start = 0,
                                                    .end = OBD_OBJECT_EOF}};
        struct lustre_handle lockh = { 0 };
        struct lov_stripe_md *lsm = lli->lli_smd;
        ldlm_error_t err;
        int flags = 0;
        ENTRY;

        if (fd->fd_flags & LL_FILE_GROUP_LOCKED) {
                RETURN(-EINVAL);
        }

        policy.l_extent.gid = arg;
        if (lli->lli_open_flags & O_NONBLOCK)
                flags = LDLM_FL_BLOCK_NOWAIT;

        err = llu_extent_lock(fd, inode, lsm, LCK_GROUP, &policy, &lockh,
                              flags);
        if (err)
                RETURN(err);

        fd->fd_flags |= LL_FILE_GROUP_LOCKED|LL_FILE_IGNORE_LOCK;
        fd->fd_gid = arg;
        memcpy(&fd->fd_cwlockh, &lockh, sizeof(lockh));

        RETURN(0);
}

static int llu_put_grouplock(struct inode *inode, unsigned long arg)
{
        struct llu_inode_info *lli = llu_i2info(inode);
        struct ll_file_data *fd = lli->lli_file_data;
        struct lov_stripe_md *lsm = lli->lli_smd;
        ldlm_error_t err;
        ENTRY;

        if (!(fd->fd_flags & LL_FILE_GROUP_LOCKED))
                RETURN(-EINVAL);

        if (fd->fd_gid != arg)
                RETURN(-EINVAL);

        fd->fd_flags &= ~(LL_FILE_GROUP_LOCKED|LL_FILE_IGNORE_LOCK);

        err = llu_extent_unlock(fd, inode, lsm, LCK_GROUP, &fd->fd_cwlockh);
        if (err)
                RETURN(err);

        fd->fd_gid = 0;
        memset(&fd->fd_cwlockh, 0, sizeof(fd->fd_cwlockh));

        RETURN(0);
}       

static int llu_iop_ioctl(struct inode *ino, unsigned long int request,
                         va_list ap)
{
        unsigned long arg;

        liblustre_wait_event(0);

        switch (request) {
        case LL_IOC_GROUP_LOCK:
                arg = va_arg(ap, unsigned long);
                return llu_get_grouplock(ino, arg);
        case LL_IOC_GROUP_UNLOCK:
                arg = va_arg(ap, unsigned long);
                return llu_put_grouplock(ino, arg);
        }

        CERROR("did not support ioctl cmd %lx\n", request);
        return -ENOSYS;
}

/*
 * we already do syncronous read/write
 */
static int llu_iop_sync(struct inode *inode)
{
        liblustre_wait_event(0);
        return 0;
}

static int llu_iop_datasync(struct inode *inode)
{
        liblustre_wait_event(0);
        return 0;
}

struct filesys_ops llu_filesys_ops =
{
        fsop_gone: llu_fsop_gone,
};

struct inode *llu_iget(struct filesys *fs, struct lustre_md *md)
{
        struct inode *inode;
        struct lustre_id id;
        struct file_identifier fileid = {&id, sizeof(id)};

        if ((md->body->valid &
             (OBD_MD_FLGENER | OBD_MD_FLID | OBD_MD_FLTYPE)) !=
            (OBD_MD_FLGENER | OBD_MD_FLID | OBD_MD_FLTYPE)) {
                CERROR("bad md body valid mask 0x"LPX64"\n", 
		       md->body->valid);
                LBUG();
                return ERR_PTR(-EPERM);
        }

        id = md->body->id1;

        /* try to find existing inode */
        inode = _sysio_i_find(fs, &fileid);
        if (inode) {
                struct llu_inode_info *lli = llu_i2info(inode);

                if (inode->i_zombie ||
                    lli->lli_st_generation != id_gen(&md->body->id1)) {
                        I_RELE(inode);
                }
                else {
                        llu_update_inode(inode, md->body, md->lsm);
                        return inode;
                }
        }

        inode = llu_new_inode(fs, &id);
        if (inode)
                llu_update_inode(inode, md->body, md->lsm);
        
        return inode;
}

static int
llu_fsswop_mount(const char *source,
                 unsigned flags,
                 const void *data __IS_UNUSED,
                 struct pnode *tocover,
                 struct mount **mntp)
{
        struct filesys *fs;
        struct inode *root;
        struct pnode_base *rootpb;
        struct obd_device *obd;
        struct lustre_id rootid;
        struct llu_sb_info *sbi;
        struct obd_statfs osfs;
        static struct qstr noname = { NULL, 0, 0 };
        struct ptlrpc_request *request = NULL;
        struct lustre_handle lmv_conn = {0, };
        struct lustre_handle lov_conn = {0, };
        struct lustre_md md;
        class_uuid_t uuid;
        struct config_llog_instance cfg;
        struct lustre_profile *lprof;
        char *lov = NULL, *lmv = NULL;
        int async = 1, err = -EINVAL;

        ENTRY;

        /* allocate & initialize sbi */
        OBD_ALLOC(sbi, sizeof(*sbi));
        if (!sbi)
                RETURN(-ENOMEM);

        INIT_LIST_HEAD(&sbi->ll_conn_chain);
        generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &sbi->ll_sb_uuid);

        /* generate a string unique to this super, let's try
         the address of the super itself.*/
        OBD_ALLOC(sbi->ll_instance, sizeof(sbi) * 2 + 1);
        if (sbi->ll_instance == NULL) 
                GOTO(out_free, err = -ENOMEM);
        sprintf(sbi->ll_instance, "%p", sbi);

        /* retrive & parse config log */
        cfg.cfg_instance = sbi->ll_instance;
        cfg.cfg_uuid = sbi->ll_sb_uuid;
        err = liblustre_process_log(&cfg, 1);
        if (err < 0) {
                CERROR("Unable to process log: %s\n", g_zconf_profile);
                GOTO(out_free, err);
        }

        lprof = class_get_profile(g_zconf_profile);
        if (lprof == NULL) {
                CERROR("No profile found: %s\n", g_zconf_profile);
                GOTO(out_free, err = -EINVAL);
        }
        if (lov)
                OBD_FREE(lov, strlen(lov) + 1);
        OBD_ALLOC(lov, strlen(lprof->lp_lov) + 
                  strlen(sbi->ll_instance) + 2);
        sprintf(lov, "%s-%s", lprof->lp_lov, sbi->ll_instance);

        if (lmv)
                OBD_FREE(lmv, strlen(lmv) + 1);
        OBD_ALLOC(lmv, strlen(lprof->lp_lmv) + 
                  strlen(sbi->ll_instance) + 2);
        sprintf(lmv, "%s-%s", lprof->lp_lmv, sbi->ll_instance);

        if (!lov) {
                CERROR("no osc\n");
                GOTO(out_free, err = -EINVAL);
        }
        if (!lmv) {
                CERROR("no mdc\n");
                GOTO(out_free, err = -EINVAL);
        }

        fs = _sysio_fs_new(&llu_filesys_ops, flags, sbi);
        if (!fs) {
                err = -ENOMEM;
                goto out_free;
        }

        obd = class_name2obd(lmv);
        if (!obd) {
                CERROR("MDC %s: not setup or attached\n", lmv);
                GOTO(out_free, err = -EINVAL);
        }
        obd_set_info(obd->obd_self_export, strlen("async"), "async",
                     sizeof(async), &async);
#if 0
        if (mdc_init_ea_size(obd, osc))
                GOTO(out_free, err = -EINVAL);
#endif
        /* setup mdc */
        err = obd_connect(&lmv_conn, obd, &sbi->ll_sb_uuid, NULL, 0);
        if (err) {
                CERROR("cannot connect to %s: rc = %d\n", lmv, err);
                GOTO(out_free, err);
        }
        sbi->ll_md_exp = class_conn2export(&lmv_conn);

        err = obd_statfs(obd, &osfs, 100000000);
        if (err)
                GOTO(out_lmv, err);

        /*
         * FIXME fill fs stat data into sbi here!!! FIXME
         */

        /* setup lov */
        obd = class_name2obd(lov);
        if (!obd) {
                CERROR("OSC %s: not setup or attached\n", lov);
                GOTO(out_lmv, err = -EINVAL);
        }
        obd_set_info(obd->obd_self_export, strlen("async"), "async",
                     sizeof(async), &async);

        err = obd_connect(&lov_conn, obd, &sbi->ll_sb_uuid, NULL, 0);
        if (err) {
                CERROR("cannot connect to %s: rc = %d\n", lov, err);
                GOTO(out_lmv, err);
        }
        sbi->ll_dt_exp = class_conn2export(&lov_conn);

        err = mdc_getstatus(sbi->ll_md_exp, &rootid);
        if (err) {
                CERROR("cannot mds_connect: rc = %d\n", err);
                GOTO(out_lov, err);
        }
        CDEBUG(D_SUPER, "rootid "LPU64"\n", rootid.li_stc.u.e3s.l3s_ino);
        sbi->ll_rootino = rootid.li_stc.u.e3s.l3s_ino;

        /* XXX: capa is NULL here, is it correct? */
        err = mdc_getattr(sbi->ll_md_exp, &rootid,
                          (OBD_MD_FLNOTOBD | OBD_MD_FLBLOCKS), NULL, 0,
                          0, 0, NULL, &request);
        if (err) {
                CERROR("mdc_getattr failed for root: rc = %d\n", err);
                GOTO(out_lov, err);
        }

        err = mdc_req2lustre_md(sbi->ll_md_exp, request, 0, 
                                sbi->ll_dt_exp, &md);
        if (err) {
                CERROR("failed to understand root inode md: rc = %d\n",err);
                GOTO(out_request, err);
        }

        LASSERT(sbi->ll_rootino != 0);

        root = llu_iget(fs, &md);
        if (!root || IS_ERR(root)) {
                CERROR("fail to generate root inode\n");
                GOTO(out_request, err = -EBADF);
        }

	/*
	 * Generate base path-node for root.
	 */
	rootpb = _sysio_pb_new(&noname, NULL, root);
	if (!rootpb) {
		err = -ENOMEM;
		goto out_inode;
	}

	err = _sysio_do_mount(fs, rootpb, flags, tocover, mntp);
	if (err) {
                _sysio_pb_gone(rootpb);
		goto out_inode;
        }

        ptlrpc_req_finished(request);

        printf("LibLustre: namespace mounted successfully!\n");

        return 0;

out_inode:
        _sysio_i_gone(root);
out_request:
        ptlrpc_req_finished(request);
out_lov:
        obd_disconnect(sbi->ll_dt_exp, 0);
out_lmv:
        obd_disconnect(sbi->ll_md_exp, 0);
out_free:
        OBD_FREE(sbi, sizeof(*sbi));
        return err;
}

struct fssw_ops llu_fssw_ops = {
        llu_fsswop_mount
};

struct inode_ops llu_inode_ops = {
        .inop_lookup         = llu_iop_lookup,
        .inop_getattr        = llu_iop_getattr,
        .inop_setattr        = llu_iop_setattr,
        .inop_getdirentries  = llu_iop_getdirentries,
        .inop_mkdir          = llu_iop_mkdir_raw,
        .inop_rmdir          = llu_iop_rmdir_raw,
        .inop_symlink        = llu_iop_symlink_raw,
        .inop_readlink       = llu_iop_readlink,
        .inop_open           = llu_iop_open,
        .inop_close          = llu_iop_close,
        .inop_link           = llu_iop_link_raw,
        .inop_unlink         = llu_iop_unlink_raw,
        .inop_rename         = llu_iop_rename_raw,
        .inop_iodone         = llu_iop_iodone,
        .inop_fcntl          = llu_iop_fcntl,
        .inop_sync           = llu_iop_sync,
        .inop_read           = llu_iop_read,
        .inop_write          = llu_iop_write,
        .inop_datasync       = llu_iop_datasync,
        .inop_ioctl          = llu_iop_ioctl,
        .inop_mknod          = llu_iop_mknod_raw,
#ifdef _HAVE_STATVFS
        .inop_statvfs        = llu_iop_statvfs,
#endif
        .inop_gone           = llu_iop_gone,
};
